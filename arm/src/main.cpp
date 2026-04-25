/*
 * a2065d — A2065 Ethernet emulation daemon for Minimig MiSTer
 *
 * Usage: a2065d [--iface IFACE] [--bridge-base ADDR] [--verbose] [--sim]
 *
 * --iface      Network interface for AF_PACKET (default: eth0)
 * --bridge-base Physical base address of HPS2FPGA bridge (hex, default: from a2065_bridge.h)
 * --verbose    Enable packet logging (matches a2065.cpp log_a2065 > 0)
 * --sim        Use POSIX shared memory instead of /dev/mem (for dev machine testing)
 */

#include "a2065_types.h"
#include "a2065_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>

/* External function declarations */
extern void registers_reset(void);
extern void registers_set_boardram(volatile uint8_t *ram);
extern void registers_set_fakemac(const uint8_t *mac);
extern void registers_set_on_interrupt(void (*fn)(void));
extern void registers_set_on_transmit(void (*fn)(void));
extern uint16_t chip_wget(uint8_t reg_offset);
extern void chip_wput(uint8_t reg_offset, uint16_t v);
extern void do_transmit(void);
extern void gotfunc(const uint8_t *data, int len);
extern void mac_set_addresses(const uint8_t *fake, const uint8_t *real);
extern int  ethernet_open(const char *iface, int promiscuous);
extern void ethernet_close(void);
extern void ethernet_send(const uint8_t *frame, int len);
extern int  ethernet_recv(uint8_t *buf, int maxlen);  /* blocking */
extern void ethernet_get_mac(uint8_t *mac_out);

static volatile sig_atomic_t running = 1;
static volatile uint8_t *bridge = NULL;
static int verbose = 0;

/* ── Signal handler ─────────────────────────────────────────────────── */
static void handle_signal(int sig)
{
    (void)sig;
    running = 0;
}

/* ── Interrupt callback (called from registers.cpp when INTR+INEA) ──── */
static void on_interrupt_cb(void)
{
    if (!bridge) return;
    /* Signal FPGA to assert INT2 via bridge interrupt register */
    BRIDGE_WRITE8(bridge, BRIDGE_INT_SET, 1);
}

/* ── Transmit demand callback ────────────────────────────────────────── */
static void on_transmit_cb(void)
{
    do_transmit();
}

/* ── RX thread — blocks on AF_PACKET recvfrom ────────────────────────── */
static void *rx_thread(void *arg)
{
    (void)arg;
    static uint8_t rxbuf[MAX_PACKET_SIZE];
    while (running) {
        int len = ethernet_recv(rxbuf, sizeof rxbuf);
        if (len > 0)
            gotfunc(rxbuf, len);
    }
    return NULL;
}

/* ── Bridge register service loop ────────────────────────────────────── */
static void service_bridge(void)
{
    if (!BRIDGE_READ8(bridge, BRIDGE_REG_NEW_REQ))
        return;

    uint8_t  addr_off = BRIDGE_READ8(bridge,  BRIDGE_REG_ADDR);
    uint8_t  rw       = BRIDGE_READ8(bridge,  BRIDGE_REG_RW);
    uint16_t data     = BRIDGE_READ16(bridge, BRIDGE_REG_DATA);

    uint16_t result = 0;
    if (rw) {
        /* Write from 68k */
        chip_wput(addr_off, data);
    } else {
        /* Read from 68k */
        result = chip_wget(addr_off);
    }

    BRIDGE_WRITE16(bridge, BRIDGE_REG_RESULT, result);
    __sync_synchronize();
    BRIDGE_WRITE8(bridge,  BRIDGE_REG_DONE, 1);
}

/* ── Main ─────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    const char *iface       = "eth1";
    uint32_t    bridge_base = BRIDGE_PHYS_BASE;
    int         sim_mode    = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--iface") && i + 1 < argc)
            iface = argv[++i];
        else if (!strcmp(argv[i], "--bridge-base") && i + 1 < argc)
            bridge_base = (uint32_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--verbose"))
            verbose = 1;
        else if (!strcmp(argv[i], "--sim"))
            sim_mode = 1;
    }

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    /* TODO Step 5: implement bridge_open_sim() for --sim mode */
    /* TODO Step 9: implement bridge_open() for real /dev/mem */
    fprintf(stderr, "[a2065d] Starting: iface=%s bridge_base=0x%08X %s\n",
            iface, bridge_base, sim_mode ? "(SIM)" : "");

    /* Get host NIC MAC, build fakemac with Commodore OUI */
    uint8_t realmac[6], fakemac[6];
    ethernet_get_mac(realmac);
    fakemac[0] = COMMODORE_OUI0;
    fakemac[1] = COMMODORE_OUI1;
    fakemac[2] = COMMODORE_OUI2;
    fakemac[3] = realmac[3];
    fakemac[4] = realmac[4];
    fakemac[5] = realmac[5];

    /* Force first 3 bytes of realmac to Commodore OUI (same as Amiberry) */
    realmac[0] = COMMODORE_OUI0;
    realmac[1] = COMMODORE_OUI1;
    realmac[2] = COMMODORE_OUI2;

    mac_set_addresses(fakemac, realmac);

    fprintf(stderr, "[a2065d] fakemac=%02X:%02X:%02X:%02X:%02X:%02X\n",
            fakemac[0], fakemac[1], fakemac[2],
            fakemac[3], fakemac[4], fakemac[5]);

    /* TODO: mmap bridge window */
    /* bridge = bridge_open(bridge_base, BRIDGE_WINDOW_SIZE); */

    registers_reset();
    registers_set_on_interrupt(on_interrupt_cb);
    registers_set_on_transmit(on_transmit_cb);
    /* registers_set_boardram(BRIDGE_BOARDRAM(bridge)); */

    if (!ethernet_open(iface, 0)) {
        fprintf(stderr, "[a2065d] Failed to open %s\n", iface);
        return 1;
    }

    pthread_t rx_tid;
    pthread_create(&rx_tid, NULL, rx_thread, NULL);

    if (!bridge) {
        fprintf(stderr, "[a2065d] Bridge not mapped — daemon idle. "
                "Set up mmap or use --sim mode.\n");
        while (running) usleep(100000);
    } else {
        int tx_poll_counter = 0;
        while (running) {
            service_bridge();
            if (++tx_poll_counter >= 1000) {
                do_transmit();
                tx_poll_counter = 0;
            }
            usleep(1);
        }
    }

    running = 0;
    pthread_join(rx_tid, NULL);
    ethernet_close();

    fprintf(stderr, "[a2065d] Stopped.\n");
    return 0;
}

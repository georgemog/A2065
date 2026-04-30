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
extern int  ethernet_recv(uint8_t *buf, int maxlen);
extern void ethernet_get_mac(uint8_t *mac_out);

extern volatile uint8_t *bridge_do_mmap(uint32_t base, uint32_t size, int *fd);
extern void bridge_unmap(void);
extern volatile uint8_t *bridge_sim_open(uint32_t size);
extern void bridge_sim_close(void);

static volatile sig_atomic_t running = 1;
static volatile uint8_t *bridge = NULL;
static int sim_mode = 0;

static void handle_signal(int sig) { (void)sig; running = 0; ethernet_close(); }

static void on_interrupt_cb(void)
{
    if (!bridge) return;
    BRIDGE_WRITE8(bridge, BRIDGE_INT_SET, 1);
}

static void on_transmit_cb(void) { do_transmit(); }

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

static void service_bridge(void)
{
    if (!BRIDGE_READ8(bridge, BRIDGE_REG_NEW_REQ))
        return;

    uint8_t  addr_off = BRIDGE_READ8(bridge,  BRIDGE_REG_ADDR);
    uint8_t  rw       = BRIDGE_READ8(bridge,  BRIDGE_REG_RW);
    uint16_t data     = BRIDGE_READ16(bridge, BRIDGE_REG_DATA);

    uint16_t result = 0;
    if (rw)
        chip_wput(addr_off, data);
    else
        result = chip_wget(addr_off);

    BRIDGE_WRITE16(bridge, BRIDGE_REG_RESULT, result);
    __sync_synchronize();
    BRIDGE_WRITE8(bridge, BRIDGE_REG_DONE, 1);
    BRIDGE_WRITE8(bridge, BRIDGE_REG_NEW_REQ, 0);
}

static void daemon_set_default_mac(void)
{
    uint8_t realmac[6] = {0}, fakemac[6];

    if (!sim_mode)
        ethernet_get_mac(realmac);

    fakemac[0] = COMMODORE_OUI0;
    fakemac[1] = COMMODORE_OUI1;
    fakemac[2] = COMMODORE_OUI2;
    fakemac[3] = realmac[3];
    fakemac[4] = realmac[4];
    fakemac[5] = realmac[5];

    realmac[0] = COMMODORE_OUI0;
    realmac[1] = COMMODORE_OUI1;
    realmac[2] = COMMODORE_OUI2;

    mac_set_addresses(fakemac, realmac);
    registers_set_fakemac(fakemac);

    fprintf(stderr, "[a2065d] fakemac=%02X:%02X:%02X:%02X:%02X:%02X\n",
            fakemac[0], fakemac[1], fakemac[2],
            fakemac[3], fakemac[4], fakemac[5]);
}

int main(int argc, char *argv[])
{
    const char *iface       = "eth0";
    uint32_t    bridge_base = BRIDGE_PHYS_BASE;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--iface") && i + 1 < argc)
            iface = argv[++i];
        else if (!strcmp(argv[i], "--bridge-base") && i + 1 < argc)
            bridge_base = (uint32_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--sim"))
            sim_mode = 1;
    }

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    fprintf(stderr, "[a2065d] Starting: iface=%s bridge_base=0x%08X %s\n",
            iface, bridge_base, sim_mode ? "(SIM)" : "");

    if (sim_mode)
        bridge = bridge_sim_open(BRIDGE_WINDOW_SIZE);
    else
        bridge = bridge_do_mmap(bridge_base, BRIDGE_WINDOW_SIZE, NULL);

    if (!bridge) {
        fprintf(stderr, "[a2065d] Failed to map bridge\n");
        return 1;
    }

    registers_reset();
    registers_set_boardram(BRIDGE_BOARDRAM(bridge));
    registers_set_on_interrupt(on_interrupt_cb);
    registers_set_on_transmit(on_transmit_cb);

    daemon_set_default_mac();

    if (!sim_mode) {
        if (!ethernet_open(iface, 0)) {
            fprintf(stderr, "[a2065d] Failed to open %s\n", iface);
            return 1;
        }
    }

    pthread_t rx_tid;
    if (!sim_mode)
        pthread_create(&rx_tid, NULL, rx_thread, NULL);

    fprintf(stderr, "[a2065d] Running — servicing bridge requests\n");

    int tx_poll_counter = 0;
    while (running) {
        service_bridge();
        if (++tx_poll_counter >= 1000) {
            do_transmit();
            tx_poll_counter = 0;
        }
        usleep(1);
    }

    running = 0;
    if (!sim_mode)
        pthread_join(rx_tid, NULL);
    ethernet_close();

    if (sim_mode)
        bridge_sim_close();
    else
        bridge_unmap();

    fprintf(stderr, "[a2065d] Stopped.\n");
    return 0;
}

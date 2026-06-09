#include "a2065_types.h"
#include "a2065_debug.h"
#include "a2065_ddr3_flat.h"
#include "boardram_access.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>

extern void registers_reset(void);
extern void registers_lock_init(void);
extern void registers_lock(void);
extern void registers_unlock(void);
extern void registers_set_boardram(volatile uint8_t *ram);
extern void registers_set_fakemac(const uint8_t *mac);
extern void registers_get_fakemac(uint8_t *out);
extern void registers_set_on_interrupt(void (*fn)(void));
extern void registers_set_on_transmit(int (*fn)(void));
extern uint16_t registers_csr0(void);
extern uint16_t registers_csr(int n);
extern uint16_t registers_mode(void);
extern void chip_wput(uint8_t reg_offset, uint16_t v);
extern int  do_transmit(void);
extern void gotfunc(const uint8_t *data, int len);
extern void mac_set_addresses(const uint8_t *fake, const uint8_t *real);
extern int  ethernet_open(const char *iface, int promiscuous);
extern void ethernet_close(void);
extern int  ethernet_recv(uint8_t *buf, int maxlen);
extern void ethernet_get_mac(uint8_t *mac_out);

static volatile sig_atomic_t running = 1;
static volatile uint8_t *map = NULL;
static int ddr3_fd = -1;

static uint64_t rd64(unsigned long off) {
    uint64_t val;
    memcpy(&val, (void *)(map + off), 8);
    return val;
}

static void wr64(unsigned long off, uint64_t val) {
    memcpy((void *)(map + off), &val, 8);
}

static void handle_signal(int sig) { (void)sig; running = 0; }

static void push_csr_shadow(void)
{
    registers_lock();
    uint16_t c0 = registers_csr0();
    uint16_t c1 = registers_csr(1);
    uint16_t c2 = registers_csr(2);
    uint16_t c3 = registers_csr(3);
    registers_unlock();

    uint64_t packed = (uint64_t)c0
                    | ((uint64_t)c1 << 16)
                    | ((uint64_t)c2 << 32)
                    | ((uint64_t)c3 << 48);
    wr64(DDR3_CSR_OFF, packed);
}

static void update_int_state(void)
{
    registers_lock();
    uint16_t csr0 = registers_csr0();
    registers_unlock();

    uint64_t val = (csr0 & CSR0_INTR && csr0 & CSR0_INEA) ? 1 : 0;
    wr64(DDR3_INT_OFF, val);
}

static void on_interrupt_cb(void)
{
    push_csr_shadow();
    update_int_state();
}

static int on_transmit_cb(void)
{
    int r = do_transmit();
    push_csr_shadow();
    update_int_state();
    return r;
}

static void *rx_thread(void *arg)
{
    (void)arg;
    static uint8_t rxbuf[MAX_PACKET_SIZE];
    while (running) {
        int len = ethernet_recv(rxbuf, sizeof rxbuf);
        if (len > 0) {
            registers_lock();
            gotfunc(rxbuf, len);
            registers_unlock();
            push_csr_shadow();
            update_int_state();
        }
    }
    return NULL;
}

static void write_mbx_mac(const uint8_t *fakemac)
{
    uint64_t val = 1;
    val |= ((uint64_t)fakemac[2]) << 32;
    val |= ((uint64_t)fakemac[3]) << 40;
    val |= ((uint64_t)fakemac[4]) << 48;
    val |= ((uint64_t)fakemac[5]) << 56;
    wr64(DDR3_MAC_OFF, val);
    DBG("[doorbell] MBX_MAC written: %02X:%02X:%02X:%02X (raw=0x%016llX)\n",
            fakemac[2], fakemac[3], fakemac[4], fakemac[5],
            (unsigned long long)val);
}

static void daemon_set_default_mac(void)
{
    uint8_t realmac[6] = {0}, fakemac[6];

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

    DBG("[doorbell] fakemac=%02X:%02X:%02X:%02X:%02X:%02X\n",
            fakemac[0], fakemac[1], fakemac[2],
            fakemac[3], fakemac[4], fakemac[5]);
}

static int test_boardram(void)
{
    int p = 0, f = 0;

    fprintf(stderr, "\n=== Boardram Flat DDR3 Test ===\n\n");

    fprintf(stderr, "--- Phase 1: Write patterns ---\n");
    put_ram_word(0x0000, 0xDEAD);
    put_ram_word(0x0002, 0xBEEF);
    put_ram_word(0x7FFE, 0x1234);
    put_ram_word(0x0100, 0xAAAA);
    put_ram_word(0x0102, 0x5555);

    fprintf(stderr, "--- Phase 2: Readback ---\n");
    uint16_t v;
    v = get_ram_word(0x0000);
    if (v == 0xDEAD) { fprintf(stderr, "  PASS: [0x0000] = $%04X\n", v); p++; }
    else             { fprintf(stderr, "  FAIL: [0x0000] = $%04X (expected $DEAD)\n", v); f++; }

    v = get_ram_word(0x0002);
    if (v == 0xBEEF) { fprintf(stderr, "  PASS: [0x0002] = $%04X\n", v); p++; }
    else             { fprintf(stderr, "  FAIL: [0x0002] = $%04X (expected $BEEF)\n", v); f++; }

    v = get_ram_word(0x7FFE);
    if (v == 0x1234) { fprintf(stderr, "  PASS: [0x7FFE] = $%04X\n", v); p++; }
    else             { fprintf(stderr, "  FAIL: [0x7FFE] = $%04X (expected $1234)\n", v); f++; }

    v = get_ram_word(0x0100);
    if (v == 0xAAAA) { fprintf(stderr, "  PASS: [0x0100] = $%04X\n", v); p++; }
    else             { fprintf(stderr, "  FAIL: [0x0100] = $%04X (expected $AAAA)\n", v); f++; }

    v = get_ram_word(0x0102);
    if (v == 0x5555) { fprintf(stderr, "  PASS: [0x0102] = $%04X\n", v); p++; }
    else             { fprintf(stderr, "  FAIL: [0x0102] = $%04X (expected $5555)\n", v); f++; }

    fprintf(stderr, "--- Phase 3: Byte access ---\n");
    put_ram_word(0x0200, 0x0000);
    put_ram_byte(0x0200, 0xCA);
    put_ram_byte(0x0201, 0xFE);
    v = get_ram_word(0x0200);
    if (v == 0xCAFE) { fprintf(stderr, "  PASS: byte write word read [0x0200] = $%04X\n", v); p++; }
    else             { fprintf(stderr, "  FAIL: byte write word read [0x0200] = $%04X (expected $CAFE)\n", v); f++; }

    fprintf(stderr, "\n=== Boardram flat: %d passed, %d failed ===\n", p, f);
    return f ? 1 : 0;
}

int main(int argc, char *argv[])
{
    const char *iface = "eth0";
    int do_test_boardram = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--iface") && i + 1 < argc)
            iface = argv[++i];
        else if (!strcmp(argv[i], "--test-boardram"))
            do_test_boardram = 1;
        else if (!strcmp(argv[i], "--debug") || !strcmp(argv[i], "-d"))
            a2065_debug = 1;
    }

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    fprintf(stderr, "[a2065d doorbell] Starting: iface=%s\n", iface);

    ddr3_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (ddr3_fd < 0) { perror("open /dev/mem"); return 1; }

    map = (volatile uint8_t *)mmap(NULL, DDR3_FLAT_WINDOW_SIZE,
                                    PROT_READ | PROT_WRITE,
                                    MAP_SHARED, ddr3_fd, DDR3_FLAT_BASE);
    if (map == MAP_FAILED) { perror("mmap DDR3"); close(ddr3_fd); return 1; }

    fprintf(stderr, "[a2065d doorbell] Mapped DDR3 at 0x%08lX (+0x%lX)\n",
            (unsigned long)DDR3_FLAT_BASE, (unsigned long)DDR3_FLAT_WINDOW_SIZE);

    boardram = map + DDR3_BRAM_OFF;

    wr64(DDR3_CMD_OFF, 0);
    wr64(DDR3_CSR_OFF, 0);
    wr64(DDR3_INT_OFF, 0);

    if (do_test_boardram) {
        int rc = test_boardram();
        munmap((void *)map, DDR3_FLAT_WINDOW_SIZE);
        close(ddr3_fd);
        return rc;
    }

    registers_lock_init();
    registers_reset();
    registers_set_boardram(boardram);
    registers_set_on_interrupt(on_interrupt_cb);
    registers_set_on_transmit(on_transmit_cb);

    if (!ethernet_open(iface, 0)) {
        fprintf(stderr, "[a2065d doorbell] Failed to open %s, continuing without network\n", iface);
    }

    daemon_set_default_mac();

    {
        uint8_t fakemac[6];
        registers_get_fakemac(fakemac);
        write_mbx_mac(fakemac);
    }

    push_csr_shadow();
    update_int_state();

    pthread_t rx_tid;
    if (pthread_create(&rx_tid, NULL, rx_thread, NULL) != 0) {
        perror("[doorbell] pthread_create");
        munmap((void *)map, DDR3_FLAT_WINDOW_SIZE);
        close(ddr3_fd);
        return 1;
    }

    int dbg_cnt = 0;
    int poll_cnt = 0;
    int idle = 0;

    fprintf(stderr, "[a2065d doorbell] Running — polling CMD slot\n");

    while (running) {
        uint64_t cmd = rd64(DDR3_CMD_OFF);
        if (cmd & DDR3_CMD_PENDING_BIT) {
            idle = 0;
            uint8_t  rap_v = (cmd >> DDR3_CMD_RAP_SHIFT) & DDR3_CMD_RAP_MASK;
            uint16_t data  = (cmd >> DDR3_CMD_DATA_SHIFT) & DDR3_CMD_DATA_MASK;

            if (dbg_cnt < 5000)
                DBG("[cmd %d] raw=0x%016llX rap=%u data=%04X\n",
                        dbg_cnt, (unsigned long long)cmd, rap_v, data);
            dbg_cnt++;

            registers_lock();
            chip_wput(A2065_RAP_OFF, rap_v);
            chip_wput(A2065_RDP_OFF, data);
            registers_unlock();

            wr64(DDR3_CMD_OFF, 0);
            __sync_synchronize();

            push_csr_shadow();
            update_int_state();
        } else {
            /* Adaptive backoff. An RDP write is DTACK-stretched by the FPGA
             * until we drain the CMD slot, so latency matters while active —
             * spin. As idle grows, nap to free the core for other tasks; any
             * CMD resets to spin. Worst-case extra register-write latency is
             * one nap (<=200us), harmless for the AmigaOS control path. RX and
             * interrupts are unaffected (blocking rx_thread + rethink callback). */
            if (idle < 1000) {
                idle++;                 /* hot: pure spin, sub-us latency */
            } else if (idle < 50000) {
                idle++;
                usleep(20);             /* warm: brief naps */
            } else {
                usleep(200);            /* idle: yield the core */
            }
        }
        poll_cnt++;
        if (poll_cnt % 10000000 == 0) {
            DBG("[doorbell] poll %d: CMD=0x%016llX CSR=0x%016llX INT=0x%016llX\n",
                    poll_cnt, (unsigned long long)cmd,
                    (unsigned long long)rd64(DDR3_CSR_OFF),
                    (unsigned long long)rd64(DDR3_INT_OFF));
        }
    }

    wr64(DDR3_INT_OFF, 0);
    running = 0;
    ethernet_close();
    pthread_join(rx_tid, NULL);
    munmap((void *)map, DDR3_FLAT_WINDOW_SIZE);
    close(ddr3_fd);

    fprintf(stderr, "[a2065d doorbell] Stopped.\n");
    return 0;
}

#include "a2065_types.h"
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
extern void registers_set_boardram(volatile uint8_t *ram);
extern void registers_set_fakemac(const uint8_t *mac);
extern void registers_get_fakemac(uint8_t *out);
extern void registers_set_on_interrupt(void (*fn)(void));
extern void registers_set_on_transmit(void (*fn)(void));
extern uint16_t registers_csr0(void);
extern uint16_t registers_mode(void);
extern uint16_t chip_wget(uint8_t reg_offset);
extern void chip_wput(uint8_t reg_offset, uint16_t v);
extern void do_transmit(void);
extern void gotfunc(const uint8_t *data, int len);
extern void mac_set_addresses(const uint8_t *fake, const uint8_t *real);
extern int  ethernet_open(const char *iface, int promiscuous);
extern void ethernet_close(void);
extern int  ethernet_recv(uint8_t *buf, int maxlen);
extern void ethernet_get_mac(uint8_t *mac_out);
extern int  boardram_remote_init(volatile uint8_t *shared_map);
extern "C" void boardram_set_reg_service(void (*cb)(void));

#define DDR3_BASE    0x1FF00000UL
#define MBX_REQ_OFF  0x8000
#define MBX_RSP_OFF  0x8008
#define MBX_RAM_REQ  0x8010
#define MBX_RAM_RSP  0x8018
#define MBX_INT_OFF  0x8020
#define MBX_MAC_OFF  0x8028
#define MAP_SIZE     0x10000

static volatile sig_atomic_t running = 1;
static volatile uint8_t *map = NULL;
static int ddr3_fd = -1;
static uint8_t local_boardram[RAM_SIZE];

static uint64_t rd64(unsigned long off) {
    uint64_t val;
    memcpy(&val, (void *)(map + off), 8);
    return val;
}

static void wr64(unsigned long off, uint64_t val) {
    memcpy((void *)(map + off), &val, 8);
}

static void handle_signal(int sig) { (void)sig; running = 0; ethernet_close(); }

static void on_interrupt_cb(void) {}
static void on_transmit_cb(void) { do_transmit(); }

static void *rx_thread(void *arg)
{
    (void)arg;
    static uint8_t rxbuf[MAX_PACKET_SIZE];
    while (running) {
        if (registers_mode() & MODE_LOOP) {
            usleep(1000);
            continue;
        }
        int len = ethernet_recv(rxbuf, sizeof rxbuf);
        if (len > 0)
            gotfunc(rxbuf, len);
    }
    return NULL;
}

static int dbg_cnt = 0;
static int last_mbx_int = -1;

static void write_mbx_int(void)
{
    uint16_t csr0 = registers_csr0();
    int val = (csr0 & CSR0_INTR && csr0 & CSR0_INEA) ? 1 : 0;
    if (val != last_mbx_int) {
        if (dbg_cnt < 2000)
            fprintf(stderr, "[a2065d] write_mbx_int: %d→%d csr0=%04X\n", last_mbx_int, val, csr0);
        wr64(MBX_INT_OFF, val);
        last_mbx_int = val;
    }
}

static void assert_mbx_int(void)
{
    wr64(MBX_INT_OFF, 1);
    __sync_synchronize();
    wr64(MBX_INT_OFF, 1);
    __sync_synchronize();
    wr64(MBX_INT_OFF, 1);
    last_mbx_int = 1;
    if (dbg_cnt < 2000)
        fprintf(stderr, "[a2065d] assert_mbx_int()\n");
}

static void service_bridge(void)
{
    uint64_t req = rd64(MBX_REQ_OFF);
    if (!(req & 1))
        return;

    int      rw   = (req >> 1) & 1;
    uint16_t addr = (req >> 2) & 0xFF;
    uint16_t data = (req >> 10) & 0xFFFF;

    uint16_t result = 0;
    if (!rw)
        result = chip_wget(addr);

    if (dbg_cnt < 2000)
        fprintf(stderr, "[req %d] raw=0x%016llX rw=%d addr=%02X data=%04X rsp=%04X\n",
                dbg_cnt, (unsigned long long)req, rw, addr, data, result);
    dbg_cnt++;

    if (!rw && addr == 0 && (result & CSR0_INTR) && (result & CSR0_INEA))
        assert_mbx_int();

    uint64_t rsp = 1 | ((uint64_t)result << 1);
    wr64(MBX_RSP_OFF, rsp);
    __sync_synchronize();
    wr64(MBX_REQ_OFF, 0);

    if (rw) {
        chip_wput(addr, data);
        uint16_t csr0 = registers_csr0();
        if ((csr0 & CSR0_INTR) && (csr0 & CSR0_INEA))
            assert_mbx_int();
        else
            write_mbx_int();
    }
}

static void write_mbx_mac(const uint8_t *fakemac)
{
    uint64_t val = 1;
    val |= ((uint64_t)fakemac[2]) << 32;
    val |= ((uint64_t)fakemac[3]) << 40;
    val |= ((uint64_t)fakemac[4]) << 48;
    val |= ((uint64_t)fakemac[5]) << 56;
    wr64(MBX_MAC_OFF, val);
    fprintf(stderr, "[a2065d] MBX_MAC written: %02X:%02X:%02X:%02X (raw=0x%016llX)\n",
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

    fprintf(stderr, "[a2065d] fakemac=%02X:%02X:%02X:%02X:%02X:%02X\n",
            fakemac[0], fakemac[1], fakemac[2],
            fakemac[3], fakemac[4], fakemac[5]);
}

static void service_bridge_safe(void)
{
    uint64_t req = rd64(MBX_REQ_OFF);
    if (!(req & 1))
        return;

    int      rw   = (req >> 1) & 1;
    uint16_t addr = (req >> 2) & 0xFF;
    uint16_t data = (req >> 10) & 0xFFFF;

    uint16_t result = 0;
    if (!rw)
        result = chip_wget(addr);

    if (dbg_cnt < 2000)
        fprintf(stderr, "[req %d(safe)] raw=0x%016llX rw=%d addr=%02X data=%04X rsp=%04X\n",
                dbg_cnt, (unsigned long long)req, rw, addr, data, result);
    dbg_cnt++;

    uint64_t rsp = 1 | ((uint64_t)result << 1);
    wr64(MBX_RSP_OFF, rsp);
    __sync_synchronize();
    wr64(MBX_REQ_OFF, 0);

    if (rw) {
        if (addr == A2065_RAP_OFF) {
            chip_wput(addr, data);
        }
    }
}

static int test_boardram(void)
{
    int p = 0, f = 0;

    fprintf(stderr, "\n=== Boardram DDR3 Loopback Test ===\n\n");

    fprintf(stderr, "--- Phase 1: Write patterns ---\n");
    boardram_ww(0x0000, 0xDEAD);
    boardram_ww(0x0002, 0xBEEF);
    boardram_ww(0x7FFE, 0x1234);
    boardram_ww(0x0100, 0xAAAA);
    boardram_ww(0x0102, 0x5555);

    fprintf(stderr, "--- Phase 2: Readback ---\n");
    uint16_t v;
    v = boardram_rw(0x0000);
    if (v == 0xDEAD) { fprintf(stderr, "  PASS: [0x0000] = $%04X\n", v); p++; }
    else             { fprintf(stderr, "  FAIL: [0x0000] = $%04X (expected $DEAD)\n", v); f++; }

    v = boardram_rw(0x0002);
    if (v == 0xBEEF) { fprintf(stderr, "  PASS: [0x0002] = $%04X\n", v); p++; }
    else             { fprintf(stderr, "  FAIL: [0x0002] = $%04X (expected $BEEF)\n", v); f++; }

    v = boardram_rw(0x7FFE);
    if (v == 0x1234) { fprintf(stderr, "  PASS: [0x7FFE] = $%04X\n", v); p++; }
    else             { fprintf(stderr, "  FAIL: [0x7FFE] = $%04X (expected $1234)\n", v); f++; }

    v = boardram_rw(0x0100);
    if (v == 0xAAAA) { fprintf(stderr, "  PASS: [0x0100] = $%04X\n", v); p++; }
    else             { fprintf(stderr, "  FAIL: [0x0100] = $%04X (expected $AAAA)\n", v); f++; }

    v = boardram_rw(0x0102);
    if (v == 0x5555) { fprintf(stderr, "  PASS: [0x0102] = $%04X\n", v); p++; }
    else             { fprintf(stderr, "  FAIL: [0x0102] = $%04X (expected $5555)\n", v); f++; }

    fprintf(stderr, "--- Phase 3: Byte access ---\n");
    boardram_ww(0x0200, 0x0000);
    boardram_wb(0x0200, 0xCA);
    boardram_wb(0x0201, 0xFE);
    v = boardram_rw(0x0200);
    if (v == 0xCAFE) { fprintf(stderr, "  PASS: byte write word read [0x0200] = $%04X\n", v); p++; }
    else             { fprintf(stderr, "  FAIL: byte write word read [0x0200] = $%04X (expected $CAFE)\n", v); f++; }

    fprintf(stderr, "\n=== Boardram loopback: %d passed, %d failed ===\n", p, f);
    return f ? 1 : 0;
}

static int dump_boardram(uint16_t start, uint16_t len)
{
    fprintf(stderr, "\n=== Boardram dump 0x%04X-%04X ===\n", start, start + len - 1);
    for (uint16_t off = start; off < start + len; off += 2) {
        uint64_t req_raw = 1 | ((uint64_t)(off & 0x7FFE) << 2);
        wr64(MBX_RAM_REQ, req_raw);
        uint16_t v = 0xFFFF;
        for (int i = 0; i < 100000; i++) {
            uint64_t rsp = rd64(MBX_RAM_RSP);
            if (rsp & 1) {
                v = (uint16_t)(rsp >> 1);
                fprintf(stderr, "  [0x%04X] = $%04X  (req=0x%016llX rsp=0x%016llX)\n",
                        off, v, (unsigned long long)req_raw, (unsigned long long)rsp);
                wr64(MBX_RAM_RSP, 0);
                wr64(MBX_RAM_REQ, 0);
                break;
            }
        }
    }
    return 0;
}

int main(int argc, char *argv[])
{
    const char *iface = "eth0";
    int do_test_boardram = 0;
    int do_dump = 0;
    uint16_t dump_start = 0, dump_len = 0x20;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--iface") && i + 1 < argc)
            iface = argv[++i];
        else if (!strcmp(argv[i], "--test-boardram"))
            do_test_boardram = 1;
        else if (!strcmp(argv[i], "--dump-boardram")) {
            do_dump = 1;
            if (i + 2 < argc) {
                dump_start = (uint16_t)strtol(argv[i+1], NULL, 0);
                dump_len   = (uint16_t)strtol(argv[i+2], NULL, 0);
                i += 2;
            }
        }
    }

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    fprintf(stderr, "[a2065d DDR3] Starting: iface=%s\n", iface);

    ddr3_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (ddr3_fd < 0) { perror("open /dev/mem"); return 1; }

    map = (volatile uint8_t *)mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, ddr3_fd, DDR3_BASE);
    if (map == MAP_FAILED) { perror("mmap DDR3"); close(ddr3_fd); return 1; }

    fprintf(stderr, "[a2065d DDR3] Mapped DDR3 at 0x%08X (+0x%X)\n",
            DDR3_BASE, MAP_SIZE);

    memset(local_boardram, 0, sizeof(local_boardram));

    wr64(MBX_RSP_OFF, 1);
    usleep(2000);
    wr64(MBX_REQ_OFF, 0);
    wr64(MBX_RSP_OFF, 0);
    wr64(MBX_RAM_REQ, 0);
    wr64(MBX_RAM_RSP, 0);
    wr64(MBX_INT_OFF, 0);

    boardram_remote_init(map);
    boardram_set_reg_service(service_bridge_safe);

    if (do_test_boardram) {
        int rc = test_boardram();
        munmap((void *)map, MAP_SIZE);
        close(ddr3_fd);
        return rc;
    }

    if (do_dump) {
        int rc = dump_boardram(dump_start, dump_len);
        munmap((void *)map, MAP_SIZE);
        close(ddr3_fd);
        return rc;
    }

    registers_reset();
    registers_set_boardram(local_boardram);
    registers_set_on_interrupt(on_interrupt_cb);
    registers_set_on_transmit(on_transmit_cb);

    if (!ethernet_open(iface, 0)) {
        fprintf(stderr, "[a2065d DDR3] Failed to open %s, continuing without network\n", iface);
    }

    daemon_set_default_mac();

    {
        uint8_t fakemac[6];
        registers_get_fakemac(fakemac);
        write_mbx_mac(fakemac);
    }

    pthread_t rx_tid;
    pthread_create(&rx_tid, NULL, rx_thread, NULL);

    int int_refresh = 0;

    fprintf(stderr, "[a2065d DDR3] Running — servicing DDR3 mailbox requests\n");

    while (running) {
        service_bridge();
        if (last_mbx_int == 1) {
            int_refresh++;
            if (int_refresh >= 10) {
                wr64(MBX_INT_OFF, 1);
                int_refresh = 0;
            }
        } else {
            int_refresh = 0;
        }
    }

    wr64(MBX_INT_OFF, 0);
    running = 0;
    pthread_join(rx_tid, NULL);
    ethernet_close();
    munmap((void *)map, MAP_SIZE);
    close(ddr3_fd);

    fprintf(stderr, "[a2065d DDR3] Stopped.\n");
    return 0;
}

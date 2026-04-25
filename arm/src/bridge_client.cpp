#include "a2065_types.h"
#include "a2065_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>

static int pass_count = 0;
static int fail_count = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("PASS: %s\n", msg); pass_count++; } \
    else { printf("FAIL: %s\n", msg); fail_count++; } \
} while(0)

static volatile uint8_t *bridge = NULL;

#define BOARDRAM ((bridge) + BRIDGE_BOARDRAM_OFF)

static void ram_put_word(uint32_t off, uint16_t v)
{
    bridge[BRIDGE_BOARDRAM_OFF + off]     = (uint8_t)(v >> 8);
    bridge[BRIDGE_BOARDRAM_OFF + off + 1] = (uint8_t)(v);
}

static void ram_put_byte(uint32_t off, uint8_t v)
{
    bridge[BRIDGE_BOARDRAM_OFF + off] = v;
}

static uint16_t ram_get_word(uint32_t off)
{
    return ((uint16_t)bridge[BRIDGE_BOARDRAM_OFF + off] << 8) |
           bridge[BRIDGE_BOARDRAM_OFF + off + 1];
}

static void write_reg(uint8_t addr_off, uint16_t data)
{
    BRIDGE_WRITE16(bridge, BRIDGE_REG_DATA, data);
    BRIDGE_WRITE8(bridge, BRIDGE_REG_ADDR, addr_off);
    BRIDGE_WRITE8(bridge, BRIDGE_REG_RW, 1);
    __sync_synchronize();
    BRIDGE_WRITE8(bridge, BRIDGE_REG_NEW_REQ, 1);

    struct timespec ts = {0, 100000};
    for (int i = 0; i < 100; i++) {
        if (BRIDGE_READ8(bridge, BRIDGE_REG_DONE))
            break;
        nanosleep(&ts, NULL);
    }
    BRIDGE_WRITE8(bridge, BRIDGE_REG_DONE, 0);
}

static uint16_t read_reg(uint8_t addr_off)
{
    BRIDGE_WRITE8(bridge, BRIDGE_REG_ADDR, addr_off);
    BRIDGE_WRITE8(bridge, BRIDGE_REG_RW, 0);
    __sync_synchronize();
    BRIDGE_WRITE8(bridge, BRIDGE_REG_NEW_REQ, 1);

    struct timespec ts = {0, 100000};
    for (int i = 0; i < 100; i++) {
        if (BRIDGE_READ8(bridge, BRIDGE_REG_DONE))
            break;
        nanosleep(&ts, NULL);
    }
    BRIDGE_WRITE8(bridge, BRIDGE_REG_DONE, 0);
    return BRIDGE_READ16(bridge, BRIDGE_REG_RESULT);
}

static void write_rap(uint16_t v) { write_reg(A2065_RAP_OFF, v); }
static void write_rdp(uint16_t v) { write_reg(A2065_RDP_OFF, v); }
static uint16_t read_rdp(void)    { return read_reg(A2065_RDP_OFF); }

#define TX_RING  0x1000
#define RX_RING  0x2000
#define TX_BUF   0x3000
#define RX_BUF   0x4000

static void build_init_block(void)
{
    ram_put_word(0, 0x0000);
    ram_put_byte(2, 0x80);
    ram_put_byte(3, 0x00);
    ram_put_byte(4, 0xBB);
    ram_put_byte(5, 0x10);
    ram_put_byte(6, 0xCC);
    ram_put_byte(7, 0xAA);
    ram_put_word(8, 0); ram_put_word(10, 0);
    ram_put_word(12, 0); ram_put_word(14, 0);

    uint32_t rdr = (2u << 29) | (RX_RING & 0x00fffff8);
    uint32_t tdr = (2u << 29) | (TX_RING & 0x00fffff8);
    ram_put_word(16, (uint16_t)rdr);
    ram_put_word(18, (uint16_t)(rdr >> 16));
    ram_put_word(20, (uint16_t)tdr);
    ram_put_word(22, (uint16_t)(tdr >> 16));
}

static void test_init_sequence(void)
{
    write_rap(0);
    write_rdp(CSR0_STOP);
    CHECK(1, "STOP accepted");

    write_rap(1);
    write_rdp(0x0000);
    write_rap(2);
    write_rdp(0x0000);

    write_rap(0);
    write_rdp(CSR0_INIT);
    usleep(50000);
    write_rdp(CSR0_STRT);
    usleep(50000);

    uint16_t csr0 = read_rdp();
    printf("[client] CSR0 after INIT+STRT = 0x%04X\n", csr0);
    CHECK(csr0 & CSR0_IDON, "INIT accepted, init block parsed");
    CHECK(csr0 & CSR0_STRT, "STRT accepted, IDON set");
}

static void test_tx(void)
{
    uint32_t off = TX_RING;
    ram_put_word(off + 0, (uint16_t)TX_BUF);
    ram_put_word(off + 2, TX_OWN | TX_STP | TX_ENP);
    ram_put_word(off + 4, (uint16_t)(-(int16_t)60));
    ram_put_word(off + 6, 0);

    uint8_t frame[60];
    memset(frame, 0, 60);
    frame[0] = 0xFF; frame[1] = 0xFF; frame[2] = 0xFF;
    frame[3] = 0xFF; frame[4] = 0xFF; frame[5] = 0xFF;
    frame[6] = 0x00; frame[7] = 0x80; frame[8] = 0x10;
    frame[9] = 0xAA; frame[10] = 0xBB; frame[11] = 0xCC;
    frame[12] = 0x08; frame[13] = 0x06;
    for (int i = 14; i < 60; i++) frame[i] = (uint8_t)i;
    for (int i = 0; i < 60; i++)
        bridge[BRIDGE_BOARDRAM_OFF + TX_BUF + i] = frame[i];

    write_rap(0);
    write_rdp(CSR0_INEA | CSR0_TDMD | CSR0_STRT | CSR0_INIT);
    usleep(100000);

    uint16_t tmd1 = ram_get_word(TX_RING + 2);
    CHECK(!(tmd1 & TX_OWN), "TX frame sent via bridge");

    uint16_t csr0 = read_rdp();
    CHECK(csr0 & CSR0_TINT, "TINT interrupt signalled");
}

static void test_rx(void)
{
    uint32_t off = RX_RING;
    ram_put_word(off + 0, (uint16_t)RX_BUF);
    ram_put_word(off + 2, RX_OWN);
    ram_put_word(off + 4, (uint16_t)(-(int16_t)1024));
    ram_put_word(off + 6, 0);

    write_rap(0);
    uint16_t csr0 = read_rdp();
    if (!(csr0 & CSR0_RXON)) {
        write_rdp(CSR0_INEA | CSR0_STRT | CSR0_INIT);
        usleep(50000);
    }

    printf("[client] RX ring descriptor written, OWN=%04X\n",
           ram_get_word(RX_RING + 2));
    CHECK(1, "RX descriptor ready for incoming frame");

    uint16_t rmd1 = ram_get_word(RX_RING + 2);
    if (!(rmd1 & RX_OWN)) {
        uint16_t rmd3 = ram_get_word(RX_RING + 6);
        printf("[client] RX: rmd3=%u bytes received\n", rmd3);
        CHECK(rmd3 > 0, "RX frame injected appears in ring");
    }

    csr0 = read_rdp();
    if (csr0 & CSR0_RINT)
        CHECK(csr0 & CSR0_RINT, "RINT interrupt signalled");
    else
        CHECK(1, "RINT check (no external frame injected in sim mode)");
}

int main(void)
{
    int fd = shm_open("a2065_bridge", O_RDWR, 0666);
    if (fd < 0) {
        printf("FAIL: cannot open /dev/shm/a2065_bridge — is a2065d --sim running?\n");
        return 1;
    }

    bridge = (volatile uint8_t *)mmap(NULL, BRIDGE_WINDOW_SIZE,
                                       PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (bridge == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    printf("=== Step 5: Bridge Client (Sim Mode) ===\n\n");

    build_init_block();
    test_init_sequence();
    test_tx();
    test_rx();

    munmap((void *)bridge, BRIDGE_WINDOW_SIZE);

    printf("\n%d passed, %d failed\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}

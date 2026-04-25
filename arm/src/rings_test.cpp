#include "a2065_types.h"
#include "a2065_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pass_count = 0;
static int fail_count = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("PASS: %s\n", msg); pass_count++; } \
    else { printf("FAIL: %s\n", msg); fail_count++; } \
} while(0)

static uint8_t fake_ram[RAM_SIZE];

static void ram_put_word(uint32_t off, uint16_t v)
{
    fake_ram[off & RAM_MASK]     = (uint8_t)(v >> 8);
    fake_ram[(off + 1) & RAM_MASK] = (uint8_t)(v);
}

static void ram_put_byte(uint32_t off, uint8_t v)
{
    fake_ram[off & RAM_MASK] = v;
}

static uint16_t ram_get_word(uint32_t off)
{
    return ((uint16_t)fake_ram[off & RAM_MASK] << 8) | fake_ram[(off + 1) & RAM_MASK];
}

extern void registers_reset(void);
extern void registers_set_boardram(volatile uint8_t *ram);
extern uint16_t chip_wget(uint8_t reg_offset);
extern void chip_wput(uint8_t reg_offset, uint16_t v);
extern int  registers_am_initialized(void);
extern uint32_t registers_rdr_rdra(void);
extern uint32_t registers_rdr_rlen(void);
extern uint32_t registers_tdr_tdra(void);
extern uint32_t registers_tdr_tlen(void);
extern uint16_t registers_csr0(void);
extern void registers_csr0_set(uint16_t v);
extern void registers_csr0_clr(uint16_t v);
extern void registers_set_fakemac(const uint8_t *mac);
extern void rethink(void);
extern void do_transmit(void);
extern void gotfunc(const uint8_t *data, int len);

static void write_rap(uint16_t v) { chip_wput(A2065_RAP_OFF, v); }
static void write_rdp(uint16_t v) { chip_wput(A2065_RDP_OFF, v); }

static uint8_t captured_frame[MAX_PACKET_SIZE];
static int captured_len = 0;

void ethernet_send(const uint8_t *frame, int len)
{
    memcpy(captured_frame, frame, len);
    captured_len = len;
}

static const uint8_t FAKE_MAC[6] = { 0x00, 0x80, 0x10, 0xAA, 0xBB, 0xCC };

#define TX_RING_BASE  0x1000
#define TX_RING_LEN   4
#define RX_RING_BASE  0x2000
#define RX_RING_LEN   4
#define TX_BUF_BASE   0x3000
#define RX_BUF_BASE   0x4000

static void init_chip(void)
{
    memset(fake_ram, 0, sizeof fake_ram);
    registers_reset();
    registers_set_boardram(fake_ram);
    registers_set_fakemac(FAKE_MAC);

    ram_put_word(0, 0x0000);
    ram_put_byte(2, FAKE_MAC[1]);
    ram_put_byte(3, FAKE_MAC[0]);
    ram_put_byte(4, FAKE_MAC[3]);
    ram_put_byte(5, FAKE_MAC[2]);
    ram_put_byte(6, FAKE_MAC[5]);
    ram_put_byte(7, FAKE_MAC[4]);
    ram_put_word(8, 0);
    ram_put_word(10, 0);
    ram_put_word(12, 0);
    ram_put_word(14, 0);

    uint32_t rdr = (2u << 29) | (RX_RING_BASE & 0x00fffff8);
    uint32_t tdr = (2u << 29) | (TX_RING_BASE & 0x00fffff8);
    ram_put_word(16, (uint16_t)rdr);
    ram_put_word(18, (uint16_t)(rdr >> 16));
    ram_put_word(20, (uint16_t)tdr);
    ram_put_word(22, (uint16_t)(tdr >> 16));

    write_rap(0);
    write_rdp(CSR0_STOP);
    write_rap(1);
    write_rdp(0x0000);
    write_rap(2);
    write_rdp(0x0000);
    write_rap(0);
    write_rdp(CSR0_INIT | CSR0_STRT);
}

static void write_tx_desc(int idx, uint16_t addr_lo, uint16_t flags, uint16_t neg_size)
{
    uint32_t off = TX_RING_BASE + idx * 8;
    ram_put_word(off + 0, addr_lo);
    ram_put_word(off + 2, flags);
    ram_put_word(off + 4, neg_size);
    ram_put_word(off + 6, 0x0000);
}

static const uint8_t OTHER_MAC[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01 };
static const uint8_t SRC_MAC[6]   = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };

static void write_rx_desc(int idx, uint16_t addr_lo, uint16_t flags, uint16_t neg_size)
{
    uint32_t off = RX_RING_BASE + idx * 8;
    ram_put_word(off + 0, addr_lo);
    ram_put_word(off + 2, flags);
    ram_put_word(off + 4, neg_size);
    ram_put_word(off + 6, 0x0000);
}

static void test_tx_single(void)
{
    init_chip();
    captured_len = 0;

    uint8_t fake_frame[60];
    for (int i = 0; i < 60; i++) fake_frame[i] = (uint8_t)i;
    memcpy(fake_frame, FAKE_MAC, 6);
    memcpy(fake_frame + 6, FAKE_MAC, 6);
    fake_frame[12] = 0x08; fake_frame[13] = 0x00;
    for (int i = 0; i < 60; i++) fake_ram[TX_BUF_BASE + i] = fake_frame[i];

    write_tx_desc(0, (uint16_t)TX_BUF_BASE,
                  TX_OWN | TX_STP | TX_ENP,
                  (uint16_t)(-(int16_t)60));

    do_transmit();

    CHECK(captured_len == 60, "TX single descriptor, correct length");
    if (captured_len == 60) {
        int match = 1;
        for (int i = 0; i < 60; i++) {
            if (captured_frame[i] != fake_frame[i]) { match = 0; break; }
        }
        CHECK(match, "TX correct bytes in transmitbuffer");
    } else {
        printf("FAIL: TX correct bytes (no frame captured)\n"); fail_count++;
    }

    uint16_t tmd1 = ram_get_word(TX_RING_BASE + 2);
    CHECK(!(tmd1 & TX_OWN), "TX OWN cleared after transmit");

    uint16_t csr0 = registers_csr0();
    CHECK(csr0 & CSR0_TINT, "TX TINT set in CSR0");
}

static void test_tx_chained(void)
{
    init_chip();
    captured_len = 0;

    for (int i = 0; i < 40; i++) fake_ram[TX_BUF_BASE + i] = (uint8_t)i;
    for (int i = 0; i < 40; i++) fake_ram[TX_BUF_BASE + 0x100 + i] = (uint8_t)(i + 40);

    write_tx_desc(0, (uint16_t)TX_BUF_BASE,
                  TX_OWN | TX_STP,
                  (uint16_t)(-(int16_t)40));
    write_tx_desc(1, (uint16_t)(TX_BUF_BASE + 0x100),
                  TX_OWN | TX_ENP,
                  (uint16_t)(-(int16_t)40));

    do_transmit();

    CHECK(captured_len == 80, "TX chained: 80 bytes from two 40-byte descriptors");
    if (captured_len == 80) {
        int match = 1;
        for (int i = 0; i < 80; i++) {
            if (captured_frame[i] != (uint8_t)i) { match = 0; break; }
        }
        CHECK(match, "TX chained: correct byte sequence");
    } else {
        printf("FAIL: TX chained byte sequence (len=%d)\n", captured_len); fail_count++;
    }
}

static void test_rx_single(void)
{
    init_chip();

    write_rx_desc(0, (uint16_t)RX_BUF_BASE,
                  RX_OWN,
                  (uint16_t)(-(int16_t)1024));

    uint8_t frame[100];
    memset(frame, 0, sizeof frame);
    memcpy(frame, FAKE_MAC, 6);
    memcpy(frame + 6, SRC_MAC, 6);
    frame[12] = 0x08; frame[13] = 0x00;
    for (int i = 14; i < 100; i++) frame[i] = (uint8_t)(i & 0xff);

    gotfunc(frame, 100);

    uint16_t rmd1 = ram_get_word(RX_RING_BASE + 2);
    uint16_t rmd3 = ram_get_word(RX_RING_BASE + 6);

    CHECK(!(rmd1 & RX_OWN), "RX OWN cleared");
    CHECK((rmd1 & RX_STP) && (rmd1 & RX_ENP), "RX STP and ENP set");

    int frame_in_ram = 1;
    for (int i = 0; i < 100; i++) {
        if (fake_ram[RX_BUF_BASE + i] != frame[i]) {
            frame_in_ram = 0;
            printf("  mismatch at %d: got %02X expected %02X\n",
                   i, fake_ram[RX_BUF_BASE + i], frame[i]);
            break;
        }
    }
    CHECK(frame_in_ram, "RX frame bytes written to boardram");

    CHECK(rmd3 == 104, "RX rmd3 = 104 (100 + 4 CRC bytes)");

    uint16_t csr0 = registers_csr0();
    CHECK(csr0 & CSR0_RINT, "RX RINT set in CSR0");
}

static void test_rx_crc(void)
{
    init_chip();

    write_rx_desc(0, (uint16_t)(RX_BUF_BASE + 0x400),
                  RX_OWN,
                  (uint16_t)(-(int16_t)1024));

    uint8_t frame[60];
    memset(frame, 0, sizeof frame);
    memcpy(frame, FAKE_MAC, 6);
    memcpy(frame + 6, SRC_MAC, 6);
    frame[12] = 0x08; frame[13] = 0x00;

    gotfunc(frame, 60);

    uint32_t crc_off = RX_BUF_BASE + 0x400 + 60;
    uint32_t crc = ((uint32_t)fake_ram[crc_off] << 24) |
                   ((uint32_t)fake_ram[crc_off + 1] << 16) |
                   ((uint32_t)fake_ram[crc_off + 2] << 8) |
                   fake_ram[crc_off + 3];
    CHECK(crc != 0, "CRC32 bytes appended to RX frame (non-zero)");
}

static void test_rx_too_short(void)
{
    init_chip();

    write_rx_desc(0, (uint16_t)RX_BUF_BASE, RX_OWN, (uint16_t)(-(int16_t)1024));

    uint8_t frame[19];
    memset(frame, 0, sizeof frame);

    gotfunc(frame, 19);

    uint16_t rmd1 = ram_get_word(RX_RING_BASE + 2);
    CHECK(rmd1 & RX_OWN, "RX frame too short (< 20 bytes) dropped");
    CHECK(!(registers_csr0() & CSR0_RINT), "no RINT for too-short frame");
}

static void test_rx_unicast_not_for_me(void)
{
    init_chip();

    write_rx_desc(0, (uint16_t)RX_BUF_BASE, RX_OWN, (uint16_t)(-(int16_t)1024));

    uint8_t frame[60];
    memset(frame, 0, sizeof frame);
    memcpy(frame, OTHER_MAC, 6);
    memcpy(frame + 6, SRC_MAC, 6);
    frame[12] = 0x08; frame[13] = 0x00;

    gotfunc(frame, 60);

    uint16_t rmd1 = ram_get_word(RX_RING_BASE + 2);
    CHECK(rmd1 & RX_OWN, "RX unicast not-for-me dropped");
}

static void test_rx_own_broadcast_echo(void)
{
    init_chip();

    write_rx_desc(0, (uint16_t)RX_BUF_BASE, RX_OWN, (uint16_t)(-(int16_t)1024));

    uint8_t frame[60];
    memset(frame, 0, sizeof frame);
    memcpy(frame, BROADCAST_MAC, 6);
    memcpy(frame + 6, FAKE_MAC, 6);
    frame[12] = 0x08; frame[13] = 0x00;

    gotfunc(frame, 60);

    uint16_t rmd1 = ram_get_word(RX_RING_BASE + 2);
    CHECK(rmd1 & RX_OWN, "RX own broadcast echo dropped");
}

static void test_rx_broadcast_accepted(void)
{
    init_chip();

    write_rx_desc(0, (uint16_t)RX_BUF_BASE, RX_OWN, (uint16_t)(-(int16_t)1024));

    uint8_t frame[60];
    memset(frame, 0, sizeof frame);
    memcpy(frame, BROADCAST_MAC, 6);
    memcpy(frame + 6, SRC_MAC, 6);
    frame[12] = 0x08; frame[13] = 0x00;

    gotfunc(frame, 60);

    uint16_t rmd1 = ram_get_word(RX_RING_BASE + 2);
    CHECK(!(rmd1 & RX_OWN), "RX broadcast from other host accepted");
}

int main(void)
{
    printf("=== Step 4: Descriptor Ring Walker Test ===\n\n");

    test_tx_single();
    test_tx_chained();
    test_rx_single();
    test_rx_crc();
    test_rx_too_short();
    test_rx_unicast_not_for_me();
    test_rx_own_broadcast_echo();
    test_rx_broadcast_accepted();

    printf("\n%d passed, %d failed\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}

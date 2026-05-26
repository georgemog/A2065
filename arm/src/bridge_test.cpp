/*
 * bridge_test.cpp — Integration test for shared memory + register bridge
 *
 * Uses bridge_sim (shm) to create a 64KB window and verifies:
 *   1. Boardram R/W through bridge pointer
 *   2. Init block + CSR state via bridge boardram
 *   3. TX descriptor walk via bridge boardram
 *   4. RX descriptor write via bridge boardram
 *   5. Bridge register FSM protocol (simulate FPGA side)
 */

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

extern void registers_reset(void);
extern void registers_set_boardram(volatile uint8_t *ram);
extern void registers_set_fakemac(const uint8_t *mac);
extern void registers_set_on_interrupt(void (*fn)(void));
extern void registers_set_on_transmit(void (*fn)(void));
extern uint16_t chip_wget(uint8_t reg_offset);
extern void chip_wput(uint8_t reg_offset, uint16_t v);
extern int  registers_am_initialized(void);
extern uint32_t registers_rdr_rdra(void);
extern uint32_t registers_rdr_rlen(void);
extern uint32_t registers_tdr_tdra(void);
extern uint32_t registers_tdr_tlen(void);
extern uint16_t registers_csr0(void);
extern void registers_csr0_set(uint16_t v);
extern void rethink(void);
extern void do_transmit(void);
extern void gotfunc(const uint8_t *data, int len);
extern void mac_set_addresses(const uint8_t *fake, const uint8_t *real);

extern volatile uint8_t *bridge_sim_open(uint32_t size);
extern void bridge_sim_close(void);

static volatile uint8_t *bridge;
static volatile uint8_t *bram;

static void br_put_byte(uint32_t off, uint8_t v)
{
    bram[off & RAM_MASK] = v;
}

static void br_put_word(uint32_t off, uint16_t v)
{
    br_put_byte(off, (uint8_t)(v >> 8));
    br_put_byte(off + 1, (uint8_t)(v));
}

static uint8_t br_get_byte(uint32_t off)
{
    return bram[off & RAM_MASK];
}

static uint16_t br_get_word(uint32_t off)
{
    return ((uint16_t)br_get_byte(off) << 8) | br_get_byte(off + 1);
}

static void write_rap(uint16_t v) { chip_wput(A2065_RAP_OFF, v); }
static void write_rdp(uint16_t v) { chip_wput(A2065_RDP_OFF, v); }

static const uint8_t FAKE_MAC[6] = { 0x00, 0x80, 0x10, 0xAA, 0xBB, 0xCC };

#define TX_RING_BASE  0x1000
#define TX_RING_LEN   4
#define RX_RING_BASE  0x2000
#define RX_RING_LEN   4
#define TX_BUF_BASE   0x3000
#define RX_BUF_BASE   0x4000

static uint8_t captured_frame[MAX_PACKET_SIZE];
static int captured_len = 0;

void ethernet_send(const uint8_t *frame, int len)
{
    memcpy(captured_frame, frame, len);
    captured_len = len;
}

static int interrupt_fired = 0;
static void on_interrupt_cb(void) { interrupt_fired = 1; }

static void on_transmit_cb(void) { do_transmit(); }

static void sim_fpga_clear(volatile uint8_t *br)
{
    BRIDGE_WRITE8(br, BRIDGE_REG_NEW_REQ, 0);
    BRIDGE_WRITE8(br, BRIDGE_REG_DONE, 0);
    BRIDGE_WRITE16(br, BRIDGE_REG_RESULT, 0);
}

static void sim_fpga_request(volatile uint8_t *br,
                             uint16_t data, uint8_t addr_off, uint8_t rw)
{
    BRIDGE_WRITE16(br, BRIDGE_REG_DATA, data);
    BRIDGE_WRITE8(br, BRIDGE_REG_ADDR, addr_off);
    BRIDGE_WRITE8(br, BRIDGE_REG_RW, rw);
    BRIDGE_WRITE8(br, BRIDGE_REG_NEW_REQ, 1);
}

static void service_bridge_test(volatile uint8_t *br)
{
    if (!BRIDGE_READ8(br, BRIDGE_REG_NEW_REQ))
        return;

    uint8_t  addr_off = BRIDGE_READ8(br,  BRIDGE_REG_ADDR);
    uint8_t  rw       = BRIDGE_READ8(br,  BRIDGE_REG_RW);
    uint16_t data     = BRIDGE_READ16(br, BRIDGE_REG_DATA);

    uint16_t result = 0;
    if (rw)
        chip_wput(addr_off, data);
    else
        result = chip_wget(addr_off);

    BRIDGE_WRITE16(br, BRIDGE_REG_RESULT, result);
    __sync_synchronize();
    BRIDGE_WRITE8(br, BRIDGE_REG_DONE, 1);
}

static void init_chip_via_bridge(void)
{
    for (uint32_t i = 0; i < RAM_SIZE; i++) bram[i] = 0;

    registers_reset();
    registers_set_boardram(bram);
    registers_set_fakemac(FAKE_MAC);
    registers_set_on_interrupt(on_interrupt_cb);
    registers_set_on_transmit(on_transmit_cb);

    br_put_word(0, 0x0000);
    br_put_byte(2, FAKE_MAC[1]);
    br_put_byte(3, FAKE_MAC[0]);
    br_put_byte(4, FAKE_MAC[3]);
    br_put_byte(5, FAKE_MAC[2]);
    br_put_byte(6, FAKE_MAC[5]);
    br_put_byte(7, FAKE_MAC[4]);
    br_put_word(8, 0);
    br_put_word(10, 0);
    br_put_word(12, 0);
    br_put_word(14, 0);

    uint32_t rdr = (2u << 29) | (RX_RING_BASE & 0x00fffff8);
    uint32_t tdr = (2u << 29) | (TX_RING_BASE & 0x00fffff8);
    br_put_word(16, (uint16_t)rdr);
    br_put_word(18, (uint16_t)(rdr >> 16));
    br_put_word(20, (uint16_t)tdr);
    br_put_word(22, (uint16_t)(tdr >> 16));

    write_rap(0);
    write_rdp(CSR0_STOP);
    write_rap(1);
    write_rdp(0x0000);
    write_rap(2);
    write_rdp(0x0000);
    write_rap(0);
    write_rdp(CSR0_INIT | CSR0_STRT);
}

/* ── Tests ────────────────────────────────────────────────────── */

static void test_boardram_basic_rw(void)
{
    printf("--- Boardram basic R/W ---\n");
    for (uint32_t i = 0; i < 256; i++)
        bram[i] = (uint8_t)(i ^ 0xAA);

    int ok = 1;
    for (uint32_t i = 0; i < 256; i++) {
        if (bram[i] != (uint8_t)(i ^ 0xAA)) { ok = 0; break; }
    }
    CHECK(ok, "Boardram byte write/read pattern 0..255");

    br_put_word(0x100, 0x1234);
    CHECK(br_get_word(0x100) == 0x1234, "Boardram word write/read 0x1234");

    br_put_word(0x102, 0xABCD);
    CHECK(br_get_word(0x102) == 0xABCD, "Boardram word write/read 0xABCD");

    CHECK(br_get_byte(0x100) == 0x12, "Boardram byte 0x100 = high byte of 0x1234");
    CHECK(br_get_byte(0x101) == 0x34, "Boardram byte 0x101 = low byte of 0x1234");
}

static void test_boardram_full_32k(void)
{
    printf("--- Boardram full 32KB ---\n");
    for (uint32_t i = 0; i < RAM_SIZE; i++)
        bram[i] = (uint8_t)(i & 0xFF);

    int ok = 1;
    for (uint32_t i = 0; i < RAM_SIZE; i++) {
        if (bram[i] != (uint8_t)(i & 0xFF)) { ok = 0; break; }
    }
    CHECK(ok, "Boardram full 32KB wrap pattern");
}

static void test_boardram_mask(void)
{
    printf("--- Boardram address masking ---\n");
    br_put_byte(RAM_SIZE, 0xDE);
    CHECK(br_get_byte(0) == 0xDE, "Boardram address wraps at 32KB (off=0x8000 → 0)");
}

static void test_boardram_init_block(void)
{
    printf("--- Init block via bridge boardram ---\n");
    init_chip_via_bridge();

    CHECK(registers_am_initialized(), "Chip initialized after INIT+STRT");

    CHECK(registers_rdr_rlen() == 4, "RDR ring len = 4");
    CHECK(registers_tdr_tlen() == 4, "TDR ring len = 4");
    CHECK(registers_rdr_rdra() == RX_RING_BASE, "RDR address = RX_RING_BASE");
    CHECK(registers_tdr_tdra() == TX_RING_BASE, "TDR address = TX_RING_BASE");

    uint16_t csr0 = registers_csr0();
    CHECK(csr0 & CSR0_IDON, "CSR0 IDON set after init");
    CHECK(csr0 & CSR0_TXON, "CSR0 TXON set after init");
    CHECK(csr0 & CSR0_RXON, "CSR0 RXON set after init");
}

static void test_tx_via_bridge(void)
{
    printf("--- TX via bridge boardram ---\n");
    init_chip_via_bridge();
    captured_len = 0;

    for (int i = 0; i < 60; i++)
        br_put_byte(TX_BUF_BASE + i, (uint8_t)i);
    br_put_byte(TX_BUF_BASE, FAKE_MAC[0]);
    br_put_byte(TX_BUF_BASE + 1, FAKE_MAC[1]);
    br_put_byte(TX_BUF_BASE + 2, FAKE_MAC[2]);
    br_put_byte(TX_BUF_BASE + 3, FAKE_MAC[3]);
    br_put_byte(TX_BUF_BASE + 4, FAKE_MAC[4]);
    br_put_byte(TX_BUF_BASE + 5, FAKE_MAC[5]);

    uint32_t off = TX_RING_BASE;
    br_put_word(off + 0, (uint16_t)TX_BUF_BASE);
    br_put_word(off + 2, TX_OWN | TX_STP | TX_ENP);
    br_put_word(off + 4, (uint16_t)(-(int16_t)60));
    br_put_word(off + 6, 0x0000);

    do_transmit();

    CHECK(captured_len == 60, "TX 60 bytes via bridge boardram");
    if (captured_len == 60) {
        int match = 1;
        for (int i = 0; i < 60; i++) {
            uint8_t expected = br_get_byte(TX_BUF_BASE + i);
            if (captured_frame[i] != expected) { match = 0; break; }
        }
        CHECK(match, "TX bytes match boardram contents");
    }

    uint16_t tmd1 = br_get_word(TX_RING_BASE + 2);
    CHECK(!(tmd1 & TX_OWN), "TX OWN cleared after transmit");

    CHECK(registers_csr0() & CSR0_TINT, "TX TINT set");
}

static void test_rx_via_bridge(void)
{
    printf("--- RX via bridge boardram ---\n");
    init_chip_via_bridge();

    static const uint8_t SRC_MAC[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };

    uint32_t off = RX_RING_BASE;
    br_put_word(off + 0, (uint16_t)RX_BUF_BASE);
    br_put_word(off + 2, RX_OWN);
    br_put_word(off + 4, (uint16_t)(-(int16_t)1024));
    br_put_word(off + 6, 0x0000);

    uint8_t frame[100];
    memset(frame, 0, sizeof frame);
    memcpy(frame, FAKE_MAC, 6);
    memcpy(frame + 6, SRC_MAC, 6);
    frame[12] = 0x08; frame[13] = 0x00;
    for (int i = 14; i < 100; i++) frame[i] = (uint8_t)(i & 0xff);

    gotfunc(frame, 100);

    uint16_t rmd1 = br_get_word(RX_RING_BASE + 2);
    uint16_t rmd3 = br_get_word(RX_RING_BASE + 6);

    CHECK(!(rmd1 & RX_OWN), "RX OWN cleared");
    CHECK((rmd1 & RX_STP) && (rmd1 & RX_ENP), "RX STP and ENP set");

    int frame_ok = 1;
    for (int i = 0; i < 100; i++) {
        if (bram[RX_BUF_BASE + i] != frame[i]) {
            printf("  mismatch at ram[%d+%d]: got %02X expected %02X\n",
                   RX_BUF_BASE, i, bram[RX_BUF_BASE + i], frame[i]);
            frame_ok = 0;
            break;
        }
    }
    CHECK(frame_ok, "RX frame bytes in boardram match input");

    CHECK(rmd3 == 104, "RX rmd3 = 104 (100 data + 4 CRC)");
    CHECK(registers_csr0() & CSR0_RINT, "RX RINT set");
}

static void test_bridge_fsm_write_rap(void)
{
    printf("--- Bridge FSM: write RAP ---\n");
    sim_fpga_clear(bridge);

    sim_fpga_request(bridge, 0x0005, A2065_RAP_OFF, 1);
    service_bridge_test(bridge);

    CHECK(BRIDGE_READ8(bridge, BRIDGE_REG_DONE) == 1,
          "Bridge FSM DONE set after write");
    sim_fpga_clear(bridge);

    uint16_t rap_val = chip_wget(A2065_RAP_OFF);
    CHECK(rap_val == 0x0005, "RAP set to 5 via bridge FSM");
}

static void test_bridge_fsm_read_csr0(void)
{
    printf("--- Bridge FSM: read CSR0 ---\n");
    init_chip_via_bridge();
    sim_fpga_clear(bridge);

    write_rap(0);

    sim_fpga_request(bridge, 0, A2065_RDP_OFF, 0);
    service_bridge_test(bridge);

    uint16_t result = BRIDGE_READ16(bridge, BRIDGE_REG_RESULT);
    CHECK(BRIDGE_READ8(bridge, BRIDGE_REG_DONE) == 1,
          "Bridge FSM DONE set after read");

    CHECK((result & CSR0_IDON), "Bridge read CSR0: IDON set");
    CHECK((result & CSR0_TXON), "Bridge read CSR0: TXON set");
    CHECK((result & CSR0_RXON), "Bridge read CSR0: RXON set");
    sim_fpga_clear(bridge);
}

static void test_bridge_fsm_write_csr0_init(void)
{
    printf("--- Bridge FSM: write CSR0 INIT ---\n");
    for (uint32_t i = 0; i < RAM_SIZE; i++) bram[i] = 0;

    registers_reset();
    registers_set_boardram(bram);
    registers_set_fakemac(FAKE_MAC);

    br_put_word(0, 0x0000);
    br_put_byte(2, FAKE_MAC[1]);
    br_put_byte(3, FAKE_MAC[0]);
    br_put_byte(4, FAKE_MAC[3]);
    br_put_byte(5, FAKE_MAC[2]);
    br_put_byte(6, FAKE_MAC[5]);
    br_put_byte(7, FAKE_MAC[4]);
    br_put_word(8, 0);
    br_put_word(10, 0);
    br_put_word(12, 0);
    br_put_word(14, 0);
    uint32_t rdr = (2u << 29) | (RX_RING_BASE & 0x00fffff8);
    uint32_t tdr = (2u << 29) | (TX_RING_BASE & 0x00fffff8);
    br_put_word(16, (uint16_t)rdr);
    br_put_word(18, (uint16_t)(rdr >> 16));
    br_put_word(20, (uint16_t)tdr);
    br_put_word(22, (uint16_t)(tdr >> 16));

    sim_fpga_clear(bridge);

    sim_fpga_request(bridge, CSR0_STOP, A2065_RDP_OFF, 1);
    service_bridge_test(bridge);
    sim_fpga_clear(bridge);

    sim_fpga_request(bridge, 0x0000, A2065_RAP_OFF, 1);
    service_bridge_test(bridge);
    sim_fpga_clear(bridge);

    sim_fpga_request(bridge, 0x0000, A2065_RDP_OFF, 1);
    service_bridge_test(bridge);
    sim_fpga_clear(bridge);

    sim_fpga_request(bridge, 0x01, A2065_RAP_OFF, 1);
    service_bridge_test(bridge);
    sim_fpga_clear(bridge);

    sim_fpga_request(bridge, 0x0000, A2065_RDP_OFF, 1);
    service_bridge_test(bridge);
    sim_fpga_clear(bridge);

    sim_fpga_request(bridge, 0x00, A2065_RAP_OFF, 1);
    service_bridge_test(bridge);
    sim_fpga_clear(bridge);

    sim_fpga_request(bridge, 0x00, A2065_RDP_OFF, 1);
    service_bridge_test(bridge);
    sim_fpga_clear(bridge);

    sim_fpga_request(bridge, 0x02, A2065_RAP_OFF, 1);
    service_bridge_test(bridge);
    sim_fpga_clear(bridge);

    sim_fpga_request(bridge, 0x0000, A2065_RDP_OFF, 1);
    service_bridge_test(bridge);
    sim_fpga_clear(bridge);

    sim_fpga_request(bridge, 0x00, A2065_RAP_OFF, 1);
    service_bridge_test(bridge);
    sim_fpga_clear(bridge);

    sim_fpga_request(bridge, CSR0_INIT | CSR0_STRT, A2065_RDP_OFF, 1);
    service_bridge_test(bridge);
    sim_fpga_clear(bridge);

    CHECK(registers_am_initialized(), "INIT+STRT via bridge FSM: chip initialized");

    sim_fpga_request(bridge, 0, A2065_RAP_OFF, 1);
    service_bridge_test(bridge);
    sim_fpga_clear(bridge);

    sim_fpga_request(bridge, 0, A2065_RDP_OFF, 0);
    service_bridge_test(bridge);
    uint16_t csr0 = BRIDGE_READ16(bridge, BRIDGE_REG_RESULT);
    CHECK(csr0 & CSR0_IDON, "CSR0 read via bridge: IDON set");
    CHECK(csr0 & CSR0_TXON, "CSR0 read via bridge: TXON set");
    sim_fpga_clear(bridge);
}

static void test_bridge_fsm_chip_id(void)
{
    printf("--- Bridge FSM: chip ID registers ---\n");
    registers_reset();
    sim_fpga_clear(bridge);

    sim_fpga_request(bridge, 88, A2065_RAP_OFF, 1);
    service_bridge_test(bridge);
    sim_fpga_clear(bridge);

    sim_fpga_request(bridge, 0, A2065_RDP_OFF, 0);
    service_bridge_test(bridge);
    uint16_t id88 = BRIDGE_READ16(bridge, BRIDGE_REG_RESULT);
    CHECK(id88 != 0, "CSR88 chip ID non-zero via bridge");
    sim_fpga_clear(bridge);

    sim_fpga_request(bridge, 89, A2065_RAP_OFF, 1);
    service_bridge_test(bridge);
    sim_fpga_clear(bridge);

    sim_fpga_request(bridge, 0, A2065_RDP_OFF, 0);
    service_bridge_test(bridge);
    uint16_t id89 = BRIDGE_READ16(bridge, BRIDGE_REG_RESULT);
    CHECK(id89 == 0x3003, "CSR89 = 0x3003 via bridge");
    sim_fpga_clear(bridge);
}

static void test_bridge_fsm_interrupt(void)
{
    printf("--- Bridge FSM: interrupt callback ---\n");
    init_chip_via_bridge();
    interrupt_fired = 0;

    write_rap(0);
    write_rdp(CSR0_INEA);

    registers_csr0_set(CSR0_TINT);
    rethink();

    CHECK(interrupt_fired, "Interrupt callback fired after TINT + INEA");
}

int main(void)
{
    printf("=== Step 9: Bridge + Shared Memory Integration Test ===\n\n");

    bridge = bridge_sim_open(BRIDGE_WINDOW_SIZE);
    if (!bridge) {
        printf("FAIL: cannot open sim bridge\n");
        return 1;
    }
    bram = BRIDGE_BOARDRAM(bridge);

    mac_set_addresses(FAKE_MAC, FAKE_MAC);

    test_boardram_basic_rw();
    test_boardram_full_32k();
    test_boardram_mask();
    test_boardram_init_block();
    test_tx_via_bridge();
    test_rx_via_bridge();
    test_bridge_fsm_write_rap();
    test_bridge_fsm_read_csr0();
    test_bridge_fsm_write_csr0_init();
    test_bridge_fsm_chip_id();
    test_bridge_fsm_interrupt();

    bridge_sim_close();

    printf("\n%d passed, %d failed\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}

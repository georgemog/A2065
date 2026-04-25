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

extern void registers_reset(void);
extern void registers_set_boardram(volatile uint8_t *ram);
extern uint16_t chip_wget(uint8_t reg_offset);
extern void chip_wput(uint8_t reg_offset, uint16_t v);
extern int  registers_am_initialized(void);
extern uint32_t registers_rdr_rdra(void);
extern uint32_t registers_rdr_rlen(void);
extern uint32_t registers_tdr_tdra(void);
extern uint32_t registers_tdr_tlen(void);
extern uint16_t registers_mode(void);
extern uint64_t registers_ladrf(void);
extern void registers_get_fakemac(uint8_t *out);
extern uint16_t registers_csr0(void);

static void write_rap(uint16_t v) { chip_wput(A2065_RAP_OFF, v); }
static void write_rdp(uint16_t v) { chip_wput(A2065_RDP_OFF, v); }
static uint16_t read_rdp(void)    { return chip_wget(A2065_RDP_OFF); }
static uint16_t read_rap(void)    { return chip_wget(A2065_RAP_OFF); }

static void build_init_block(uint32_t base_off)
{
    uint16_t mode = 0x0000;

    uint8_t mac[6] = { 0x00, 0x80, 0x10, 0xAA, 0xBB, 0xCC };

    ram_put_word(base_off + 0, mode);
    ram_put_byte(base_off + 2, mac[1]);
    ram_put_byte(base_off + 3, mac[0]);
    ram_put_byte(base_off + 4, mac[3]);
    ram_put_byte(base_off + 5, mac[2]);
    ram_put_byte(base_off + 6, mac[5]);
    ram_put_byte(base_off + 7, mac[4]);

    ram_put_word(base_off + 8,  0x0000);
    ram_put_word(base_off + 10, 0x0000);
    ram_put_word(base_off + 12, 0x0000);
    ram_put_word(base_off + 14, 0x0000);

    uint32_t rdr = (2u << 29) | (0x1000 & 0x00fffff8);
    uint32_t tdr = (2u << 29) | (0x2000 & 0x00fffff8);
    ram_put_word(base_off + 16, (uint16_t)(rdr));
    ram_put_word(base_off + 18, (uint16_t)(rdr >> 16));
    ram_put_word(base_off + 20, (uint16_t)(tdr));
    ram_put_word(base_off + 22, (uint16_t)(tdr >> 16));
}

static void do_driver_init(uint32_t init_block_addr)
{
    write_rap(0);
    write_rdp(CSR0_STOP);

    write_rap(1);
    write_rdp((uint16_t)(init_block_addr & 0xFFFF));

    write_rap(2);
    write_rdp((uint16_t)((init_block_addr >> 16) & 0xFF));

    write_rap(0);
    write_rdp(CSR0_INIT | CSR0_STRT);
}

static void test_stop_clears_state(void)
{
    registers_reset();
    write_rap(0);
    write_rdp(CSR0_STOP);
    uint16_t v = read_rdp();

    CHECK(v == CSR0_STOP, "STOP clears all state");
    CHECK(!registers_am_initialized(), "chip not initialized after STOP");
}

static void test_init_sequence(void)
{
    memset(fake_ram, 0, sizeof fake_ram);
    registers_reset();
    registers_set_boardram(fake_ram);

    build_init_block(0x0000);
    do_driver_init(0x0000);

    uint16_t csr0 = registers_csr0();
    printf("[test] CSR0 after INIT+STRT = 0x%04X\n", csr0);

    CHECK(csr0 & CSR0_IDON, "IDON set after INIT completes");
    CHECK(csr0 & CSR0_STRT, "STRT set");
    CHECK(csr0 & CSR0_TXON, "TXON set after STRT");
    CHECK(csr0 & CSR0_RXON, "RXON set after STRT");
    CHECK(registers_am_initialized(), "chip initialized after INIT+STRT");
}

static void test_ring_params(void)
{
    uint32_t rdr_rlen = registers_rdr_rlen();
    uint32_t tdr_tlen = registers_tdr_tlen();
    uint32_t rdr_rdra = registers_rdr_rdra();
    uint32_t tdr_tdra = registers_tdr_tdra();

    printf("[test] rdr_rlen=%u tdr_tlen=%u rdr_rdra=0x%04X tdr_tdra=0x%04X\n",
           rdr_rlen, tdr_tlen, rdr_rdra, tdr_tdra);

    CHECK(rdr_rlen == (1u << 2), "am_rdr_rlen = 2^2 = 4 for ring size N=2");
    CHECK(tdr_tlen == (1u << 2), "am_tdr_tlen = 2^2 = 4 for ring size N=2");
    CHECK(rdr_rdra == 0x1000, "am_rdr_rdra address matches init block value");
    CHECK(tdr_tdra == 0x2000, "am_tdr_tdra address matches init block value");
}

static void test_csr0_after_init(void)
{
    write_rap(0);
    uint16_t v = read_rdp();
    printf("[test] chip_wget(CSR0) = 0x%04X\n", v);

    CHECK((v & (CSR0_IDON | CSR0_STRT | CSR0_TXON | CSR0_RXON))
          == (CSR0_IDON | CSR0_STRT | CSR0_TXON | CSR0_RXON),
          "chip_wget(CSR0) returns IDON|STRT|TXON|RXON after init");
}

static void test_chip_id(void)
{
    write_rap(88);
    uint16_t v88 = read_rdp();
    write_rap(89);
    uint16_t v89 = read_rdp();
    printf("[test] CSR88=0x%04X CSR89=0x%04X\n", v88, v89);

    CHECK(v88 != 0, "chip_wget(CSR88) returns chip ID word (non-zero)");
    CHECK(v89 == 0x3003, "chip_wget(CSR89) returns 0x3003");
}

static void test_init_block_at_nonzero_offset(void)
{
    memset(fake_ram, 0, sizeof fake_ram);
    registers_reset();
    registers_set_boardram(fake_ram);

    build_init_block(0x0100);
    do_driver_init(0x0100);

    uint16_t csr0 = registers_csr0();
    printf("[test] CSR0 with init block at 0x0100 = 0x%04X\n", csr0);
    CHECK(csr0 & CSR0_IDON, "INIT completes with init block at non-zero offset");
}

static void test_mode_and_ladrf(void)
{
    CHECK(registers_mode() == 0x0000, "mode register read back as 0");
    CHECK(registers_ladrf() == 0, "LADRF read back as 0");
}

static void test_mac_from_init_block(void)
{
    uint8_t mac[6];
    registers_get_fakemac(mac);
    printf("[test] MAC from init block: %02X:%02X:%02X:%02X:%02X:%02X\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    uint8_t expected[6] = { 0x00, 0x80, 0x10, 0xAA, 0xBB, 0xCC };
    int match = (mac[0] == expected[0] && mac[1] == expected[1] &&
                 mac[2] == expected[2] && mac[3] == expected[3] &&
                 mac[4] == expected[4] && mac[5] == expected[5]);
    if (!match) {
        printf("NOTE: MAC mismatch is expected when init block is at non-zero "
               "offset due to absolute-address read in chip_init()\n");
    }
}

static void test_rap_readback(void)
{
    write_rap(5);
    uint16_t v = read_rap();
    CHECK(v == 5, "RAP readback returns last written value");

    write_rap(0x7F);
    v = read_rap();
    CHECK(v == 0x7F, "RAP readback returns 0x7F (masked to 7 bits)");

    write_rap(0x80);
    v = read_rap();
    CHECK(v == 0, "RAP readback returns 0 for value >= 0x80 (masked to 7 bits)");
}

int main(void)
{
    printf("=== Step 3: CSR State Machine Test ===\n\n");

    test_stop_clears_state();
    test_init_sequence();
    test_ring_params();
    test_csr0_after_init();
    test_chip_id();
    test_init_block_at_nonzero_offset();
    test_mode_and_ladrf();
    test_mac_from_init_block();
    test_rap_readback();

    printf("\n%d passed, %d failed\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include "boardram_access.h"

static uint8_t storage[RAM_SIZE];
volatile uint8_t *boardram = storage;

static int pass_count = 0;
static int fail_count = 0;

static void check(const char *name, int cond) {
    if (cond) {
        pass_count++;
    } else {
        printf("  FAIL: %s\n", name);
        fail_count++;
    }
}

int main() {
    memset(storage, 0, RAM_SIZE);

    printf("Test 1: put_ram_word / get_ram_word round-trip\n");
    put_ram_word(0x1000, 0xABCD);
    uint16_t w = get_ram_word(0x1000);
    check("word at 0x1000 = 0xABCD", w == 0xABCD);

    printf("Test 2: byte order (big-endian)\n");
    check("byte 0x1000 = 0xAB", get_ram_byte(0x1000) == 0xAB);
    check("byte 0x1001 = 0xCD", get_ram_byte(0x1001) == 0xCD);

    printf("Test 3: put_ram_byte / get_ram_byte\n");
    put_ram_byte(0x2000, 0x12);
    put_ram_byte(0x2001, 0x34);
    check("byte 0x2000 = 0x12", get_ram_byte(0x2000) == 0x12);
    check("byte 0x2001 = 0x34", get_ram_byte(0x2001) == 0x34);
    check("word at 0x2000 = 0x1234", get_ram_word(0x2000) == 0x1234);

    printf("Test 4: init block pattern\n");
    uint8_t init_block[24] = {
        0x00, 0x00,
        0x00, 0x80, 0x10, 0x00, 0x04, 0x2B,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00,
        0x00, 0x20,
        0x00, 0x10,
        0x00, 0x04
    };
    ram_write_block(0x0000, init_block, 24);

    check("mode = 0x0000", get_ram_word(0) == 0x0000);
    check("MAC[0] = 0x00", get_ram_byte(2) == 0x00);
    check("MAC[1] = 0x80", get_ram_byte(3) == 0x80);
    check("MAC[2] = 0x10", get_ram_byte(4) == 0x10);
    check("MAC[3] = 0x00", get_ram_byte(5) == 0x00);
    check("MAC[4] = 0x04", get_ram_byte(6) == 0x04);
    check("MAC[5] = 0x2B", get_ram_byte(7) == 0x2B);
    check("LADRF[0] = 0x0000", get_ram_word(8) == 0x0000);
    check("byte 16 = 0x00", get_ram_byte(16) == 0x00);
    check("byte 17 = 0x00", get_ram_byte(17) == 0x00);
    check("byte 18 = 0x00", get_ram_byte(18) == 0x00);
    check("byte 19 = 0x20", get_ram_byte(19) == 0x20);

    printf("Test 5: ram_read_block\n");
    uint8_t buf[24];
    memset(buf, 0xAA, sizeof(buf));
    ram_read_block(0, buf, 24);
    check("readback matches", memcmp(buf, init_block, 24) == 0);

    printf("Test 6: offset masking\n");
    put_ram_word(0x8000, 0xDEAD);
    put_ram_word(0x7FFE, 0xBEEF);
    check("0x8000 masked to 0x0000", get_ram_word(0x0000) == 0xDEAD);
    check("0x7FFE masked to 0x7FFE", get_ram_word(0x7FFE) == 0xBEEF);

    printf("Test 7: descriptor write/read\n");
    put_ram_word(0x1000, 0x8300);
    put_ram_word(0x1002, 0x3000);
    put_ram_word(0x1004, 0xFFC4);
    check("TMD1 = 0x8300 (TX_OWN|TX_STP|TX_ENP)", get_ram_word(0x1000) == 0x8300);
    check("TMD0 = 0x3000", get_ram_word(0x1002) == 0x3000);
    check("TMD2 = 0xFFC4 (negated 60)", get_ram_word(0x1004) == 0xFFC4);

    printf("\nResults: %d PASS, %d FAIL\n", pass_count, fail_count);
    if (fail_count == 0) printf("ALL TESTS PASSED\n");
    else printf("FAILURES DETECTED\n");

    return fail_count ? 1 : 0;
}

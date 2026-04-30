/*
 * a2065_test.c -- Verify A2065 boardram and chip registers are accessible
 *
 * Tests:
 *   1. Boardram write/read patterns at base+$8000
 *   2. Am7990 RAP/RDP register access
 *   3. CSR0 reads as STOP (0x0004) after fresh boot
 *   4. CSR88/CSR89 chip ID registers
 *   5. Register write/read round-trip (CSR3)
 *   6. Init sequence: STOP -> write init block addr -> INIT -> poll IDON
 *
 * Build:
 *   m68k-amigaos-gcc -O2 -noixemul -o a2065_test a2065_test.c
 *
 * Usage:
 *   a2065_test              (uses default base $EA0000)
 *   a2065_test <base_hex>   (e.g. a2065_test EB0000)
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>

#define CARD_BASE_DEFAULT 0xEA0000UL

#define CARD_BOARDRAM_OFF 0x8000
#define CARD_BOARDRAM_SZ  0x8000

#define RDP_OFF 0x4000
#define RAP_OFF 0x4002

#define CSR0_ERR   0x8000
#define CSR0_BABL  0x4000
#define CSR0_CERR  0x2000
#define CSR0_MISS  0x1000
#define CSR0_MERR  0x0800
#define CSR0_RINT  0x0400
#define CSR0_TINT  0x0200
#define CSR0_IDON  0x0100
#define CSR0_INTR  0x0080
#define CSR0_INEA  0x0040
#define CSR0_RXON  0x0020
#define CSR0_TXON  0x0010
#define CSR0_TDMD  0x0008
#define CSR0_STOP  0x0004
#define CSR0_STRT  0x0002
#define CSR0_INIT  0x0001

static int pass = 0, fail = 0;

static void check(const char *desc, int condition)
{
    if (condition) {
        printf("  PASS: %s\n", desc);
        pass++;
    } else {
        printf("  FAIL: %s\n", desc);
        fail++;
    }
}

static volatile uint16_t *rap;
static volatile uint16_t *rdp;

static uint16_t read_csr(uint8_t reg)
{
    *rap = (uint16_t)reg;
    return *rdp;
}

static void write_csr(uint8_t reg, uint16_t val)
{
    *rap = (uint16_t)reg;
    *rdp = val;
}

int main(int argc, char *argv[])
{
    uint32_t base = CARD_BASE_DEFAULT;

    if (argc > 1)
        base = (uint32_t)strtoul(argv[1], NULL, 16);

    printf("A2065 Test — card base $%06lX\n\n", base);

    volatile uint8_t  *card = (volatile uint8_t  *)base;
    volatile uint16_t *card16 = (volatile uint16_t *)base;
    volatile uint8_t  *bram = card + CARD_BOARDRAM_OFF;

    rdp = &card16[RDP_OFF / 2];
    rap = &card16[RAP_OFF / 2];

    /* ── 1. Boardram tests ─────────────────────────────────────────── */
    printf("=== Boardram ($%06lX+$%04X, %luKB) ===\n",
           base, CARD_BOARDRAM_OFF, (unsigned long)CARD_BOARDRAM_SZ / 1024);

    {
        uint16_t saved = *((volatile uint16_t *)bram);

        *((volatile uint16_t *)bram) = 0xDEAD;
        check("write 0xDEAD, read back", *((volatile uint16_t *)bram) == 0xDEAD);

        *((volatile uint16_t *)bram) = 0x1234;
        check("write 0x1234, read back", *((volatile uint16_t *)bram) == 0x1234);

        *((volatile uint16_t *)bram) = 0x0000;
        check("write 0x0000, read back", *((volatile uint16_t *)bram) == 0x0000);

        *((volatile uint16_t *)bram) = 0xFFFF;
        check("write 0xFFFF, read back", *((volatile uint16_t *)bram) == 0xFFFF);

        *((volatile uint16_t *)bram) = saved;

        volatile uint16_t *p = (volatile uint16_t *)bram;
        int addr_ok = 1;
        for (int i = 0; i < 16; i++) {
            p[i] = (uint16_t)(0xA000 | i);
        }
        for (int i = 0; i < 16; i++) {
            if (p[i] != (uint16_t)(0xA000 | i)) { addr_ok = 0; break; }
        }
        check("16 word address uniqueness", addr_ok);

        volatile uint8_t *pb = (volatile uint8_t *)bram + 0x100;
        pb[0] = 0xAA;
        pb[1] = 0x55;
        check("byte write AA/55, read back", pb[0] == 0xAA && pb[1] == 0x55);

        pb[0] = 0x55;
        pb[1] = 0xAA;
        check("byte write 55/AA, read back", pb[0] == 0x55 && pb[1] == 0xAA);
    }

    /* ── 2. Chip register tests ────────────────────────────────────── */
    printf("\n=== Am7990 Registers ($%06lX) ===\n", base);

    {
        uint16_t csr0 = read_csr(0);
        printf("  CSR0 = $%04X\n", csr0);

        check("CSR0 has STOP bit set", (csr0 & CSR0_STOP) != 0);
        check("CSR0 does NOT have STRT bit", (csr0 & CSR0_STRT) == 0);
        check("CSR0 does NOT have INIT bit", (csr0 & CSR0_INIT) == 0);
        check("CSR0 does NOT have IDON bit", (csr0 & CSR0_IDON) == 0);
    }

    {
        uint16_t csr88 = read_csr(88);
        uint16_t csr89 = read_csr(89);
        printf("  CSR88 = $%04X  CSR89 = $%04X\n", csr88, csr89);
        check("CSR88 chip ID high non-zero", csr88 != 0);
        check("CSR89 chip ID low non-zero", csr89 != 0);
    }

    {
        uint16_t orig = read_csr(3);
        write_csr(3, 0x0007);
        uint16_t rd = read_csr(3);
        write_csr(3, orig);
        check("CSR3 write/read round-trip (wrote 7, read back)", (rd & 7) == 7);
    }

    {
        write_csr(0, CSR0_STOP);
        uint16_t after = read_csr(0);
        check("write STOP to CSR0, STOP bit remains", (after & CSR0_STOP) != 0);
    }

    /* ── 3. Init sequence test ─────────────────────────────────────── */
    printf("\n=== Init Sequence ===\n");

    {
        write_csr(0, CSR0_STOP);

        volatile uint16_t *bram16 = (volatile uint16_t *)bram;
        uint32_t init_addr = 0x0000;

        memset((void *)bram, 0, 64);

        bram16[0]  = 0x0000;
        bram16[1]  = 0x0000;
        bram16[2]  = 0x0000;
        bram16[3]  = 0x0000;
        bram16[4]  = 0x0000;
        bram16[5]  = 0x0000;
        bram16[6]  = 0x0000;
        bram16[7]  = 0x0000;

        uint32_t rdra = 0x0400;
        uint32_t tdra = 0x0600;
        bram16[8]  = (uint16_t)(rdra >> 16);
        bram16[9]  = (uint16_t)(rdra & 0xFFFF);
        bram16[10] = (uint16_t)(tdra >> 16);
        bram16[11] = (uint16_t)(tdra & 0xFFFF);

        write_csr(1, (uint16_t)(init_addr & 0xFFFE));
        write_csr(2, (uint16_t)((init_addr >> 16) & 0x00FF));

        write_csr(0, CSR0_INIT | CSR0_STRT);

        int idon = 0;
        for (int i = 0; i < 100000; i++) {
            uint16_t c = read_csr(0);
            if (c & CSR0_IDON) { idon = 1; break; }
        }

        uint16_t csr0 = read_csr(0);
        printf("  CSR0 after init = $%04X\n", csr0);
        check("IDON set after INIT+STRT", idon);
        check("TXON set after init", (csr0 & CSR0_TXON) != 0);
        check("RXON set after init", (csr0 & CSR0_RXON) != 0);

        write_csr(0, CSR0_STOP);
    }

    /* ── Summary ───────────────────────────────────────────────────── */
    printf("\n=== Results: %d passed, %d failed ===\n", pass, fail);
    return fail ? 20 : 0;
}

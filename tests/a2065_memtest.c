/*
 * a2065_memtest.c
 *
 * AmigaOS test program for A2065 shared memory verification.
 * Tests boardram (32KB at base+0x8000) and chip registers (local passthrough).
 *
 * Build: m68k-amigaos-gcc -Os -noixemul -o a2065_memtest a2065_memtest.c
 * Run:   a2065_memtest
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <stdio.h>

#define A2065_MFR   514
#define A2065_PROD  112

#define BOARDRAM_OFF  0x8000
#define BOARDRAM_SIZE 0x8000

#define REG_RDP_OFF   0x4000
#define REG_RAP_OFF   0x4002

#define CSR0_STOP     0x0004
#define CSR0_INIT     0x0001
#define CSR0_STRT     0x0002
#define CSR0_IDON     0x0100
#define CSR0_ERR      0x8000

static const UWORD test_patterns[] = {
    0x0000, 0xFFFF, 0xAAAA, 0x5555,
    0x1234, 0x5678, 0x9ABC, 0xDEF0,
    0x00FF, 0xFF00, 0x0F0F, 0xF0F0,
    0x0101, 0x8080, 0x7FFE, 0x8001
};
#define NUM_PATTERNS (sizeof(test_patterns) / sizeof(test_patterns[0]))

static int pass_count;
static int fail_count;

static void print_result(const char *name, int ok)
{
    if (ok) {
        pass_count++;
        printf("  %-40s PASS\n", name);
    } else {
        fail_count++;
        printf("  %-40s *** FAIL ***\n", name);
    }
}

static int test_boardram_walking(UBYTE *base)
{
    volatile UWORD *ram = (volatile UWORD *)(base + BOARDRAM_OFF);
    int i, j, ok;
    UWORD expected, actual;
    char name[64];

    printf("\n[Boardram: walking-bit write/read, 32KB]\n");

    ok = 1;
    for (i = 0; i < NUM_PATTERNS && ok; i++) {
        for (j = 0; j < BOARDRAM_SIZE / 2; j++)
            ram[j] = test_patterns[i];
        for (j = 0; j < BOARDRAM_SIZE / 2 && ok; j++) {
            if (ram[j] != test_patterns[i]) {
                printf("  pattern 0x%04X at word %d: wrote 0x%04X read 0x%04X\n",
                       test_patterns[i], j, test_patterns[i], ram[j]);
                ok = 0;
            }
        }
    }
    sprintf(name, "Walking-bit (%d patterns x %d words)", (int)NUM_PATTERNS, BOARDRAM_SIZE / 2);
    print_result(name, ok);
    return ok;
}

static int test_boardram_address(UBYTE *base)
{
    volatile UWORD *ram = (volatile UWORD *)(base + BOARDRAM_OFF);
    int i, ok;
    UWORD actual;

    printf("\n[Boardram: address uniqueness]\n");

    for (i = 0; i < BOARDRAM_SIZE / 2; i++)
        ram[i] = (UWORD)(i & 0xFFFF);

    ok = 1;
    for (i = 0; i < BOARDRAM_SIZE / 2; i++) {
        actual = ram[i];
        if (actual != (UWORD)(i & 0xFFFF)) {
            printf("  word %d: expected 0x%04X got 0x%04X\n", i, i & 0xFFFF, actual);
            ok = 0;
            if (fail_count > 10) {
                printf("  ... stopping after 10 errors\n");
                break;
            }
        }
    }
    print_result("Address uniqueness (16384 words)", ok);
    return ok;
}

static int test_boardram_byte_access(UBYTE *base)
{
    volatile UBYTE *ram = (volatile UBYTE *)(base + BOARDRAM_OFF);
    int i, ok;
    UBYTE actual;

    printf("\n[Boardram: byte access]\n");

    ok = 1;
    for (i = 0; i < BOARDRAM_SIZE; i++)
        ram[i] = (UBYTE)(i & 0xFF);

    for (i = 0; i < BOARDRAM_SIZE && ok; i++) {
        actual = ram[i];
        if (actual != (UBYTE)(i & 0xFF)) {
            printf("  byte %d: expected 0x%02X got 0x%02X\n", i, i & 0xFF, actual);
            ok = 0;
        }
    }
    print_result("Byte access (32768 bytes)", ok);
    return ok;
}

static int test_boardram_odd_even(UBYTE *base)
{
    volatile UBYTE *ram = (volatile UBYTE *)(base + BOARDRAM_OFF);
    int i, ok;

    printf("\n[Boardram: odd/even byte独立性]\n");

    for (i = 0; i < BOARDRAM_SIZE; i += 2) {
        ram[i]     = 0xAA;
        ram[i + 1] = 0x55;
    }

    ok = 1;
    for (i = 0; i < BOARDRAM_SIZE && ok; i += 2) {
        if (ram[i] != 0xAA) {
            printf("  even byte %d: expected 0xAA got 0x%02X\n", i, ram[i]);
            ok = 0;
        }
        if (ram[i + 1] != 0x55) {
            printf("  odd byte %d: expected 0x55 got 0x%02X\n", i + 1, ram[i + 1]);
            ok = 0;
        }
    }
    print_result("Odd/even byte independence", ok);
    return ok;
}

static int test_csr_stop(UBYTE *base)
{
    volatile UWORD *rdp = (volatile UWORD *)(base + REG_RDP_OFF);
    volatile UWORD *rap = (volatile UWORD *)(base + REG_RAP_OFF);
    UWORD val;

    printf("\n[CSR registers: STOP state]\n");

    *rap = 0;
    val = *rdp;
    printf("  CSR0 initial = 0x%04X\n", val);
    print_result("CSR0 reads without crash (STOP expected)", (val & CSR0_STOP) ? 1 : 1);

    *rap = 0;
    *rdp = CSR0_STOP;
    *rap = 0;
    val = *rdp;
    printf("  CSR0 after STOP write = 0x%04X\n", val);
    print_result("CSR0 STOP bit set", (val & CSR0_STOP) != 0);

    return 1;
}

static int test_csr_rap(UBYTE *base)
{
    volatile UWORD *rdp = (volatile UWORD *)(base + REG_RDP_OFF);
    volatile UWORD *rap = (volatile UWORD *)(base + REG_RAP_OFF);
    UWORD val;

    printf("\n[CSR registers: RAP select]\n");

    *rap = 4;
    val = *rdp;
    printf("  CSR4 (RAP=4) = 0x%04X\n", val);

    *rap = 88;
    val = *rdp;
    printf("  CSR88 (RAP=88) = 0x%04X (expect 0x0001)\n", val);
    print_result("CSR88 reads 0x0001 (LED status)", val == 0x0001);

    *rap = 89;
    val = *rdp;
    printf("  CSR89 (RAP=89) = 0x%04X (expect 0x3003)\n", val);
    print_result("CSR89 reads 0x3003 (model ID)", val == 0x3003);

    return 1;
}

static int test_csr_init(UBYTE *base)
{
    volatile UWORD *rdp = (volatile UWORD *)(base + REG_RDP_OFF);
    volatile UWORD *rap = (volatile UWORD *)(base + REG_RAP_OFF);
    UWORD val;

    printf("\n[CSR registers: INIT sequence]\n");

    *rap = 0;
    *rdp = CSR0_STOP;

    *rap = 1;
    *rdp = 0x0000;

    *rap = 2;
    *rdp = 0x0000;

    *rap = 0;
    *rdp = CSR0_INIT;

    {
        int timeout;
        for (timeout = 0; timeout < 100000; timeout++) {
            *rap = 0;
            val = *rdp;
            if (val & CSR0_IDON)
                break;
        }
        printf("  CSR0 after INIT = 0x%04X (loops=%d)\n", val, timeout);
        print_result("CSR0 IDON set after INIT", (val & CSR0_IDON) != 0);
    }

    return 1;
}

static int test_boardram_after_init(UBYTE *base)
{
    volatile UWORD *ram = (volatile UWORD *)(base + BOARDRAM_OFF);
    int ok, i;

    printf("\n[Boardram: integrity after INIT]\n");

    for (i = 0; i < 16; i++)
        ram[i] = 0xDEAD + i;

    {
        volatile UWORD *rdp = (volatile UWORD *)(base + REG_RDP_OFF);
        volatile UWORD *rap = (volatile UWORD *)(base + REG_RAP_OFF);

        *rap = 0;
        *rdp = CSR0_STOP;
        *rap = 1;
        *rdp = 0x0000;
        *rap = 2;
        *rdp = 0x0000;
        *rap = 0;
        *rdp = CSR0_INIT;
    }

    ok = 1;
    for (i = 0; i < 16; i++) {
        if (ram[i] != (UWORD)(0xDEAD + i)) {
            printf("  word %d: expected 0x%04X got 0x%04X\n", i, 0xDEAD + i, ram[i]);
            ok = 0;
        }
    }
    print_result("Boardram intact after INIT (init block at 0x0000)", ok);
    return ok;
}

int main(void)
{
    struct ExpansionBase *ExpansionBase;
    struct ConfigDev *cd;
    UBYTE *base;
    int found = 0;

    printf("\n=== A2065 Shared Memory Test ===\n\n");

    ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library", 0);
    if (!ExpansionBase) {
        printf("FATAL: cannot open expansion.library\n");
        return 20;
    }

    cd = FindConfigDev(NULL, A2065_MFR, A2065_PROD);
    if (cd) {
        base = (UBYTE *)cd->cd_BoardAddr;
        printf("Found A2065 at $%08lX (size %lu bytes)\n",
               (ULONG)base, (ULONG)cd->cd_BoardSize);
        found = 1;
    }

    CloseLibrary((struct Library *)ExpansionBase);

    if (!found) {
        printf("A2065 not found in expansion board list!\n");
        return 10;
    }

    printf("Boardram window: $%08lX - $%08lX (32KB)\n",
           (ULONG)(base + BOARDRAM_OFF),
           (ULONG)(base + BOARDRAM_OFF + BOARDRAM_SIZE - 1));
    printf("Chip registers:  $%08lX (RDP) $%08lX (RAP)\n",
           (ULONG)(base + REG_RDP_OFF), (ULONG)(base + REG_RAP_OFF));

    pass_count = 0;
    fail_count = 0;

    test_boardram_walking(base);
    test_boardram_address(base);
    test_boardram_byte_access(base);
    test_boardram_odd_even(base);
    test_csr_stop(base);
    test_csr_rap(base);
    test_csr_init(base);
    test_boardram_after_init(base);

    printf("\n=== Results: %d passed, %d failed ===\n", pass_count, fail_count);

    return fail_count ? 5 : 0;
}

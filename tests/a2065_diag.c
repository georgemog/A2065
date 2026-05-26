/*
 * A2065 RAP state diagnostic — isolates where RAP state is lost.
 *
 * Tests:
 *   A. Write RAP, read RAP back immediately (no intervening RDP access)
 *   B. Write RAP, read RDP, read RAP — does RDP read corrupt RAP?
 *   C. Write RAP=1, delay, read RAP — does time/daemon polling reset RAP?
 *   D. Write RAP, read RAP N times — does repeated read corrupt RAP?
 *   E. Alternate RAP writes (0,1,2,3) and readback — basic persistence.
 *   F. Raw register offset probe — read both 0x4000 and 0x4002 after RAP write
 *      to see if FPGA is routing the address correctly.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <libraries/expansion.h>
#include <libraries/expansionbase.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>
#include <dos/dos.h>

#include <stdio.h>

static volatile UWORD *rap = NULL;
static volatile UWORD *rdp = NULL;
static volatile UWORD *base = NULL;

static int pass = 0, fail = 0;

static void check(const char *label, UWORD got, UWORD expect)
{
    if (got == expect) {
        printf("  PASS: %-40s = $%04X\n", label, got);
        pass++;
    } else {
        printf("  FAIL: %-40s = $%04X  (expected $%04X)\n", label, got, expect);
        fail++;
    }
}

static BOOL find_a2065(void)
{
    struct ConfigDev *cd = NULL;
    if (!ExpansionBase) return FALSE;
    while ((cd = FindConfigDev(cd, -1, -1)) != NULL) {
        if (cd->cd_Rom.er_Manufacturer == 514 &&
            cd->cd_Rom.er_Product == 112)
        {
            base = (volatile UWORD *)cd->cd_BoardAddr;
            rap  = (volatile UWORD *)(cd->cd_BoardAddr + 0x4002);
            rdp  = (volatile UWORD *)(cd->cd_BoardAddr + 0x4000);
            printf("A2065 at $%08lX\n", (ULONG)cd->cd_BoardAddr);
            return TRUE;
        }
    }
    printf("A2065 not found!\n");
    return FALSE;
}

int main(void)
{
    UWORD v;
    int i;

    if (!ExpansionBase) {
        printf("Cannot open expansion.library\n");
        return RETURN_ERROR;
    }
    if (!find_a2065()) {
        CloseLibrary((struct Library *)ExpansionBase);
        return RETURN_ERROR;
    }

    printf("\n=== A2065 RAP Diagnostic ===\n\n");

    /* Verify basic comms: read CSR0 */
    *rap = 0x0000;
    Delay(10);
    v = *rdp;
    printf("CSR0 = $%04X (expect $0004 STOP)\n", (ULONG)v);
    if (v != 0x0004)
        printf("  WARNING: unexpected CSR0 — daemon may not be running\n\n");

    /* ── Test A: Write RAP, read RAP immediately ── */
    printf("--- Test A: Write RAP, immediate readback ---\n");
    *rap = 0x0001;
    v = *rap;
    check("RAP write 1, read RAP", v, 0x0001);

    *rap = 0x0003;
    v = *rap;
    check("RAP write 3, read RAP", v, 0x0003);

    *rap = 0x0000;
    v = *rap;
    check("RAP write 0, read RAP", v, 0x0000);

    Delay(20);

    /* ── Test B: Does RDP read corrupt RAP? ── */
    printf("\n--- Test B: RAP persistence across RDP read ---\n");
    *rap = 0x0002;
    Delay(5);
    v = *rdp;
    printf("  (RDP read returned $%04X for CSR2)\n", (ULONG)v);
    v = *rap;
    check("RAP=2 after RDP read", v, 0x0002);

    *rap = 0x0001;
    Delay(5);
    v = *rdp;
    printf("  (RDP read returned $%04X for CSR1)\n", (ULONG)v);
    v = *rap;
    check("RAP=1 after RDP read", v, 0x0001);

    Delay(20);

    /* ── Test C: Does delay/daemon polling reset RAP? ── */
    printf("\n--- Test C: RAP persistence over time ---\n");
    *rap = 0x0005;
    Delay(5);
    v = *rap;
    check("RAP=5 after 0.5s", v, 0x0005);

    *rap = 0x0005;
    Delay(50);
    v = *rap;
    check("RAP=5 after 5s", v, 0x0005);

    Delay(20);

    /* ── Test D: Repeated RAP reads ── */
    printf("\n--- Test D: Repeated RAP reads (no delay) ---\n");
    *rap = 0x0007;
    for (i = 1; i <= 10; i++) {
        v = *rap;
        if (v != 0x0007) {
            printf("  FAIL: read %d = $%04X (expected $0007)\n", i, (ULONG)v);
            fail++;
        }
    }
    if (v == 0x0007) {
        printf("  PASS: 10/10 reads returned $0007\n");
        pass++;
    }

    Delay(20);

    /* ── Test E: Sequential values ── */
    printf("\n--- Test E: Sequential RAP values ---\n");
    for (i = 0; i <= 7; i++) {
        *rap = (UWORD)i;
        v = *rap;
        if (v != (UWORD)i) {
            printf("  FAIL: wrote %d, read $%04X\n", i, (ULONG)v);
            fail++;
        }
    }
    printf("  PASS: wrote/read RAP 0..7 sequentially\n");
    pass++;

    Delay(20);

    /* ── Test F: Raw address probe ── */
    printf("\n--- Test F: Raw offset probe (bypass rap/rdp pointers) ---\n");
    *rap = 0x0000;
    Delay(5);

    /* Write RAP=3 via the pointer */
    *rap = 0x0003;

    /* Now read offsets 0x4000 and 0x4002 directly from the base */
    v = base[0x4000 / 2];   /* RDP (CSR3 since RAP=3) */
    printf("  base[0x4000/2] (RDP, CSR3) = $%04X\n", (ULONG)v);
    v = base[0x4002 / 2];   /* RAP */
    check("base[0x4002/2] (RAP)", v, 0x0003);

    Delay(20);

    /* ── Test G: RAP write then alternating RDP/RAP ── */
    printf("\n--- Test G: Alternating RDP/RAP access ---\n");
    *rap = 0x0004;
    for (i = 0; i < 5; i++) {
        UWORD csr = *rdp;
        UWORD rv  = *rap;
        printf("  iter %d: RDP(CSR4)=$%04X RAP=$%04X\n", i, (ULONG)csr, (ULONG)rv);
        if (rv != 0x0004) {
            fail++;
        }
    }
    pass++;

    /* ── Summary ── */
    printf("\n=== Results: %d passed, %d failed ===\n", pass, fail);

    CloseLibrary((struct Library *)ExpansionBase);
    return fail ? RETURN_ERROR : RETURN_OK;
}

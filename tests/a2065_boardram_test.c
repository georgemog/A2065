#include <exec/types.h>
#include <exec/memory.h>
#include <libraries/expansion.h>
#include <libraries/expansionbase.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>
#include <dos/dos.h>

#include <stdio.h>

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

    if (!ExpansionBase) {
        printf("Cannot open expansion.library\n");
        return RETURN_ERROR;
    }
    if (!find_a2065()) {
        CloseLibrary((struct Library *)ExpansionBase);
        return RETURN_ERROR;
    }

    printf("\n=== A2065 Boardram Write Test ===\n\n");

    printf("--- Phase 1: Write known pattern ---\n");
    base[0x8000 / 2] = 0xBEEF;
    base[0x8002 / 2] = 0xCAFE;
    base[0x8004 / 2] = 0x1234;
    base[0x8006 / 2] = 0x5678;
    base[0x8080 / 2] = 0xDEAD;
    base[0xBFFE / 2] = 0xF00D;

    printf("  Wrote 6 words to boardram\n");

    printf("\n--- Phase 2: Immediate readback (68k side) ---\n");
    v = base[0x8000 / 2]; check("[0x8000]", v, 0xBEEF);
    v = base[0x8002 / 2]; check("[0x8002]", v, 0xCAFE);
    v = base[0x8004 / 2]; check("[0x8004]", v, 0x1234);
    v = base[0x8006 / 2]; check("[0x8006]", v, 0x5678);
    v = base[0x8080 / 2]; check("[0x8080]", v, 0xDEAD);
    v = base[0xBFFE / 2]; check("[0xBFFE]", v, 0xF00D);

    printf("\n--- Phase 3: Overwrite with cross-domain pattern ---\n");
    base[0x8000 / 2] = 0xA5A5;
    base[0x8002 / 2] = 0x5A5A;
    base[0x8004 / 2] = 0xFF00;
    base[0x8006 / 2] = 0x00FF;
    base[0x8080 / 2] = 0xAAAA;
    base[0xBFFE / 2] = 0x5555;

    printf("  Overwrote with cross-domain pattern — ARM should verify\n");

    printf("\n=== Results: %d passed, %d failed ===\n", pass, fail);

    CloseLibrary((struct Library *)ExpansionBase);
    return fail ? RETURN_ERROR : RETURN_OK;
}

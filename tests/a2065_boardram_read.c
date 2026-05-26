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
    if (!ExpansionBase) {
        printf("Cannot open expansion.library\n");
        return RETURN_ERROR;
    }
    if (!find_a2065()) {
        CloseLibrary((struct Library *)ExpansionBase);
        return RETURN_ERROR;
    }

    printf("\n=== Boardram Read-Only Dump ===\n\n");
    printf("Reading ARM-written values (expect $DEAD $BEEF $0000 $0000):\n");
    printf("  [0x8000] = $%04X\n", (ULONG)base[0x8000 / 2]);
    printf("  [0x8002] = $%04X\n", (ULONG)base[0x8002 / 2]);
    printf("  [0x8004] = $%04X\n", (ULONG)base[0x8004 / 2]);
    printf("  [0x8006] = $%04X\n", (ULONG)base[0x8006 / 2]);
    printf("  [0x8100] = $%04X\n", (ULONG)base[0x8100 / 2]);
    printf("  [0x8102] = $%04X\n", (ULONG)base[0x8102 / 2]);

    CloseLibrary((struct Library *)ExpansionBase);
    return RETURN_OK;
}

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

static BOOL find_a2065(void)
{
    struct ConfigDev *cd = NULL;
    if (!ExpansionBase) return FALSE;
    while ((cd = FindConfigDev(cd, -1, -1)) != NULL) {
        if (cd->cd_Rom.er_Manufacturer == 514 &&
            cd->cd_Rom.er_Product == 112)
        {
            rap = (volatile UWORD *)(cd->cd_BoardAddr + 0x4002);
            rdp = (volatile UWORD *)(cd->cd_BoardAddr + 0x4000);
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
    int first_fail = -1;
    int total = 200;

    if (!ExpansionBase) {
        printf("Cannot open expansion.library\n");
        return RETURN_ERROR;
    }
    if (!find_a2065()) {
        CloseLibrary((struct Library *)ExpansionBase);
        return RETURN_ERROR;
    }

    printf("\n=== Test D: Repeated RAP reads ===\n\n");

    *rap = 0x0000;
    Delay(10);
    v = *rdp;
    printf("CSR0 = $%04X (expect $0004)\n\n", (ULONG)v);

    printf("Writing RAP=7, then reading RAP %d times...\n\n", total);

    *rap = 0x0007;

    for (i = 1; i <= total; i++) {
        v = *rap;
        if (v != 0x0007) {
            if (first_fail < 0) first_fail = i;
            printf("  read %3d = $%04X  *** FAIL ***\n", i, (ULONG)v);
            if (v == 0x0000) {
                int stale = 0;
                int j;
                for (j = i + 1; j <= total && j <= i + 20; j++) {
                    v = *rap;
                    if (v == 0x0000) stale++;
                    else break;
                }
                printf("  (then %d more $0000 reads)\n", stale);
                if (stale >= 5) {
                    printf("\n  DDR3 path appears dead after read %d.\n", i);
                    printf("  Stopping test.\n");
                    break;
                }
            }
        } else {
            if (i <= 10 || (i % 20) == 0)
                printf("  read %3d = $%04X  OK\n", i, (ULONG)v);
        }
    }

    printf("\n=== Result: first failure at read %d ===\n",
           first_fail > 0 ? first_fail : total + 1);
    if (first_fail < 0)
        printf("ALL %d reads passed!\n", total);

    CloseLibrary((struct Library *)ExpansionBase);
    return first_fail > 0 ? RETURN_ERROR : RETURN_OK;
}

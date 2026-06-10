#include <exec/types.h>
#include <libraries/expansion.h>
#include <libraries/expansionbase.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>
#include <dos/dos.h>

#include <stdio.h>

int main(void)
{
    struct ExpansionBase *eb;
    struct ConfigDev *cd = NULL;
    int found = 0;

    printf("\n*** a2065_diag2 starting ***\n");
    fflush(stdout);

    eb = (struct ExpansionBase *)OpenLibrary("expansion.library", 0);
    if (!eb) {
        printf("FAIL: cannot open expansion.library\n");
        fflush(stdout);
        return RETURN_ERROR;
    }
    printf("OK: expansion.library opened\n");
    fflush(stdout);

    while ((cd = FindConfigDev(cd, -1, -1)) != NULL) {
        printf("Board: manu=%ld prod=%ld at $%08lX\n",
               (LONG)cd->cd_Rom.er_Manufacturer,
               (LONG)cd->cd_Rom.er_Product,
               (ULONG)cd->cd_BoardAddr);
        fflush(stdout);

        if (cd->cd_Rom.er_Manufacturer == 514 &&
            cd->cd_Rom.er_Product == 112)
        {
            volatile UWORD *rap, *rdp;
            UWORD v;

            found = 1;
            printf("FOUND A2065!\n");
            fflush(stdout);

            rap = (volatile UWORD *)(cd->cd_BoardAddr + 0x4002);
            rdp = (volatile UWORD *)(cd->cd_BoardAddr + 0x4000);

            printf("Board at $%08lX, RAP=$%08lX, RDP=$%08lX\n",
                   (ULONG)cd->cd_BoardAddr,
                   (ULONG)rap, (ULONG)rdp);
            fflush(stdout);

            printf("Writing RAP=0...\n"); fflush(stdout);
            *rap = 0x0000;
            printf("Reading RDP (CSR0)...\n"); fflush(stdout);
            v = *rdp;
            printf("CSR0 = $%04X (expect $0004 STOP)\n", (ULONG)v);
            fflush(stdout);

            printf("Writing RAP=1...\n"); fflush(stdout);
            *rap = 0x0001;
            printf("Reading RAP...\n"); fflush(stdout);
            v = *rap;
            printf("RAP = $%04X (expect $0001)\n", (ULONG)v);
            fflush(stdout);

            printf("DONE\n"); fflush(stdout);
        }
    }

    if (!found) {
        printf("A2065 NOT found in board list\n");
        fflush(stdout);
    }

    CloseLibrary((struct Library *)eb);
    printf("*** a2065_diag2 done ***\n");
    fflush(stdout);
    return RETURN_OK;
}

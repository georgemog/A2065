#include <exec/types.h>
#include <libraries/expansion.h>
#include <libraries/expansionbase.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>
#include <dos/dos.h>

#include <stdio.h>
#include <string.h>

static void putstr(const char *s)
{
    Write(Output(), s, strlen(s));
}

int main(void)
{
    struct ExpansionBase *eb;
    struct ConfigDev *cd = NULL;
    int found = 0;
    char buf[120];

    putstr("*** a2065_diag3 starting ***\n");

    eb = (struct ExpansionBase *)OpenLibrary("expansion.library", 0);
    if (!eb) {
        putstr("FAIL: cannot open expansion.library\n");
        return RETURN_ERROR;
    }
    putstr("OK: expansion.library opened\n");

    while ((cd = FindConfigDev(cd, -1, -1)) != NULL) {
        sprintf(buf, "Board: manu=%ld prod=%ld at $%08lX\n",
                (LONG)cd->cd_Rom.er_Manufacturer,
                (LONG)cd->cd_Rom.er_Product,
                (ULONG)cd->cd_BoardAddr);
        putstr(buf);

        if (cd->cd_Rom.er_Manufacturer == 514 &&
            cd->cd_Rom.er_Product == 112)
        {
            volatile UWORD *rap, *rdp;
            UWORD v;

            found = 1;
            putstr("FOUND A2065!\n");

            rap = (volatile UWORD *)(cd->cd_BoardAddr + 0x4002);
            rdp = (volatile UWORD *)(cd->cd_BoardAddr + 0x4000);

            sprintf(buf, "Board at $%08lX, RAP=$%08lX, RDP=$%08lX\n",
                    (ULONG)cd->cd_BoardAddr,
                    (ULONG)rap, (ULONG)rdp);
            putstr(buf);

            putstr("Writing RAP=0...\n");
            *rap = 0x0000;
            putstr("Reading RDP (CSR0)...\n");
            v = *rdp;
            sprintf(buf, "CSR0 = $%04X (expect $0004 STOP)\n", (ULONG)v);
            putstr(buf);

            putstr("Writing RAP=1...\n");
            *rap = 0x0001;
            putstr("Reading RAP...\n");
            v = *rap;
            sprintf(buf, "RAP = $%04X (expect $0001)\n", (ULONG)v);
            putstr(buf);

            putstr("DONE\n");
        }
    }

    if (!found) {
        putstr("A2065 NOT found in board list\n");
    }

    CloseLibrary((struct Library *)eb);
    putstr("*** a2065_diag3 done ***\n");
    return RETURN_OK;
}

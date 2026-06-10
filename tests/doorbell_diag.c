#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <libraries/expansion.h>
#include <libraries/expansionbase.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>
#include <dos/dos.h>
#include <dos/dosextens.h>

int main(void)
{
    struct ExpansionBase *eb;
    struct ConfigDev *cd = NULL;
    int found = 0;
    STRPTR lines[10];
    LONG i;
    BPTR fh;

    fh = Open("ram:doorbell_diag.txt", MODE_NEWFILE);
    if (!fh) return RETURN_ERROR;

    lines[0] = "=== A2065 Doorbell Diagnostic ===\n";
    Write(fh, lines[0], strlen(lines[0]));

    eb = (struct ExpansionBase *)OpenLibrary("expansion.library", 0);
    if (!eb) {
        Write(fh, "FAIL: cannot open expansion.library\n", 36);
        Close(fh);
        return RETURN_ERROR;
    }
    Write(fh, "OK: expansion.library opened\n", 29);

    while ((cd = FindConfigDev(cd, -1, -1)) != NULL) {
        char buf[120];
        LONG len;
        len = sprintf(buf, "Board: manu=%ld prod=%ld at $%08lX\n",
                      (LONG)cd->cd_Rom.er_Manufacturer,
                      (LONG)cd->cd_Rom.er_Product,
                      (ULONG)cd->cd_BoardAddr);
        Write(fh, buf, len);

        if (cd->cd_Rom.er_Manufacturer == 514 &&
            cd->cd_Rom.er_Product == 112)
        {
            volatile UWORD *rap, *rdp;
            UWORD v;

            found = 1;
            Write(fh, "FOUND A2065!\n", 13);

            rap = (volatile UWORD *)(cd->cd_BoardAddr + 0x4002);
            rdp = (volatile UWORD *)(cd->cd_BoardAddr + 0x4000);

            len = sprintf(buf, "RAP=$%08lX RDP=$%08lX\n",
                          (ULONG)rap, (ULONG)rdp);
            Write(fh, buf, len);

            Write(fh, "Writing RAP=0...\n", 17);
            *rap = 0x0000;
            Write(fh, "Reading RDP (CSR0)...\n", 22);
            v = *rdp;
            len = sprintf(buf, "CSR0 = $%04X\n", (ULONG)v);
            Write(fh, buf, len);

            Write(fh, "Writing RAP=1...\n", 17);
            *rap = 0x0001;
            Write(fh, "Reading RAP...\n", 15);
            v = *rap;
            len = sprintf(buf, "RAP = $%04X\n", (ULONG)v);
            Write(fh, buf, len);

            Write(fh, "DONE\n", 5);
        }
    }

    if (!found) {
        Write(fh, "A2065 NOT found\n", 16);
    }

    CloseLibrary((struct Library *)eb);
    Write(fh, "=== END ===\n", 13);
    Close(fh);
    return RETURN_OK;
}

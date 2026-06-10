#include <exec/types.h>
#include <libraries/expansion.h>
#include <libraries/expansionbase.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>
#include <dos/dos.h>

#include <stdio.h>
#include <string.h>

struct ExpansionBase *ExpansionBase;

int main(void)
{
    struct ConfigDev *cd = NULL;
    BPTR fh;
    char buf[120];

    fh = Open("ram:vbcc_diag.txt", MODE_NEWFILE);
    if (!fh) return RETURN_ERROR;

    Write(fh, "start\n", 6);

    while ((cd = FindConfigDev(cd, -1, -1)) != NULL) {
        int len = sprintf(buf, "m=%ld p=%ld addr=$%08lx\n",
                          (LONG)cd->cd_Rom.er_Manufacturer,
                          (LONG)cd->cd_Rom.er_Product,
                          (ULONG)cd->cd_BoardAddr);
        Write(fh, buf, len);

        if (cd->cd_Rom.er_Manufacturer == 514 &&
            cd->cd_Rom.er_Product == 112)
        {
            volatile UWORD *rap, *rdp;
            UWORD v;
            int len2;

            Write(fh, "FOUND A2065!\n", 13);

            rap = (volatile UWORD *)((ULONG)cd->cd_BoardAddr + 0x4002);
            rdp = (volatile UWORD *)((ULONG)cd->cd_BoardAddr + 0x4000);

            *rap = 0x0000;
            v = *rdp;
            len2 = sprintf(buf, "CSR0=$%04x\n", v);
            Write(fh, buf, len2);

            *rap = 0x0001;
            v = *rap;
            len2 = sprintf(buf, "RAP=$%04x\n", v);
            Write(fh, buf, len2);

            Write(fh, "DONE\n", 5);
        }
    }

    Write(fh, "end\n", 4);
    Close(fh);
    return RETURN_OK;
}

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

    fh = Open("ram:vbcc_enum.txt", MODE_NEWFILE);
    if (!fh) return RETURN_ERROR;

    Write(fh, "start\n", 6);

    while ((cd = FindConfigDev(cd, -1, -1)) != NULL) {
        int len = sprintf(buf, "m=%ld p=%ld addr=$%08lx\n",
                          (LONG)cd->cd_Rom.er_Manufacturer,
                          (LONG)cd->cd_Rom.er_Product,
                          (ULONG)cd->cd_BoardAddr);
        Write(fh, buf, len);
    }

    Write(fh, "end\n", 4);
    Close(fh);
    return RETURN_OK;
}

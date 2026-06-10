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

static int pass = 0, fail = 0;

int main(void)
{
    struct ConfigDev *cd = NULL;
    BPTR fh;
    char buf[120];
    int len;

    fh = Open("ram:diag4.txt", MODE_NEWFILE);
    if (!fh) return RETURN_ERROR;

    len = sprintf(buf, "start\n");
    Write(fh, buf, len);

    if (!ExpansionBase) {
        len = sprintf(buf, "FAIL: ExpansionBase is NULL\n");
        Write(fh, buf, len);
        Close(fh);
        return RETURN_ERROR;
    }

    len = sprintf(buf, "ExpansionBase OK\n");
    Write(fh, buf, len);

    while ((cd = FindConfigDev(cd, -1, -1)) != NULL) {
        len = sprintf(buf, "m=%ld p=%ld addr=$%08lX\n",
                      (LONG)cd->cd_Rom.er_Manufacturer,
                      (LONG)cd->cd_Rom.er_Product,
                      (ULONG)cd->cd_BoardAddr);
        Write(fh, buf, len);
    }

    len = sprintf(buf, "end\n");
    Write(fh, buf, len);
    Close(fh);
    return RETURN_OK;
}

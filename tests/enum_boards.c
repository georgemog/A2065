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

#ifndef __CLIB2__
#undef ExpansionBase
#endif
struct ExpansionBase *ExpansionBase;

int main(void)
{
    struct Library *expansion_lib;
    struct ExpansionBase *eb;
    struct ConfigDev *cd = NULL;
    BPTR fh;
    char buf[120];

    fh = Open("ram:enum.txt", MODE_NEWFILE);
    if (!fh) return RETURN_ERROR;

    FWrite(fh, "start\n", 1, 6);

    expansion_lib = OpenLibrary("expansion.library", 0);
    if (!expansion_lib) {
        FWrite(fh, "no expansion\n", 1, 13);
        Close(fh);
        return RETURN_ERROR;
    }
    eb = (struct ExpansionBase *)expansion_lib;
    FWrite(fh, "expansion ok\n", 1, 13);

    while ((cd = FindConfigDev(cd, -1, -1)) != NULL) {
        int len = sprintf(buf, "m=%ld p=%ld addr=$%08lX\n",
                          (LONG)cd->cd_Rom.er_Manufacturer,
                          (LONG)cd->cd_Rom.er_Product,
                          (ULONG)cd->cd_BoardAddr);
        FWrite(fh, buf, 1, len);
    }

    FWrite(fh, "end\n", 1, 4);
    Close(fh);
    CloseLibrary(expansion_lib);
    return RETURN_OK;
}

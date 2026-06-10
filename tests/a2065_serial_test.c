#include <exec/types.h>
#include <libraries/expansion.h>
#include <libraries/expansionbase.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>
#include <dos/dos.h>

#include <stdio.h>
#include <string.h>

static void raw_serial(const char *s)
{
    volatile UWORD *serdat = (volatile UWORD *)0xDFF0030;
    volatile UWORD *serper = (volatile UWORD *)0xDFF0032;
    volatile UWORD *serdatr = (volatile UWORD *)0xDFF0018;

    *serper = 124;

    while (*s) {
        while (!(*serdatr & 0x2000))
            ;
        *serdat = (UWORD)*s | 0x100;
        s++;
    }
}

int main(void)
{
    struct ExpansionBase *eb = NULL;
    struct ConfigDev *cd = NULL;

    raw_serial("=== A2065 SERIAL TEST ===\r\n");

    eb = (struct ExpansionBase *)OpenLibrary("expansion.library", 0);
    if (!eb) {
        raw_serial("FAIL: cannot open expansion.library\r\n");
        return RETURN_ERROR;
    }
    raw_serial("OK: expansion.library opened\r\n");

    while ((cd = FindConfigDev(cd, -1, -1)) != NULL) {
        char buf[80];
        sprintf(buf, "Board: manu=%ld prod=%ld at $%08lX\r\n",
                (LONG)cd->cd_Rom.er_Manufacturer,
                (LONG)cd->cd_Rom.er_Product,
                (ULONG)cd->cd_BoardAddr);
        raw_serial(buf);

        if (cd->cd_Rom.er_Manufacturer == 514 &&
            cd->cd_Rom.er_Product == 112)
        {
            volatile UWORD *rap = (volatile UWORD *)(cd->cd_BoardAddr + 0x4002);
            volatile UWORD *rdp = (volatile UWORD *)(cd->cd_BoardAddr + 0x4000);
            char buf2[80];

            raw_serial("FOUND A2065!\r\n");

            sprintf(buf2, "Board addr = $%08lX\r\n", (ULONG)cd->cd_BoardAddr);
            raw_serial(buf2);

            *rap = 0x0000;
            raw_serial("Wrote RAP=0\r\n");

            {
                UWORD v = *rdp;
                sprintf(buf2, "Read RDP (CSR0) = $%04X\r\n", (ULONG)v);
                raw_serial(buf2);
            }

            *rap = 0x0001;
            raw_serial("Wrote RAP=1\r\n");

            {
                UWORD v = *rap;
                sprintf(buf2, "Read RAP = $%04X\r\n", (ULONG)v);
                raw_serial(buf2);
            }

            raw_serial("DONE\r\n");
        }
    }

    if (!cd) {
        raw_serial("No A2065 found in board list\r\n");
    }

    CloseLibrary((struct Library *)eb);
    raw_serial("=== END ===\r\n");
    return RETURN_OK;
}

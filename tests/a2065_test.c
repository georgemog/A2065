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
    int found = 0;

    if (!ExpansionBase)
    {
        printf("[ERROR] Cannot open expansion.library!\n");
        return FALSE;
    }
    printf("[DEBUG] expansion.library opened at $%08lx\n", (ULONG)ExpansionBase);

    printf("[DEBUG] Scanning expansion boards for A2065...\n");

    while ((cd = FindConfigDev(cd, -1, -1)) != NULL)
    {
        printf("[DEBUG]   Board at $%08lx: mfr=$%04x prod=$%04x size=%ld\n",
               (ULONG)cd->cd_BoardAddr,
               (UWORD)cd->cd_Rom.er_Manufacturer,
               (UWORD)cd->cd_Rom.er_Product,
               (ULONG)cd->cd_BoardSize);

        if (cd->cd_Rom.er_Manufacturer == 514 &&
            cd->cd_Rom.er_Product == 112)
        {
            found = 1;
            break;
        }
    }

    if (!found)
    {
        printf("[ERROR] A2065 not found in expansion board list!\n");
        return FALSE;
    }

    printf("[DEBUG] Found A2065 at physical $%08lx (%ld bytes)\n",
           (ULONG)cd->cd_BoardAddr, (ULONG)cd->cd_BoardSize);

    rap = (volatile UWORD *)(cd->cd_BoardAddr + 0x4002);
    rdp = (volatile UWORD *)(cd->cd_BoardAddr + 0x4000);

    printf("[DEBUG] RAP mapped to $%08lx\n", (ULONG)rap);
    printf("[DEBUG] RDP mapped to $%08lx\n", (ULONG)rdp);

    return TRUE;
}

int main(void)
{
    UWORD val;
    int i;

    printf("=== A2065 DDR3 Mailbox Test ===\n\n");

    if (!find_a2065())
    {
        printf("\nTest FAILED: could not locate A2065\n");
        return RETURN_ERROR;
    }

    printf("\n--- Test 1: Read RAP (should return 0 or last value) ---\n");
    printf("[DEBUG] About to read RAP at $%08lx...\n", (ULONG)rap);
    val = *rap;
    printf("[DEBUG] RAP read returned: $%04x\n", (ULONG)val);
    printf("Test 1 result: RAP = $%04x\n", (ULONG)val);

    Delay(50);

    printf("\n--- Test 2: Write 0x0000 to RAP (select CSR0) ---\n");
    printf("[DEBUG] About to write $0000 to RAP at $%08lx...\n", (ULONG)rap);
    *rap = 0x0000;
    printf("[DEBUG] Write completed (no bus error = good)\n");

    Delay(50);

    printf("\n--- Test 3: Read RDP (CSR0 value) ---\n");
    printf("[DEBUG] About to read RDP at $%08lx...\n", (ULONG)rdp);
    val = *rdp;
    printf("[DEBUG] RDP read returned: $%04x\n", (ULONG)val);
    printf("Test 3 result: CSR0 = $%04x\n", (ULONG)val);
    if (val == 0)
        printf("  WARNING: CSR0 is $0000 (ARM daemon may not be responding)\n");
    else
        printf("  CSR0 bits: STOP=%d STRT=%d INIT=%d ERR=%d\n",
               (val >> 2) & 1, (val >> 1) & 1, val & 1, (val >> 15) & 1);

    Delay(50);

    printf("\n--- Test 4: Write 0x0001 to RAP (select CSR1) ---\n");
    *rap = 0x0001;
    printf("[DEBUG] Wrote $0001 to RAP\n");

    Delay(50);

    printf("\n--- Test 5: Read RDP (CSR1 value) ---\n");
    val = *rdp;
    printf("[DEBUG] RDP read returned: $%04x\n", (ULONG)val);
    printf("Test 5 result: CSR1 = $%04x\n", (ULONG)val);

    Delay(50);

    printf("\n--- Test 6: Rapid read RAP 5 times ---\n");
    for (i = 0; i < 5; i++)
    {
        val = *rap;
        printf("  Read %d: RAP = $%04x\n", i + 1, (ULONG)val);
        Delay(5);
    }

    printf("\n=== Test complete ===\n");
    printf("If all reads returned non-zero, DDR3 mailbox is working!\n");
    printf("If all reads returned $0000, ARM daemon may not be running.\n");

    if (ExpansionBase)
        CloseLibrary((struct Library *)ExpansionBase);

    return RETURN_OK;
}

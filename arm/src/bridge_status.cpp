/*
 * bridge_status.cpp — Diagnostic tool for A2065 HPS2FPGA bridge
 *
 * Reads and displays bridge register state, probes boardram,
 * and optionally polls for FPGA requests.
 *
 * IMPORTANT: On Cyclone V HPS, unmapped Avalon slaves cause infinite bus
 * stalls (not recoverable — hard lockup).  Signal handlers CANNOT catch
 * this.  We use an altera bridge-enable check and a --force flag as safety.
 *
 * Usage: bridge_status [--bridge-base 0xFF20XXXX] [--poll N] [--force]
 */

#include "a2065_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>

#define BRIDGE_MGR_PHYS    0xFF400000UL
#define BRIDGE_MGR_SIZE    0x1000UL
#define BRIDGE_MGR_CTL_OFF 0x00

static uint32_t phys_base = BRIDGE_PHYS_BASE;
static int      opt_force = 0;

static int check_bridge_mgr(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) return -1;

    void *p = mmap(NULL, BRIDGE_MGR_SIZE, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, (off_t)BRIDGE_MGR_PHYS);
    if (p == MAP_FAILED) {
        close(fd);
        return -1;
    }

    volatile uint32_t *ctl = (volatile uint32_t *)((uint8_t *)p + BRIDGE_MGR_CTL_OFF);
    uint32_t val = *ctl;
    munmap(p, BRIDGE_MGR_SIZE);
    close(fd);
    return (int)val;
}

static volatile uint8_t *do_mmap(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem");
        return NULL;
    }

    void *p = mmap(NULL, BRIDGE_WINDOW_SIZE, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, (off_t)phys_base);
    if (p == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return NULL;
    }

    fprintf(stderr, "[bridge_status] mapped phys 0x%08X (size 0x%X) at virt %p\n",
            phys_base, BRIDGE_WINDOW_SIZE, p);
    return (volatile uint8_t *)p;
}

static void dump_regs(volatile uint8_t *base)
{
    uint16_t data    = BRIDGE_READ16(base, BRIDGE_REG_DATA);
    uint8_t  addr    = BRIDGE_READ8(base,  BRIDGE_REG_ADDR);
    uint8_t  rw      = BRIDGE_READ8(base,  BRIDGE_REG_RW);
    uint8_t  new_req = BRIDGE_READ8(base,  BRIDGE_REG_NEW_REQ);
    uint8_t  done    = BRIDGE_READ8(base,  BRIDGE_REG_DONE);
    uint16_t result  = BRIDGE_READ16(base, BRIDGE_REG_RESULT);

    printf("=== Bridge Registers ===\n");
    printf("  DATA    [0x00] = 0x%04X\n", data);
    printf("  ADDR    [0x02] = 0x%02X  (%s)\n", addr,
           addr == 0 ? "RDP" : addr == 2 ? "RAP" : "???");
    printf("  RW      [0x03] = 0x%02X  (%s)\n", rw,
           rw ? "write" : "read");
    printf("  NEW_REQ [0x04] = 0x%02X  (%s)\n", new_req,
           new_req ? "PENDING" : "idle");
    printf("  DONE    [0x05] = 0x%02X\n", done);
    printf("  RESULT  [0x06] = 0x%04X\n", result);
    printf("\n");
}

static void dump_mac_shadow(volatile uint8_t *base)
{
    printf("=== MAC Shadow [0x08–0x0D] ===\n  ");
    for (int i = 0; i < 6; i++)
        printf("%02X%c", BRIDGE_READ8(base, BRIDGE_MAC_BASE + i),
               i < 5 ? ':' : '\n');
    printf("\n");
}

static void dump_int_regs(volatile uint8_t *base)
{
    printf("=== Interrupt Registers ===\n");
    printf("  INT_SET [0x10] = 0x%02X\n", BRIDGE_READ8(base, BRIDGE_INT_SET));
    printf("  INT_CLR [0x11] = 0x%02X\n", BRIDGE_READ8(base, BRIDGE_INT_CLR));
    printf("\n");
}

static void probe_boardram(volatile uint8_t *base)
{
    volatile uint8_t *ram = BRIDGE_BOARDRAM(base);

    printf("=== Boardram Probe [0x8000–0xFFFF] ===\n");

    uint8_t saved0 = ram[0];
    uint8_t saved1 = ram[1];

    ram[0] = 0xA5;
    ram[1] = 0x5A;
    __sync_synchronize();

    uint8_t r0 = ram[0];
    uint8_t r1 = ram[1];

    if (r0 == 0xA5 && r1 == 0x5A) {
        printf("  Write/readback: OK (wrote A5 5A, read A5 5A)\n");
    } else {
        printf("  Write/readback: FAIL (wrote A5 5A, read %02X %02X)\n", r0, r1);
    }

    ram[0] = saved0;
    ram[1] = saved1;

    printf("  First 16 bytes:");
    for (int i = 0; i < 16; i++)
        printf(" %02X", ram[i]);
    printf("\n\n");
}

static void poll_requests(volatile uint8_t *base, int count)
{
    printf("=== Polling for NEW_REQ (up to %d reads) ===\n", count);
    int seen = 0;
    for (int i = 0; i < count; i++) {
        uint8_t req = BRIDGE_READ8(base, BRIDGE_REG_NEW_REQ);
        if (req) {
            seen++;
            uint16_t data = BRIDGE_READ16(base, BRIDGE_REG_DATA);
            uint8_t  addr = BRIDGE_READ8(base,  BRIDGE_REG_ADDR);
            uint8_t  rw   = BRIDGE_READ8(base,  BRIDGE_REG_RW);
            printf("  [%d] NEW_REQ=1  addr=0x%02X(%s) rw=%d(%s) data=0x%04X\n",
                   i, addr, addr==0?"RDP":addr==2?"RAP":"???",
                   rw, rw?"write":"read", data);
        }
        usleep(10);
    }
    if (seen == 0)
        printf("  No requests seen.\n");
    printf("\n");
}

int main(int argc, char *argv[])
{
    int poll_count = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bridge-base") && i + 1 < argc)
            phys_base = (uint32_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--poll") && i + 1 < argc)
            poll_count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--force"))
            opt_force = 1;
    }

    printf("A2065 Bridge Status — phys 0x%08X\n\n", phys_base);

    printf("Checking HPS2FPGA bridge manager (0xFF400000)...\n");
    int mgr = check_bridge_mgr();
    if (mgr < 0) {
        printf("  Cannot read bridge manager — need root?\n");
        return 1;
    }
    printf("  Bridge manager CTL = 0x%08X\n", (uint32_t)mgr);
    if (!(mgr & 1)) {
        printf("  WARNING: HPS2FPGA bridge is NOT enabled (bit 0 clear).\n");
        printf("  On MiSTer, the main core usually enables this at boot.\n");
        if (!opt_force) {
            printf("  Refusing to probe — would cause hard lockup.\n");
            printf("  Use --force to override (DANGEROUS).\n");
            return 1;
        }
        printf("  --force given, proceeding anyway...\n");
    } else {
        printf("  HPS2FPGA bridge is enabled.\n");
    }

    printf("\n  NOTE: Even with bridge enabled, accessing addresses that have no\n");
    printf("  Avalon slave will cause a hard lockup. This tool assumes the A2065\n");
    printf("  slave is configured at 0x%08X. If not, do NOT run this.\n\n", phys_base);

    volatile uint8_t *base = do_mmap();
    if (!base) {
        fprintf(stderr, "Failed to map bridge at 0x%08X\n", phys_base);
        return 1;
    }

    dump_regs(base);
    dump_mac_shadow(base);
    dump_int_regs(base);
    probe_boardram(base);

    if (poll_count > 0)
        poll_requests(base, poll_count);

    printf("=== Summary ===\n");
    uint8_t new_req = BRIDGE_READ8(base, BRIDGE_REG_NEW_REQ);
    uint8_t done    = BRIDGE_READ8(base, BRIDGE_REG_DONE);
    printf("  Bridge accessible: YES\n");
    printf("  NEW_REQ active: %s\n", new_req ? "YES" : "no");
    printf("  DONE active: %s\n", done ? "YES" : "no");

    if (new_req == 0 && done == 0) {
        printf("\n  The bridge registers read back but show no FPGA activity.\n");
        printf("  This likely means the FPGA-side bridge wiring is not yet\n");
        printf("  connected (bridge_done/bridge_result still tied to zeros).\n");
    }

    munmap((void *)base, BRIDGE_WINDOW_SIZE);
    return 0;
}

/*
 * ddr3_scan.cpp — Scan ARM physical address space to find A2065 DDR3 magic.
 * The FPGA writes {0xA2065A20, 0x65A20650} at f2sdram2 address 0.
 * Scans every 1MB across the entire 1GB DDR3 range.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define MAGIC_HI  0xA2065A20UL
#define MAGIC_LO  0x65A20650UL

int try_read(int fd, unsigned long phys) {
    volatile uint8_t *map = (volatile uint8_t *)mmap(
        NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, phys);
    if (map == MAP_FAILED) return -1;

    uint32_t lo = *(volatile uint32_t *)(map);
    uint32_t hi = *(volatile uint32_t *)(map + 4);

    if (lo == MAGIC_LO && hi == MAGIC_HI) {
        uint32_t counter = *(volatile uint32_t *)(map + 8);
        printf("\n  *** FOUND at phys 0x%08lX!  counter=%u ***\n",
               phys, counter);
        munmap((void *)map, 0x10000);
        return 0;
    }

    munmap((void *)map, 0x10000);
    return 1;
}

int main(void) {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }

    printf("Scanning for A2065 DDR3 magic {0x%08X, 0x%08X}...\n", MAGIC_HI, MAGIC_LO);
    printf("Scanning 0x00000000-0x3F000000 at 1MB steps (1024 pages)...\n\n");

    int found = 0;
    for (unsigned long addr = 0x00000000; addr < 0x40000000; addr += 0x100000) {
        int r = try_read(fd, addr);
        if (r == 0) { found = 1; break; }
        if (r < 0) continue;
        if ((addr & 0x0F000000) == 0) {
            printf("  0x%08lX ...\n", addr);
            fflush(stdout);
        }
    }

    if (!found) printf("\nMagic NOT found.\n");
    close(fd);
    return found ? 0 : 1;
}

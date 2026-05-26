/*
 * sdram_map.cpp — Read HPS SDRAM controller registers to find f2sdram
 * address mapping. These registers are in the HPS peripheral space
 * (0xFFC20000), NOT in the FPGA bridge window, so they're safe to access.
 *
 * This tells us which addresses in the hps2fpga window go to f2sdram
 * (SDRAM) and which go to the AXI master port (our slave).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define SDR_CTRL_BASE   0xFFC20000UL
#define SDR_CTRL_SIZE   0x1000

#define SDR_CTRLGRP_F2SDRAM_MPU_REGION_BASE_0     0x5080
#define SDR_CTRLGRP_F2SDRAM_MPU_REGION_MASK_0     0x5084
#define SDR_CTRLGRP_F2SDRAM_MPU_REGION_BASE_1     0x5088
#define SDR_CTRLGRP_F2SDRAM_MPU_REGION_MASK_1     0x508C
#define SDR_CTRLGRP_F2SDRAM_MPU_REGION_BASE_2     0x5090
#define SDR_CTRLGRP_F2SDRAM_MPU_REGION_MASK_2     0x5094
#define SDR_CTRLGRP_F2SDRAM_MPU_REGION_BASE_3     0x5098
#define SDR_CTRLGRP_F2SDRAM_MPU_REGION_MASK_3     0x509C

#define HPS2FPGA_BASE  0xC0000000UL
#define HPS2FPGA_SIZE  0x20000000UL

static uint32_t read_reg(volatile uint8_t *map, unsigned offset) {
    return *(volatile uint32_t *)(map + offset);
}

int main(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }

    volatile uint8_t *map = (volatile uint8_t *)mmap(
        NULL, SDR_CTRL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
        fd, SDR_CTRL_BASE);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    printf("=== Cyclone V HPS SDRAM Controller f2sdram Address Map ===\n\n");
    printf("hps2fpga bridge window: 0x%08lX - 0x%08lX (512MB)\n\n",
           HPS2FPGA_BASE, HPS2FPGA_BASE + HPS2FPGA_SIZE - 1);

    struct { const char *name; unsigned base_off; unsigned mask_off; } regions[] = {
        { "f2sdram port 0 (vbuf)",   SDR_CTRLGRP_F2SDRAM_MPU_REGION_BASE_0, SDR_CTRLGRP_F2SDRAM_MPU_REGION_MASK_0 },
        { "f2sdram port 1 (ram1)",   SDR_CTRLGRP_F2SDRAM_MPU_REGION_BASE_1, SDR_CTRLGRP_F2SDRAM_MPU_REGION_MASK_1 },
        { "f2sdram port 2 (ram2)",   SDR_CTRLGRP_F2SDRAM_MPU_REGION_BASE_2, SDR_CTRLGRP_F2SDRAM_MPU_REGION_MASK_2 },
        { "f2sdram port 3",          SDR_CTRLGRP_F2SDRAM_MPU_REGION_BASE_3, SDR_CTRLGRP_F2SDRAM_MPU_REGION_MASK_3 },
    };

    for (int i = 0; i < 4; i++) {
        uint32_t base = read_reg(map, regions[i].base_off);
        uint32_t mask = read_reg(map, regions[i].mask_off);
        printf("%s:\n", regions[i].name);
        printf("  base register: 0x%08X\n", base);
        printf("  mask register: 0x%08X\n", mask);
        if (base & 1) {
            uint32_t addr_base  = base & ~0xFFF;
            uint32_t addr_mask  = mask & ~0xFFF;
            uint32_t size       = ~addr_mask + 1;
            printf("  -> ENABLED: DDR3 addr 0x%08X, size %uMB\n",
                   addr_base, size / (1024*1024));
            printf("  -> ARM phys:  0x%08lX - 0x%08lX\n",
                   HPS2FPGA_BASE + (unsigned long)addr_base,
                   HPS2FPGA_BASE + (unsigned long)addr_base + size - 1);
        } else {
            printf("  -> DISABLED\n");
        }
        printf("\n");
    }

    printf("=== Raw register dump ===\n");
    for (unsigned off = 0x5080; off <= 0x509C; off += 4) {
        printf("  0x%04X: 0x%08X\n", off, read_reg(map, off));
    }

    munmap((void *)map, SDR_CTRL_SIZE);
    close(fd);
    return 0;
}

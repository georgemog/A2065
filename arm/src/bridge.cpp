/*
 * bridge.cpp — /dev/mem mmap for HPS2FPGA bridge access
 *
 * Maps the HPS2FPGA lightweight bridge window so ARM can access
 * both the chip register bridge (8 bytes) and boardram (32KB).
 */

#include "a2065_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

static void *bridge_mapping = NULL;
static size_t bridge_mapped_size = 0;
static int    bridge_fd = -1;

volatile uint8_t *bridge_do_mmap(uint32_t base, uint32_t size, int *out_fd)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("[a2065] open /dev/mem");
        return NULL;
    }

    void *mapped = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)base);
    if (mapped == MAP_FAILED) {
        perror("[a2065] mmap");
        close(fd);
        return NULL;
    }

    bridge_mapping    = mapped;
    bridge_mapped_size = size;
    bridge_fd         = fd;
    if (out_fd) *out_fd = fd;

    fprintf(stderr, "[a2065] bridge mapped: phys=0x%08X size=0x%X virt=%p\n",
            base, size, mapped);
    return (volatile uint8_t *)mapped;
}

void bridge_unmap(void)
{
    if (bridge_mapping && bridge_mapping != MAP_FAILED) {
        munmap(bridge_mapping, bridge_mapped_size);
        bridge_mapping = NULL;
    }
    if (bridge_fd >= 0) {
        close(bridge_fd);
        bridge_fd = -1;
    }
}

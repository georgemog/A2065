#include "a2065_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

static void *sim_mapping = NULL;
static size_t sim_size = 0;
static int sim_fd = -1;

volatile uint8_t *bridge_sim_open(uint32_t size)
{
    sim_fd = shm_open("a2065_bridge", O_RDWR | O_CREAT, 0666);
    if (sim_fd < 0) {
        perror("[a2065] shm_open");
        return NULL;
    }

    if (ftruncate(sim_fd, (off_t)size) < 0) {
        perror("[a2065] ftruncate");
        close(sim_fd); sim_fd = -1;
        return NULL;
    }

    sim_mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, sim_fd, 0);
    if (sim_mapping == MAP_FAILED) {
        perror("[a2065] mmap sim");
        close(sim_fd); sim_fd = -1;
        return NULL;
    }

    memset(sim_mapping, 0, size);
    sim_size = size;

    fprintf(stderr, "[a2065] sim bridge: shm=/dev/shm/a2065_bridge size=0x%X\n", size);
    return (volatile uint8_t *)sim_mapping;
}

void bridge_sim_close(void)
{
    if (sim_mapping && sim_mapping != MAP_FAILED) {
        munmap(sim_mapping, sim_size);
        sim_mapping = NULL;
    }
    if (sim_fd >= 0) {
        close(sim_fd);
        shm_unlink("a2065_bridge");
        sim_fd = -1;
    }
}

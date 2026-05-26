#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define DDR3_BASE    0x1FF00000UL
#define MBX_REQ_OFF  0x8000
#define MBX_RSP_OFF  0x8008
#define MAP_SIZE     0x10000

static volatile uint8_t *map;
static int fd;

static uint64_t rd64(unsigned long off) {
    uint64_t val;
    memcpy(&val, (void *)(map + off), 8);
    return val;
}

static void wr64(unsigned long off, uint64_t val) {
    memcpy((void *)(map + off), &val, 8);
}

int main(void) {
    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }

    map = (volatile uint8_t *)mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fd, DDR3_BASE);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    printf("A2065 DDR3 mailbox bridge daemon\n");
    printf("DDR3 base: 0x%08lX, mailbox req: +0x%X, rsp: +0x%X\n",
           DDR3_BASE, MBX_REQ_OFF, MBX_RSP_OFF);

    wr64(MBX_REQ_OFF, 0);
    wr64(MBX_RSP_OFF, 0);

    uint32_t count = 0;
    for (;;) {
        uint64_t req = rd64(MBX_REQ_OFF);
        if (req & 1) {
            int rw       = (req >> 1) & 1;
            uint16_t addr = (req >> 2) & 0xFF;
            uint16_t data = (req >> 10) & 0xFFFF;

            count++;
            if (rw)
                printf("[%u] WRITE  addr=0x%04X data=0x%04X\n", count, addr, data);
            else
                printf("[%u] READ   addr=0x%04X\n", count, addr);

            uint64_t rsp = 0;
            if (!rw) {
                uint16_t result = 0x0004;
                if (addr == 2) {
                    if (data == 0)
                        result = 0x0004;
                    else
                        result = 0x0115;
                }
                rsp = 1 | ((uint64_t)result << 1);
            } else {
                rsp = 1;
            }

            wr64(MBX_RSP_OFF, rsp);
            wr64(MBX_REQ_OFF, 0);
            usleep(1000);
        }
        usleep(100);
    }

    munmap((void *)map, MAP_SIZE);
    close(fd);
    return 0;
}

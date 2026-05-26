#include "a2065_types.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

static volatile uint8_t *map = NULL;
static int fd = -1;
static void (*reg_service_cb)(void) = NULL;

#define DDR3_BASE    0x1FF00000UL
#define MBX_RAM_REQ  0x8010
#define MBX_RAM_RSP  0x8018

static uint64_t rd64(unsigned long off) {
    uint64_t val;
    memcpy(&val, (void *)(map + off), 8);
    return val;
}

static void wr64(unsigned long off, uint64_t val) {
    memcpy((void *)(map + off), &val, 8);
}

int boardram_remote_init(volatile uint8_t *shared_map)
{
    map = shared_map;
    return 0;
}

extern "C" void boardram_set_reg_service(void (*cb)(void))
{
    reg_service_cb = cb;
}

static uint16_t boardram_xfer(uint16_t off, int write, uint16_t wdata)
{
    uint64_t req = 1 | ((uint64_t)write << 1)
                 | ((uint64_t)(off & 0x7FFE) << 2)
                 | ((uint64_t)wdata << 17);

    for (int attempt = 0; attempt < 3; attempt++) {
        wr64(MBX_RAM_REQ, req);
        __sync_synchronize();
        rd64(MBX_RAM_REQ);

        for (int i = 0; i < 10000; i++) {
            uint64_t rsp = rd64(MBX_RAM_RSP);
            if (rsp & 1) {
                wr64(MBX_RAM_RSP, 0);
                wr64(MBX_RAM_REQ, 0);
                return (uint16_t)(rsp >> 1);
            }
            if (reg_service_cb) reg_service_cb();
        }
    }

    wr64(MBX_RAM_REQ, 0);
    fprintf(stderr, "[boardram] timeout off=0x%04X rw=%d\n", off, write);
    return 0xFFFF;
}

extern "C" uint8_t  boardram_rb(uint32_t off) {
    uint16_t w = boardram_xfer(off & 0x7FFE, 0, 0);
    return (off & 1) ? (uint8_t)(w & 0xFF) : (uint8_t)(w >> 8);
}

extern "C" uint16_t boardram_rw(uint32_t off) {
    return boardram_xfer(off & 0x7FFE, 0, 0);
}

extern "C" void boardram_wb(uint32_t off, uint8_t val) {
    uint16_t old = boardram_xfer(off & 0x7FFE, 0, 0);
    uint16_t nw = (off & 1) ? ((old & 0xFF00) | val) : ((val << 8) | (old & 0x00FF));
    boardram_xfer(off & 0x7FFE, 1, nw);
}

extern "C" void boardram_ww(uint32_t off, uint16_t val) {
    boardram_xfer(off & 0x7FFE, 1, val);
}

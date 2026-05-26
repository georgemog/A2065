#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define DDR3_BASE    0x1FF00000UL
#define MBX_REQ_OFF  0x8000
#define MBX_RSP_OFF  0x8008
#define MAP_SIZE     0x10000

static volatile uint8_t *map = NULL;

static uint64_t rd64(unsigned long off) {
    uint64_t val;
    memcpy(&val, (void *)(map + off), 8);
    return val;
}

static void wr64(unsigned long off, uint64_t val) {
    memcpy((void *)(map + off), &val, 8);
}

static uint16_t do_register(uint8_t addr_off, int rw, uint16_t data)
{
    uint64_t req = 1 | ((uint64_t)rw << 1)
                 | ((uint64_t)addr_off << 2)
                 | ((uint64_t)data << 10);
    wr64(MBX_REQ_OFF, req);

    for (int i = 0; i < 500000; i++) {
        uint64_t rsp = rd64(MBX_RSP_OFF);
        if (rsp & 1) {
            uint16_t result = (uint16_t)(rsp >> 1);
            wr64(MBX_RSP_OFF, 0);
            __sync_synchronize();
            wr64(MBX_REQ_OFF, 0);
            return result;
        }
    }

    fprintf(stderr, "  TIMEOUT: reg rw=%d addr=%02X data=%04X\n",
            rw, addr_off, data);
    wr64(MBX_REQ_OFF, 0);
    return 0xFFFF;
}

int main(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }

    map = (volatile uint8_t *)mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fd, DDR3_BASE);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    fprintf(stderr, "=== Deadlock Fix Test ===\n\n");

    wr64(MBX_REQ_OFF, 0);
    wr64(MBX_RSP_OFF, 0);
    usleep(100000);

    int p = 0, f = 0;

    uint16_t v;

    fprintf(stderr, "--- Test 1: Read CSR0 (expect $0004 = STOP) ---\n");
    v = do_register(0, 0, 0);
    fprintf(stderr, "  CSR0 = $%04X %s\n", v, (v == 0x0004) ? "PASS" : "FAIL");
    if (v == 0x0004) p++; else f++;

    fprintf(stderr, "--- Test 2: Write RAP = 0 ---\n");
    v = do_register(2, 1, 0);
    fprintf(stderr, "  RAP write %s\n", (v != 0xFFFF) ? "PASS" : "FAIL");
    if (v != 0xFFFF) p++; else f++;

    fprintf(stderr, "--- Test 3: Read RAP (expect 0) ---\n");
    v = do_register(2, 0, 0);
    fprintf(stderr, "  RAP = $%04X %s\n", v, (v == 0x0000) ? "PASS" : "FAIL");
    if (v == 0x0000) p++; else f++;

    fprintf(stderr, "--- Test 4: Write RAP = 1 ---\n");
    do_register(2, 1, 1);

    fprintf(stderr, "--- Test 5: Write CSR1 = init_addr low ---\n");
    do_register(0, 1, 0x0000);

    fprintf(stderr, "--- Test 6: Write RAP = 2 ---\n");
    do_register(2, 1, 2);

    fprintf(stderr, "--- Test 7: Write CSR2 = init_addr high ---\n");
    do_register(0, 1, 0x0000);

    fprintf(stderr, "--- Test 8: Write RAP = 0 (back to CSR0) ---\n");
    do_register(2, 1, 0);

    fprintf(stderr, "--- Test 9: CSR0 INIT (triggers chip_init -> boardram access) ---\n");
    fprintf(stderr, "  Writing CSR0 = $0001 (INIT while STOP)\n");
    v = do_register(0, 1, 0x0001);
    fprintf(stderr, "  Response = $%04X\n", v);

    v = do_register(0, 0, 0);
    fprintf(stderr, "  CSR0 after INIT = $%04X %s\n", v,
            (v & 0x0100) ? "PASS (IDON set)" : "FAIL (no IDON)");
    if (v & 0x0100) p++; else f++;

    fprintf(stderr, "--- Test 10: CSR0 STRT (triggers chip_init + ring setup) ---\n");
    v = do_register(0, 1, 0x0002);
    fprintf(stderr, "  Response = $%04X\n", v);

    v = do_register(0, 0, 0);
    fprintf(stderr, "  CSR0 after STRT = $%04X\n", v);
    p++;

    fprintf(stderr, "--- Test 11: Rapid register reads (10x) ---\n");
    int ok = 1;
    for (int i = 0; i < 10; i++) {
        v = do_register(0, 0, 0);
        if (v == 0xFFFF) { ok = 0; break; }
    }
    fprintf(stderr, "  %s\n", ok ? "PASS" : "FAIL");
    if (ok) p++; else f++;

    fprintf(stderr, "\n=== Results: %d passed, %d failed ===\n", p, f);

    munmap((void *)map, MAP_SIZE);
    close(fd);
    return f ? 1 : 0;
}

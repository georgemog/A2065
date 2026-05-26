#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define DDR3_BASE    0x1FF00000UL
#define MBX_REQ_OFF  0x8000
#define MBX_RSP_OFF  0x8008
#define MBX_INT_OFF  0x8020
#define MAP_SIZE     0x10000

#define CSR0_STOP  0x0004
#define CSR0_STRT  0x0002
#define CSR0_INIT  0x0001
#define CSR0_INEA  0x0040
#define CSR0_INTR  0x0080
#define CSR0_IDON  0x0100
#define CSR0_TINT  0x0200
#define CSR0_RINT  0x0400

static volatile uint8_t *map = NULL;

static uint64_t rd64(unsigned long off) {
    uint64_t val;
    memcpy(&val, (void *)(map + off), 8);
    return val;
}

static void wr64(unsigned long off, uint64_t val) {
    memcpy((void *)(map + off), &val, 8);
}

static uint16_t do_reg(uint8_t addr_off, int rw, uint16_t data)
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

    fprintf(stderr, "  TIMEOUT: rw=%d addr=%02X data=%04X\n", rw, addr_off, data);
    wr64(MBX_REQ_OFF, 0);
    return 0xFFFF;
}

static void write_rap(uint16_t v) { do_reg(2, 1, v); }
static void write_csr0(uint16_t v) { write_rap(0); do_reg(0, 1, v); }
static uint16_t read_csr0(void) { write_rap(0); return do_reg(0, 0, 0); }

int main(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }

    map = (volatile uint8_t *)mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fd, DDR3_BASE);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    fprintf(stderr, "=== Interrupt Test ===\n\n");

    int p = 0, f = 0;

    fprintf(stderr, "--- Test 1: Reset with STOP ---\n");
    write_csr0(CSR0_STOP);
    usleep(5000);
    uint16_t v = read_csr0();
    fprintf(stderr, "  CSR0 = $%04X %s\n", v, (v == CSR0_STOP) ? "PASS" : "FAIL");
    if (v == CSR0_STOP) p++; else f++;

    fprintf(stderr, "--- Test 2: INT should be deasserted ---\n");
    usleep(5000);
    uint64_t int_val = rd64(MBX_INT_OFF);
    fprintf(stderr, "  MBX_INT = %llu %s\n", (unsigned long long)int_val,
            (int_val == 0) ? "PASS" : "FAIL");
    if (int_val == 0) p++; else f++;

    fprintf(stderr, "--- Test 3: Set init addr (CSR1=0, CSR2=0) ---\n");
    write_rap(1); do_reg(0, 1, 0x0000);
    write_rap(2); do_reg(0, 1, 0x0000);

    fprintf(stderr, "--- Test 4: INIT (triggers chip_init + IDON) ---\n");
    write_csr0(CSR0_INIT);
    usleep(5000);
    v = read_csr0();
    fprintf(stderr, "  CSR0 = $%04X (expect IDON set) %s\n", v,
            (v & CSR0_IDON) ? "PASS" : "FAIL");
    if (v & CSR0_IDON) p++; else f++;

    fprintf(stderr, "--- Test 5: INT should be 0 (INEA not set, no interrupt) ---\n");
    usleep(5000);
    int_val = rd64(MBX_INT_OFF);
    fprintf(stderr, "  MBX_INT = %llu %s\n", (unsigned long long)int_val,
            (int_val == 0) ? "PASS" : "FAIL");
    if (int_val == 0) p++; else f++;

    fprintf(stderr, "--- Test 6: STRT + INEA (enable interrupts + start) ---\n");
    write_csr0(CSR0_INEA | CSR0_STRT);
    usleep(10000);
    v = read_csr0();
    fprintf(stderr, "  CSR0 = $%04X\n", v);

    fprintf(stderr, "--- Test 7: INT should be 1 (IDON + INEA -> INTR) ---\n");
    usleep(5000);
    int_val = rd64(MBX_INT_OFF);
    fprintf(stderr, "  MBX_INT = %llu %s\n", (unsigned long long)int_val,
            (int_val & 1) ? "PASS (interrupt asserted)" : "FAIL (no interrupt)");
    if (int_val & 1) p++; else f++;

    fprintf(stderr, "--- Test 8: Clear IDON (write INEA|IDON to keep INEA, clear IDON) ---\n");
    write_csr0(CSR0_INEA | CSR0_IDON);
    usleep(10000);
    v = read_csr0();
    fprintf(stderr, "  CSR0 = $%04X (IDON should be clear) %s\n", v,
            (!(v & CSR0_IDON)) ? "PASS" : "FAIL");
    if (!(v & CSR0_IDON)) p++; else f++;

    fprintf(stderr, "--- Test 9: INT should be 0 (no interrupt-causing flags) ---\n");
    usleep(5000);
    int_val = rd64(MBX_INT_OFF);
    fprintf(stderr, "  MBX_INT = %llu %s\n", (unsigned long long)int_val,
            (int_val == 0) ? "PASS (interrupt deasserted)" : "FAIL (still asserted)");
    if (int_val == 0) p++; else f++;

    fprintf(stderr, "--- Test 10: STOP (full reset) ---\n");
    write_csr0(CSR0_STOP);
    usleep(5000);
    v = read_csr0();
    fprintf(stderr, "  CSR0 = $%04X %s\n", v, (v == CSR0_STOP) ? "PASS" : "FAIL");
    if (v == CSR0_STOP) p++; else f++;

    fprintf(stderr, "\n=== Results: %d passed, %d failed ===\n", p, f);

    munmap((void *)map, MAP_SIZE);
    close(fd);
    return f ? 1 : 0;
}

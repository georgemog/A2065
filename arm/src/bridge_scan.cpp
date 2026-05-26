/*
 * bridge_scan.cpp — Scan hps2fpga bridge window to find AXI slave.
 * Tries multiple addresses with SIGALRM timeout to avoid hard lockup.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>
#include <string.h>

#define BRIDGE_WINDOW_BASE 0xC0000000UL
#define BRIDGE_WINDOW_SIZE 0x20000000UL

static volatile sig_atomic_t got_alarm;

static void alarm_handler(int sig) { (void)sig; got_alarm = 1; }

int main(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = alarm_handler;
    sigaction(SIGALRM, &sa, NULL);

    unsigned long bases[] = {
        0xC0000000UL,
        0xC4000000UL,
        0xC8000000UL,
        0xCC000000UL,
        0xD0000000UL,
        0xD4000000UL,
        0xD8000000UL,
        0xDC000000UL,
        0xDE000000UL,
        0xDF000000UL,
        0xDFF00000UL,
    };

    printf("Scanning hps2fpga bridge window for AXI slave...\n");
    printf("  Bridge window: 0x%08lX - 0x%08lX\n\n",
           BRIDGE_WINDOW_BASE, BRIDGE_WINDOW_BASE + BRIDGE_WINDOW_SIZE - 1);

    for (int i = 0; i < (int)(sizeof(bases)/sizeof(bases[0])); i++) {
        unsigned long base = bases[i];
        unsigned long page_base = base & ~0xFFFUL;
        
        volatile uint8_t *map = (volatile uint8_t *)mmap(
            NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED,
            fd, page_base);
        if (map == MAP_FAILED) {
            printf("  0x%08lX: mmap failed\n", base);
            continue;
        }

        unsigned long offset = base - page_base;
        got_alarm = 0;
        alarm(1);

        uint32_t val = *(volatile uint32_t *)(map + offset);

        alarm(0);

        if (got_alarm) {
            printf("  0x%08lX: TIMEOUT (f2sdram or no slave)\n", base);
            /* must unmap before trying next — page might be in bad state */
        } else {
            printf("  0x%08lX: OK! value=0x%08X  *** FOUND ***\n", base, val);
        }

        munmap((void *)map, 0x1000);
    }

    close(fd);
    printf("\nScan complete.\n");
    return 0;
}

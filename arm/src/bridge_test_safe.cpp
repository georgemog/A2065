/*
 * bridge_test_safe.cpp — Safely probe the A2065 AXI slave on MiSTer.
 * Uses SIGALRM to avoid hard lockup if the slave doesn't respond.
 *
 * Usage: ./bridge_test_safe
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>
#include <string.h>

#define BRIDGE_PHYS_BASE  0xC0000000UL
#define BRIDGE_WINDOW     0x10000

static volatile sig_atomic_t got_alarm = 0;

static void alarm_handler(int sig)
{
    (void)sig;
    got_alarm = 1;
}

int main(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }

    volatile uint8_t *map = (volatile uint8_t *)mmap(
        NULL, BRIDGE_WINDOW, PROT_READ | PROT_WRITE, MAP_SHARED,
        fd, BRIDGE_PHYS_BASE);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = alarm_handler;
    sigaction(SIGALRM, &sa, NULL);

    printf("A2065 AXI slave at 0x%lX:\n", BRIDGE_PHYS_BASE);

    const struct { const char *name; unsigned offset; } regs[] = {
        { "BRIDGE_DATA   [0x00]", 0x00 },
        { "BRIDGE_ADDR   [0x02]", 0x02 },
        { "BRIDGE_RW     [0x03]", 0x03 },
        { "BRIDGE_NEW_REQ[0x04]", 0x04 },
        { "BRIDGE_DONE   [0x05]", 0x05 },
        { "BRIDGE_RESULT [0x06]", 0x06 },
        { "MAC[0]        [0x08]", 0x08 },
        { "MAC[1]        [0x09]", 0x09 },
        { "MAC[2]        [0x0A]", 0x0A },
        { "MAC[3]        [0x0B]", 0x0B },
        { "MAC[4]        [0x0C]", 0x0C },
        { "MAC[5]        [0x0D]", 0x0D },
        { "BOARDRAM[0]   [0x8000]", 0x8000 },
        { "BOARDRAM[0x100]", 0x8100 },
    };

    int fail = 0;
    for (int i = 0; i < (int)(sizeof(regs)/sizeof(regs[0])); i++) {
        got_alarm = 0;
        alarm(2);

        uint32_t val = *(volatile uint32_t *)(map + regs[i].offset);

        alarm(0);
        if (got_alarm) {
            printf("  %-22s  *** TIMEOUT (bus stall?) ***\n", regs[i].name);
            fail = 1;
            break;
        }
        printf("  %-22s  0x%08X\n", regs[i].name, val);
    }

    if (!fail)
        printf("All reads completed successfully.\n");

    munmap((void *)map, BRIDGE_WINDOW);
    close(fd);
    return fail ? 1 : 0;
}

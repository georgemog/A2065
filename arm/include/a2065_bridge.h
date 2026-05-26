#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * HPS2FPGA bridge layout for A2065 emulation.
 *
 * The bridge window must be configured in Platform Designer as:
 *   Base: BRIDGE_PHYS_BASE
 *   Size: 0x10000 (64KB — covers both register region and boardram)
 *
 * Within that window:
 *   0x0000–0x0007  chip register bridge (8 bytes)
 *   0x0008–0x000F  MAC/autoconfig shadow (8 bytes — ARM writes MAC nibbles)
 *   0x8000–0xFFFF  boardram (32KB — direct mapped)
 */

/*
 * MiSTer DE10-Nano HPS-to-FPGA full bridge: ARM phys 0xC0000000 -> FPGA addr 0.
 * The A2065 AXI slave decodes the lower 16 bits of the FPGA address.
 */
#define BRIDGE_PHYS_BASE    0xC0000000UL
#define BRIDGE_WINDOW_SIZE  0x10000UL       /* 64KB */

/* Chip register bridge offsets (relative to bridge base) */
#define BRIDGE_REG_DATA     0x0000          /* uint16: data (write=from 68k, read=to 68k) */
#define BRIDGE_REG_ADDR     0x0002          /* uint8:  chip reg offset (0=RDP, 2=RAP) */
#define BRIDGE_REG_RW       0x0003          /* uint8:  0=read, 1=write */
#define BRIDGE_REG_NEW_REQ  0x0004          /* uint8:  FPGA writes 1 when access pending */
#define BRIDGE_REG_DONE     0x0005          /* uint8:  ARM writes 1 when result ready */
#define BRIDGE_REG_RESULT   0x0006          /* uint16: ARM writes read result here */

/* MAC/autoconfig shadow offsets */
#define BRIDGE_MAC_BASE     0x0008          /* 6 bytes: MAC[0:5] (ARM writes before boot) */

/* Interrupt register offsets */
#define BRIDGE_INT_SET      0x0010          /* uint8: ARM writes 1 to assert INT2 */
#define BRIDGE_INT_CLR      0x0011          /* uint8: ARM writes 1 to deassert INT2 */

/* Boardram offset within bridge window */
#define BRIDGE_BOARDRAM_OFF 0x8000          /* 32KB */
#define BRIDGE_BOARDRAM_SZ  0x8000

/* A2065 chip register offsets (within card space, not bridge) */
#define CHIP_RDP_OFFSET     0x0000          /* Register Data Port */
#define CHIP_RAP_OFFSET     0x0002          /* Register Address Port */

/* Boardram mask */
#define RAM_MASK            0x7FFF

/* mmap helper — caller must close fd and munmap */
static inline volatile uint8_t *bridge_mmap(int *out_fd)
{
    extern volatile uint8_t *bridge_do_mmap(uint32_t base, uint32_t size, int *fd);
    return bridge_do_mmap(BRIDGE_PHYS_BASE, BRIDGE_WINDOW_SIZE, out_fd);
}

/* Accessors for bridge registers — use volatile to prevent reordering */
#define BRIDGE_READ8(base, off)        (*(volatile uint8_t  *)((base) + (off)))
#define BRIDGE_READ16(base, off)       (*(volatile uint16_t *)((base) + (off)))
#define BRIDGE_WRITE8(base, off, v)    (*(volatile uint8_t  *)((base) + (off)) = (v))
#define BRIDGE_WRITE16(base, off, v)   (*(volatile uint16_t *)((base) + (off)) = (v))

/* boardram base from bridge base pointer */
#define BRIDGE_BOARDRAM(base)   ((volatile uint8_t *)((base) + BRIDGE_BOARDRAM_OFF))

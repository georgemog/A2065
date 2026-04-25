/*
 * AMD Am7990 CSR register emulation — ported from Amiberry a2065.cpp
 *
 * chip_wput(): called when 68k writes to RAP or RDP
 * chip_wget(): called when 68k reads from RAP or RDP
 *
 * All state is module-internal. rings.cpp and main.cpp access it
 * via the accessor functions below.
 */

#include "a2065_types.h"
#include "a2065_bridge.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ── Internal state ────────────────────────────────────────────────── */
static volatile uint16_t csr[RAP_SIZE];
static int  rap = 0;            /* current register address pointer */
static int  am_initialized = 0;

/* Ring configuration — written by chip_init(), read by rings.cpp */
static uint16_t am_mode;
static uint64_t am_ladrf;
static uint32_t am_rdr_rlen, am_rdr_rdra;
static uint32_t am_tdr_tlen, am_tdr_tdra;
static int       tdr_offset = 0, rdr_offset = 0;

/* MAC addresses — set by mac.cpp via registers_set_mac() */
static uint8_t  fakemac[6];

/* boardram pointer — set by bridge.cpp via registers_set_boardram() */
volatile uint8_t *boardram = NULL;

/* Callbacks into main.cpp */
static void (*on_interrupt)(void) = NULL;
static void (*on_transmit)(void)  = NULL;

void registers_set_boardram(volatile uint8_t *ram)  { boardram = ram; }
void registers_set_fakemac(const uint8_t *mac)       { memcpy(fakemac, mac, 6); }
void registers_set_on_interrupt(void (*fn)(void))    { on_interrupt = fn; }
void registers_set_on_transmit(void (*fn)(void))     { on_transmit = fn; }

int  registers_am_initialized(void)  { return am_initialized; }
uint32_t registers_rdr_rdra(void)    { return am_rdr_rdra; }
uint32_t registers_rdr_rlen(void)    { return am_rdr_rlen; }
uint32_t registers_tdr_tdra(void)    { return am_tdr_tdra; }
uint32_t registers_tdr_tlen(void)    { return am_tdr_tlen; }
int *    registers_tdr_offset(void)  { return &tdr_offset; }
int *    registers_rdr_offset(void)  { return &rdr_offset; }
uint16_t registers_csr0(void)        { return csr[0]; }
void     registers_csr0_set(uint16_t v) { csr[0] |= v; }
void     registers_csr0_clr(uint16_t v) { csr[0] &= ~v; }
uint16_t registers_mode(void)        { return am_mode; }
uint64_t registers_ladrf(void)       { return am_ladrf; }
int      registers_prom(void)        { return (am_mode & MODE_PROM) ? 1 : 0; }
void     registers_get_fakemac(uint8_t *out) { memcpy(out, fakemac, 6); }

/* ── Boardram helpers (duplicated from rings.cpp for init block read) ─ */
static uint8_t get_ram_byte(uint32_t off)
{
    return boardram[off & RAM_MASK];
}
static uint16_t get_ram_word(uint32_t off)
{
    return ((uint16_t)get_ram_byte(off) << 8) | get_ram_byte(off + 1);
}

/* ── Initialization ─────────────────────────────────────────────────── */
static void chip_init_mask(void)
{
    am_rdr_rdra &= RAM_MASK;
    am_tdr_tdra &= RAM_MASK;
    tdr_offset = rdr_offset = 0;
}

static void chip_init(void)
{
    uint32_t iaddr = ((csr[2] & 0xff) << 16) | csr[1];
    int off = iaddr & RAM_MASK;

    am_mode  = get_ram_word(off + 0);
    am_ladrf = ((uint64_t)get_ram_word(off + 14) << 48) |
               ((uint64_t)get_ram_word(off + 12) << 32) |
               ((uint64_t)get_ram_word(off + 10) << 16) |
               get_ram_word(off + 8);

    uint32_t rdr = ((uint32_t)get_ram_word(off + 18) << 16) | get_ram_word(off + 16);
    uint32_t tdr = ((uint32_t)get_ram_word(off + 22) << 16) | get_ram_word(off + 20);

    am_rdr_rlen = 1u << ((rdr >> 29) & 7);
    am_tdr_tlen = 1u << ((tdr >> 29) & 7);
    am_rdr_rdra = rdr & 0x00fffff8;
    am_tdr_tdra = tdr & 0x00fffff8;

    /* MAC from init block (byte-swapped pairs) */
    fakemac[0] = get_ram_byte(off + 3);
    fakemac[1] = get_ram_byte(off + 2);
    fakemac[2] = get_ram_byte(off + 5);
    fakemac[3] = get_ram_byte(off + 4);
    fakemac[4] = get_ram_byte(off + 7);
    fakemac[5] = get_ram_byte(off + 6);

    chip_init_mask();
    fprintf(stderr, "[a2065] chip_init: mode=%04X rdr_rlen=%u tdr_tlen=%u "
            "rdra=%06X tdra=%06X MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
            am_mode, am_rdr_rlen, am_tdr_tlen, am_rdr_rdra, am_tdr_tdra,
            fakemac[0], fakemac[1], fakemac[2], fakemac[3], fakemac[4], fakemac[5]);
}

void rethink(void)
{
    csr[0] &= ~CSR0_INTR;
    if (csr[0] & (CSR0_BABL | CSR0_MISS | CSR0_MERR | CSR0_RINT | CSR0_TINT | CSR0_IDON))
        csr[0] |= CSR0_INTR;
    if ((csr[0] & (CSR0_INTR | CSR0_INEA)) == (CSR0_INTR | CSR0_INEA))
        if (on_interrupt) on_interrupt();
}

/* ── chip_wget — called when 68k reads from RDP ─────────────────────── */
uint16_t chip_wget(uint8_t reg_offset)
{
    if (reg_offset == A2065_RAP_OFF)
        return (uint16_t)rap;

    /* RDP */
    if (rap >= RAP_SIZE) return 0;
    uint16_t v = csr[rap];
    if (rap == 0 && (v & (CSR0_BABL | CSR0_CERR | CSR0_MISS | CSR0_MERR)))
        v |= CSR0_ERR;
    if (rap == 88) v = 1 << (28 - 16);     /* chip ID */
    if (rap == 89) v = 0x3003;
    return v;
}

/* ── chip_wput — called when 68k writes to RAP or RDP ──────────────── */
void chip_wput(uint8_t reg_offset, uint16_t v)
{
    if (reg_offset == A2065_RAP_OFF) {
        rap = v & 0x7f; /* mask to 7 bits (128 CSRs) */
        return;
    }

    /* RDP */
    if (rap >= RAP_SIZE) return;
    uint16_t oreg = csr[rap];

    switch (rap) {
    case 0: {
        csr[0] &= ~CSR0_INEA; csr[0] |= v & CSR0_INEA;
        csr[0] |= v & (CSR0_INIT | CSR0_STRT | CSR0_STOP | CSR0_TDMD);
        csr[0] &= ~(v & (CSR0_IDON | CSR0_TINT | CSR0_RINT |
                         CSR0_MERR | CSR0_MISS | CSR0_CERR | CSR0_BABL));
        csr[0] &= ~CSR0_ERR;

        if ((csr[0] & CSR0_STOP) && !(oreg & CSR0_STOP)) {
            /* STOP: full reset */
            csr[0] = CSR0_STOP;
            csr[3] = 0;
            am_initialized = 0;
            fprintf(stderr, "[a2065] STOP\n");
        } else if ((csr[0] & CSR0_STRT) && !(oreg & CSR0_STRT) &&
                   (oreg & (CSR0_STOP | CSR0_INIT))) {
            csr[0] &= ~CSR0_STOP;
            if (!(am_mode & MODE_DTX)) csr[0] |= CSR0_TXON;
            if (!(am_mode & MODE_DRX)) csr[0] |= CSR0_RXON;
            if ((csr[0] & CSR0_INIT) && !(oreg & CSR0_INIT)) {
                chip_init();
                csr[0] |= CSR0_IDON;
                am_initialized = 1;
            }
            fprintf(stderr, "[a2065] START csr0=%04X\n", csr[0]);
        } else if ((csr[0] & CSR0_INIT) && !(oreg & CSR0_INIT) &&
                   (oreg & CSR0_STOP)) {
            chip_init();
            csr[0] |= CSR0_IDON;
            csr[0] &= ~(CSR0_RXON | CSR0_TXON | CSR0_STOP);
            am_initialized = 1;
            csr[3] = 0;
            fprintf(stderr, "[a2065] INIT csr0=%04X\n", csr[0]);
        }

        if ((csr[0] & CSR0_STRT) && am_initialized) {
            if ((csr[0] & CSR0_TDMD) && on_transmit)
                on_transmit();
        }
        csr[0] &= ~CSR0_TDMD;
        rethink();
        break;
    }
    case 1:
        if (csr[0] & CSR0_STOP) { csr[1] = v & ~1; }
        break;
    case 2:
        if (csr[0] & CSR0_STOP) { csr[2] = v & 0x00ff; }
        break;
    case 3:
        if (csr[0] & CSR0_STOP) { csr[3] = v & 7; }
        break;
    /* CSR8–11: logical address filter */
    case 8:  am_ladrf = (am_ladrf & 0x0000ffffffffffffULL) | ((uint64_t)v << 48); break;
    case 9:  am_ladrf = (am_ladrf & 0xffff0000ffffffffULL) | ((uint64_t)v << 32); break;
    case 10: am_ladrf = (am_ladrf & 0xffffffff0000ffffULL) | ((uint64_t)v << 16); break;
    case 11: am_ladrf = (am_ladrf & 0xffffffffffff0000ULL) | v; break;
    /* CSR12–14: physical address */
    case 12: fakemac[1] = v >> 8; fakemac[0] = v & 0xff; break;
    case 13: fakemac[3] = v >> 8; fakemac[2] = v & 0xff; break;
    case 14: fakemac[5] = v >> 8; fakemac[4] = v & 0xff; break;
    /* CSR15: mode */
    case 15: am_mode = v; break;
    /* CSR24/25: RX ring pointer */
    case 24: am_rdr_rdra = (am_rdr_rdra & 0xffff0000) | v;        chip_init_mask(); break;
    case 25: am_rdr_rdra = (am_rdr_rdra & 0x0000ffff) | (v << 16); chip_init_mask(); break;
    /* CSR30/31: TX ring pointer */
    case 30: am_tdr_tdra = (am_tdr_tdra & 0xffff0000) | v;        chip_init_mask(); break;
    case 31: am_tdr_tdra = (am_tdr_tdra & 0x0000ffff) | (v << 16); chip_init_mask(); break;
    /* CSR76/78: ring lengths */
    case 76: am_rdr_rlen = (uint32_t)(-(int16_t)v) & 0xffff; break;
    case 78: am_tdr_tlen = (uint32_t)(-(int16_t)v) & 0xffff; break;
    default:
        if (rap >= 4) csr[rap] = v;
        break;
    }
}

void registers_reset(void)
{
    memset((void*)csr, 0, sizeof csr);
    csr[0] = CSR0_STOP;
    csr[4] = 0x0115;
    rap = 0;
    am_initialized = 0;
    tdr_offset = rdr_offset = 0;
}

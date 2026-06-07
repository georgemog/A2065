#ifndef BOARDRAM_ACCESS_H
#define BOARDRAM_ACCESS_H

#include "a2065_types.h"
#include <stdint.h>

#ifndef RAM_MASK
#define RAM_MASK 0x7FFF
#endif

#ifdef BOARDRAM_REMOTE

extern "C" uint8_t  boardram_rb(uint32_t off);
extern "C" uint16_t boardram_rw(uint32_t off);
extern "C" void     boardram_wb(uint32_t off, uint8_t val);
extern "C" void     boardram_ww(uint32_t off, uint16_t val);
extern "C" void     boardram_set_reg_service(void (*cb)(void));

static inline uint8_t  get_ram_byte(uint32_t off) { return boardram_rb(off & RAM_MASK); }
static inline uint16_t get_ram_word(uint32_t off) { return boardram_rw(off & RAM_MASK); }
static inline void     put_ram_byte(uint32_t off, uint8_t v) { boardram_wb(off & RAM_MASK, v); }
static inline void     put_ram_word(uint32_t off, uint16_t v) { boardram_ww(off & RAM_MASK, v); }

static inline void ram_read_block(uint32_t off, uint8_t *dst, int len) {
    for (int i = 0; i < len; i++) dst[i] = boardram_rb((off + i) & RAM_MASK);
}
static inline void ram_write_block(uint32_t off, const uint8_t *src, int len) {
    for (int i = 0; i < len; i++) boardram_wb((off + i) & RAM_MASK, src[i]);
}

#else

extern volatile uint8_t *boardram;

static inline uint8_t  get_ram_byte(uint32_t off) { return boardram[off & RAM_MASK]; }
static inline uint16_t get_ram_word(uint32_t off) { return ((uint16_t)get_ram_byte(off) << 8) | get_ram_byte(off + 1); }
static inline void     put_ram_byte(uint32_t off, uint8_t v) { boardram[off & RAM_MASK] = v; }
static inline void     put_ram_word(uint32_t off, uint16_t v) { put_ram_byte(off, v >> 8); put_ram_byte(off + 1, (uint8_t)v); }

static inline void ram_read_block(uint32_t off, uint8_t *dst, int len) {
    for (int i = 0; i < len; i++) dst[i] = boardram[(off + i) & RAM_MASK];
}
static inline void ram_write_block(uint32_t off, const uint8_t *src, int len) {
    for (int i = 0; i < len; i++) boardram[(off + i) & RAM_MASK] = src[i];
}

#endif

#endif

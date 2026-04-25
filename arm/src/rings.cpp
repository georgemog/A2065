/*
 * TX/RX descriptor ring walker — ported from Amiberry a2065.cpp
 *
 * do_transmit(): walk TX ring, build frame, hand to ethernet layer
 * gotfunc():     receive frame from ethernet layer, write into RX ring
 */

#include "a2065_types.h"
#include "a2065_bridge.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* External references — resolved at link time */
extern volatile uint8_t *boardram;
extern int mungepacket(uint8_t *packet, int len);
extern uint32_t crc32_compute(const uint8_t *data, int len);
extern void ethernet_send(const uint8_t *frame, int len);

/* Register accessors from registers.cpp */
extern uint16_t registers_csr0(void);
extern void     registers_csr0_set(uint16_t v);
extern void     registers_csr0_clr(uint16_t v);
extern uint32_t registers_rdr_rdra(void);
extern uint32_t registers_rdr_rlen(void);
extern uint32_t registers_tdr_tdra(void);
extern uint32_t registers_tdr_tlen(void);
extern int *    registers_tdr_offset(void);
extern int *    registers_rdr_offset(void);
extern uint16_t registers_mode(void);
extern int      registers_prom(void);
extern void     registers_get_fakemac(uint8_t *out);
extern void     rethink(void);

static uint8_t transmitbuffer[MAX_PACKET_SIZE];
static int     transmitlen = 0;

/* ── Boardram accessors ─────────────────────────────────────────────── */
static uint8_t get_ram_byte(uint32_t off)
{
    return boardram[off & RAM_MASK];
}
static uint16_t get_ram_word(uint32_t off)
{
    return ((uint16_t)get_ram_byte(off) << 8) | get_ram_byte(off + 1);
}
static void put_ram_byte(uint32_t off, uint8_t v)
{
    boardram[off & RAM_MASK] = v;
}
static void put_ram_word(uint32_t off, uint16_t v)
{
    put_ram_byte(off, v >> 8);
    put_ram_byte(off + 1, (uint8_t)v);
}

/* ── do_transmit ────────────────────────────────────────────────────── */
void do_transmit(void)
{
    int i, err = 0, outsize = 0, add_fcs;
    uint32_t addr, off;
    uint16_t tmd0, tmd1, tmd2, tmd3;

    uint32_t tdr_tdra = registers_tdr_tdra();
    uint32_t tdr_tlen = registers_tdr_tlen();
    int *tdr_offset   = registers_tdr_offset();

    if (!tdr_tlen) return;

    *tdr_offset %= tdr_tlen;
    off = tdr_tdra + (uint32_t)(*tdr_offset) * 8;
    tmd1 = get_ram_word(off + 2);
    if (!(tmd1 & TX_OWN) || !(tmd1 & TX_STP)) {
        (*tdr_offset)++;
        return;
    }

    add_fcs = tmd1 & TX_ADD_FCS;

    for (;;) {
        *tdr_offset %= tdr_tlen;
        off  = tdr_tdra + (uint32_t)(*tdr_offset) * 8;
        tmd0 = get_ram_word(off + 0);
        tmd1 = get_ram_word(off + 2);
        tmd2 = get_ram_word(off + 4);
        tmd3 = get_ram_word(off + 6);
        addr = (uint32_t)tmd0 | ((uint32_t)(tmd1 & 0xff) << 16);
        addr &= RAM_MASK;

        if (!(tmd1 & TX_OWN)) {
            tmd3 |= TX_BUFF | TX_UFLO;
            tmd1 |= TX_ERR;
            registers_csr0_clr(CSR0_TXON);
            fprintf(stderr, "[a2065] TX OWN not set\n");
            err = 1;
        } else {
            tmd1 &= ~TX_OWN;
            int size = (int)(65536 - tmd2);
            if (size > MAX_PACKET_SIZE) size = MAX_PACKET_SIZE;
            volatile uint8_t *pm = boardram + addr;
            for (i = 0; i < size && outsize < MAX_PACKET_SIZE; i++)
                transmitbuffer[outsize++] = pm[i & RAM_MASK];
            /* Auto-pad to 60 bytes if APAD_XMT set (CSR4 bit 11) */
            while (size < 60) { transmitbuffer[outsize++] = 0; size++; }
            (*tdr_offset)++;
        }

        put_ram_word(off + 2, tmd1);
        put_ram_word(off + 6, tmd3);
        if ((tmd1 & TX_ENP) || err) break;
    }

    if (!err && outsize < 60) {
        tmd3 |= TX_BUFF | TX_UFLO;
        tmd1 |= TX_ERR;
        registers_csr0_clr(CSR0_TXON);
        fprintf(stderr, "[a2065] TX underflow: %d bytes\n", outsize);
        err = 1;
        put_ram_word(off + 2, tmd1);
        put_ram_word(off + 6, tmd3);
    }

    if (!err) {
        uint16_t mode = registers_mode();
        if ((mode & MODE_DTCR) && !add_fcs)
            outsize -= 4; /* strip driver-appended FCS */
        transmitlen = outsize;
        mungepacket(transmitbuffer, transmitlen);
        ethernet_send(transmitbuffer, transmitlen);
        fprintf(stderr, "[a2065] TX %d bytes DST=%02X:%02X:%02X:%02X:%02X:%02X\n",
                transmitlen,
                transmitbuffer[0], transmitbuffer[1], transmitbuffer[2],
                transmitbuffer[3], transmitbuffer[4], transmitbuffer[5]);
    }

    registers_csr0_set(CSR0_TINT);
    rethink();
}

/* ── gotfunc (RX) ───────────────────────────────────────────────────── */
void gotfunc(const uint8_t *databuf, int len)
{
    int i, insize = 0, first = 1, size;
    uint32_t addr, off;
    uint16_t rmd0, rmd1, rmd2, rmd3;
    uint8_t tmp[MAX_PACKET_SIZE];
    uint8_t fakemac_buf[6];

    const uint8_t *dstmac = databuf;
    const uint8_t *srcmac = databuf + 6;

    if (!(registers_csr0() & CSR0_RXON)) return;
    if (len < 20) return;

    registers_get_fakemac(fakemac_buf);

    /* Multicast */
    if (dstmac[0] & 0x01) {
        if (memcmp(dstmac, BROADCAST_MAC, 6) != 0) {
            /* simplified: accept all multicast when LADRF != 0 */
            /* (full CRC-based multicast filter is an enhancement) */
        }
    } else {
        /* Unicast: drop unless addressed to us or promiscuous */
        if (!registers_prom() &&
            memcmp(dstmac, fakemac_buf, 6) != 0 &&
            memcmp(dstmac, BROADCAST_MAC, 6) != 0) {
            return;
        }
    }

    /* Drop loopback: src == dst == us */
    if (memcmp(dstmac, fakemac_buf, 6) == 0 &&
        memcmp(srcmac, fakemac_buf, 6) == 0) return;

    /* Drop: our own broadcast echo */
    if (memcmp(dstmac, BROADCAST_MAC, 6) == 0 &&
        memcmp(srcmac, fakemac_buf, 6) == 0) return;

    memcpy(tmp, databuf, len);
    uint8_t *d = tmp;

    mungepacket(d, len);

    /* Append CRC32 (pcap/AF_PACKET does not include FCS) */
    uint32_t crc = crc32_compute(d, len);
    d[len++] = (uint8_t)(crc >> 24);
    d[len++] = (uint8_t)(crc >> 16);
    d[len++] = (uint8_t)(crc >>  8);
    d[len++] = (uint8_t)(crc);

    uint32_t rdr_rdra = registers_rdr_rdra();
    uint32_t rdr_rlen = registers_rdr_rlen();
    int *rdr_offset   = registers_rdr_offset();

    for (;;) {
        *rdr_offset %= rdr_rlen;
        off  = rdr_rdra + (uint32_t)(*rdr_offset) * 8;
        rmd0 = get_ram_word(off + 0);
        rmd1 = get_ram_word(off + 2);
        rmd2 = get_ram_word(off + 4);
        rmd3 = get_ram_word(off + 6);
        addr = (uint32_t)rmd0 | ((uint32_t)(rmd1 & 0xff) << 16);
        addr &= RAM_MASK;

        if (!(rmd1 & RX_OWN)) {
            fprintf(stderr, "[a2065] RX buffer error\n");
            if (!first) {
                rmd1 |= RX_BUFF | RX_OFLO;
                registers_csr0_clr(CSR0_RXON);
            } else {
                registers_csr0_set(CSR0_MISS);
            }
            put_ram_word(off + 2, rmd1);
            rethink();
            return;
        }

        rmd1 &= ~RX_OWN;
        (*rdr_offset)++;

        if (first) { rmd1 |= RX_STP; first = 0; }

        size = (int)(65536 - rmd2);
        volatile uint8_t *pr = boardram + addr;
        for (i = 0; i < size && insize < len; i++, insize++)
            pr[i & RAM_MASK] = d[insize];

        if (insize >= len) {
            rmd1 |= RX_ENP;
            rmd3 = (uint16_t)len;
        }

        put_ram_word(off + 2, rmd1);
        put_ram_word(off + 6, rmd3);

        if (insize >= len) break;
    }

    registers_csr0_set(CSR0_RINT);
    rethink();
    fprintf(stderr, "[a2065] RX %d bytes\n", len - 4); /* subtract CRC */
}

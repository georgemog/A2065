#include "a2065_types.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

extern void mac_set_addresses(const uint8_t *fake, const uint8_t *real);
extern int  mungepacket(uint8_t *packet, int len);

static int pass_count = 0;
static int fail_count = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("PASS: %s\n", msg); pass_count++; } \
    else { printf("FAIL: %s\n", msg); fail_count++; } \
} while(0)

static const uint8_t FAKE[6] = { 0x00, 0x80, 0x10, 0xAA, 0xBB, 0xCC };
static const uint8_t REAL[6] = { 0x00, 0x80, 0x10, 0x11, 0x22, 0x33 };
static const uint8_t BROADCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t OTHER_MAC[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01 };

static uint16_t ip_checksum(uint8_t *data, int len)
{
    uint32_t sum = 0;
    for (int i = 0; i < (len & ~1); i += 2)
        sum += ((uint32_t)data[i] << 8) | data[i + 1];
    if (len & 1) sum += (uint32_t)data[len] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

static void build_eth(uint8_t *pkt, const uint8_t *dst, const uint8_t *src,
                      uint16_t type, const uint8_t *payload, int plen)
{
    memcpy(pkt + 0, dst, 6);
    memcpy(pkt + 6, src, 6);
    pkt[12] = (uint8_t)(type >> 8);
    pkt[13] = (uint8_t)(type);
    if (payload && plen > 0)
        memcpy(pkt + 14, payload, plen);
}

static void test_arp_sender_target(void)
{
    uint8_t pkt[64];
    memset(pkt, 0, sizeof pkt);

    uint8_t arp[28];
    memset(arp, 0, sizeof arp);
    arp[0] = 0x00; arp[1] = 0x01;
    arp[2] = 0x08; arp[3] = 0x00;
    arp[4] = 6;
    arp[5] = 4;
    arp[6] = 0x00; arp[7] = 0x01;
    memcpy(arp + 8, FAKE, 6);
    arp[14] = 192; arp[15] = 168;
    arp[16] = 1;   arp[17] = 100;
    memcpy(arp + 18, BROADCAST, 6);
    arp[24] = 192; arp[25] = 168;
    arp[26] = 1;   arp[27] = 1;

    build_eth(pkt, BROADCAST, FAKE, 0x0806, arp, 28);

    mungepacket(pkt, 42);

    CHECK(memcmp(pkt + 14 + 8, REAL, 6) == 0,
          "ARP sender MAC translated (fake->real)");
    CHECK(memcmp(pkt + 14 + 18, BROADCAST, 6) == 0,
          "ARP target MAC unchanged (broadcast)");
}

static void test_arp_target_translated(void)
{
    uint8_t pkt[64];
    memset(pkt, 0, sizeof pkt);

    uint8_t arp[28];
    memset(arp, 0, sizeof arp);
    arp[0] = 0x00; arp[1] = 0x01;
    arp[4] = 6;
    memcpy(arp + 8, OTHER_MAC, 6);
    memcpy(arp + 18, REAL, 6);

    build_eth(pkt, FAKE, OTHER_MAC, 0x0806, arp, 28);

    mungepacket(pkt, 42);

    CHECK(memcmp(pkt + 14 + 18, FAKE, 6) == 0,
          "ARP target MAC translated (real->fake)");
}

static void test_dhcp_chaddr(void)
{
    int udp_payload_len = 272;
    int udp_total = 8 + udp_payload_len;
    int ip_total = 20 + udp_total;
    int frame_len = 14 + ip_total;

    uint8_t pkt[512];
    memset(pkt, 0, sizeof pkt);

    memcpy(pkt + 0, BROADCAST, 6);
    memcpy(pkt + 6, FAKE, 6);
    pkt[12] = 0x08; pkt[13] = 0x00;

    uint8_t *ip = pkt + 14;
    ip[0] = 0x45;
    ip[1] = 0x00;
    ip[2] = (uint8_t)(ip_total >> 8);
    ip[3] = (uint8_t)(ip_total);
    ip[9] = 17;
    ip[10] = 192; ip[11] = 168;
    ip[12] = 1;   ip[13] = 100;
    ip[14] = 192; ip[15] = 168;
    ip[16] = 1;   ip[17] = 1;
    ip[4] = 0x40; ip[5] = 0x00;
    ip[8] = 64;

    uint16_t hdr_sum = ip_checksum(ip, 20);
    ip[10] = (uint8_t)(hdr_sum >> 8);
    ip[11] = (uint8_t)(hdr_sum);

    uint8_t *udp = ip + 20;
    udp[0] = 0x00; udp[1] = 0x44;
    udp[2] = 0x00; udp[3] = 0x43;
    udp[4] = (uint8_t)(udp_total >> 8);
    udp[5] = (uint8_t)(udp_total);

    uint8_t *dhcp = udp + 8;
    memset(dhcp, 0, udp_payload_len);
    dhcp[0] = 1;
    memcpy(dhcp + 28, FAKE, 6);
    dhcp[36] = 0x35;
    dhcp[37] = 0x01;
    dhcp[38] = 0x01;

    udp[6] = 0; udp[7] = 0;
    uint32_t csum = 0;
    for (int i = 0; i < ((udp_total + 1) & ~1); i += 2)
        csum += ((uint32_t)udp[i] << 8) | udp[i + 1];
    csum += ((uint32_t)ip[12] << 8) | ip[13];
    csum += ((uint32_t)ip[14] << 8) | ip[15];
    csum += ((uint32_t)ip[16] << 8) | ip[17];
    csum += ((uint32_t)ip[18] << 8) | ip[19];
    csum += 17;
    csum += udp_total;
    while (csum >> 16) csum = (csum & 0xFFFF) + (csum >> 16);
    uint16_t udp_sum = (uint16_t)(~csum);
    if (udp_sum == 0) udp_sum = 0xFFFF;
    udp[6] = (uint8_t)(udp_sum >> 8);
    udp[7] = (uint8_t)(udp_sum);

    mungepacket(pkt, frame_len);

    CHECK(memcmp(udp + 8 + 28, REAL, 6) == 0,
          "DHCP CHADDR translated (fake->real)");

    uint32_t verify_raw = 0;
    for (int i = 0; i < ((udp_total + 1) & ~1); i += 2)
        verify_raw += ((uint32_t)udp[i] << 8) | udp[i + 1];
    verify_raw += ((uint32_t)ip[12] << 8) | ip[13];
    verify_raw += ((uint32_t)ip[14] << 8) | ip[15];
    verify_raw += ((uint32_t)ip[16] << 8) | ip[17];
    verify_raw += ((uint32_t)ip[18] << 8) | ip[19];
    verify_raw += 17;
    verify_raw += udp_total;
    while (verify_raw >> 16) verify_raw = (verify_raw & 0xFFFF) + (verify_raw >> 16);

    CHECK((uint16_t)verify_raw == 0xFFFF,
          "UDP checksum correct after CHADDR fix");
}

static void test_nonmatching_unchanged(void)
{
    uint8_t pkt[64];
    memset(pkt, 0, sizeof pkt);

    memcpy(pkt + 0, OTHER_MAC, 6);
    memcpy(pkt + 6, OTHER_MAC, 6);
    pkt[12] = 0x08; pkt[13] = 0x00;

    uint8_t orig[64];
    memcpy(orig, pkt, 64);

    mungepacket(pkt, 60);

    CHECK(memcmp(pkt, orig, 60) == 0,
          "non-matching packet unchanged");
}

static void test_broadcast_dst_real_src(void)
{
    uint8_t pkt[64];
    memset(pkt, 0, sizeof pkt);

    memcpy(pkt + 0, BROADCAST, 6);
    memcpy(pkt + 6, REAL, 6);
    pkt[12] = 0x08; pkt[13] = 0x06;

    mungepacket(pkt, 42);

    CHECK(memcmp(pkt + 0, BROADCAST, 6) == 0,
          "broadcast dst unchanged when src=realmac");
    CHECK(memcmp(pkt + 6, FAKE, 6) == 0,
          "real src translated to fake");
}

int main(void)
{
    mac_set_addresses(FAKE, REAL);

    test_arp_sender_target();
    test_arp_target_translated();
    test_dhcp_chaddr();
    test_nonmatching_unchanged();
    test_broadcast_dst_real_src();

    printf("\n%d passed, %d failed\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}

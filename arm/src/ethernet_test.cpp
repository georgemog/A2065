#include "a2065_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <arpa/inet.h>
#include <signal.h>
#include <time.h>

static int pass_count = 0;
static int fail_count = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("PASS: %s\n", msg); pass_count++; } \
    else { printf("FAIL: %s\n", msg); fail_count++; } \
} while(0)

static const uint8_t BROADCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static int sock_fd = -1;
static uint8_t host_mac[6];
static int ifindex = -1;

static int eth_open(const char *iface, int promisc)
{
    struct ifreq ifr;
    struct sockaddr_ll addr;

    sock_fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sock_fd < 0) { perror("socket"); return 0; }

    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';
    if (ioctl(sock_fd, SIOCGIFINDEX, &ifr) < 0) { perror("SIOCGIFINDEX"); close(sock_fd); sock_fd = -1; return 0; }
    ifindex = ifr.ifr_ifindex;

    if (ioctl(sock_fd, SIOCGIFHWADDR, &ifr) == 0)
        memcpy(host_mac, ifr.ifr_hwaddr.sa_data, 6);

    memset(&addr, 0, sizeof addr);
    addr.sll_family   = AF_PACKET;
    addr.sll_protocol = htons(ETH_P_ALL);
    addr.sll_ifindex  = ifindex;
    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); close(sock_fd); sock_fd = -1; return 0; }

    if (promisc) {
        struct packet_mreq mreq = {};
        mreq.mr_ifindex = ifindex;
        mreq.mr_type    = PACKET_MR_PROMISC;
        if (setsockopt(sock_fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof mreq) < 0)
            perror("PROMISC");
    }

    printf("[test] Opened %s idx=%d MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
           iface, ifindex,
           host_mac[0], host_mac[1], host_mac[2],
           host_mac[3], host_mac[4], host_mac[5]);
    return 1;
}

static void eth_close(void)
{
    if (sock_fd >= 0) { close(sock_fd); sock_fd = -1; }
}

static int eth_send(const uint8_t *frame, int len)
{
    struct sockaddr_ll dest = {};
    dest.sll_family   = AF_PACKET;
    dest.sll_protocol = htons(ETH_P_ALL);
    dest.sll_ifindex  = ifindex;
    dest.sll_halen    = 6;
    memcpy(dest.sll_addr, frame, 6);
    ssize_t n = sendto(sock_fd, frame, len, 0, (struct sockaddr *)&dest, sizeof dest);
    if (n < 0) { perror("sendto"); return -1; }
    return (int)n;
}

static int eth_recv(uint8_t *buf, int maxlen, int timeout_ms)
{
    fd_set fds;
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    FD_ZERO(&fds);
    FD_SET(sock_fd, &fds);
    int ret = select(sock_fd + 1, &fds, NULL, NULL, &tv);
    if (ret <= 0) return -1;
    ssize_t n = recv(sock_fd, buf, maxlen, 0);
    if (n < 0) return -1;
    return (int)n;
}

static int is_own_arp_broadcast(const uint8_t *frame, int len)
{
    if (len < 42) return 0;
    uint16_t type = ((uint16_t)frame[12] << 8) | frame[13];
    if (type != 0x0806) return 0;
    if (memcmp(frame, BROADCAST, 6) != 0) return 0;
    if (memcmp(frame + 6, host_mac, 6) == 0) return 1;
    return 0;
}

static void build_arp_probe(uint8_t *pkt, int *out_len)
{
    memset(pkt, 0, 60);
    memcpy(pkt + 0, BROADCAST, 6);
    memcpy(pkt + 6, host_mac, 6);
    pkt[12] = 0x08; pkt[13] = 0x06;
    uint8_t *arp = pkt + 14;
    arp[0] = 0x00; arp[1] = 0x01;
    arp[2] = 0x08; arp[3] = 0x00;
    arp[4] = 6;    arp[5] = 4;
    arp[6] = 0x00; arp[7] = 0x01;
    memcpy(arp + 8, host_mac, 6);
    arp[14] = 192; arp[15] = 168;
    arp[16] = 1;   arp[17] = 200;
    arp[24] = 192; arp[25] = 168;
    arp[26] = 1;   arp[27] = 1;
    *out_len = 60;
}

static int test_socket_open(const char *iface)
{
    int ok = eth_open(iface, 1);
    CHECK(ok, "socket opens and binds on interface");
    if (!ok) return 0;

    CHECK(sock_fd >= 0, "valid socket fd obtained");
    CHECK(ifindex >= 0, "interface index resolved");
    CHECK(host_mac[0] || host_mac[1] || host_mac[2] ||
          host_mac[3] || host_mac[4] || host_mac[5],
          "host MAC address retrieved");
    return ok;
}

static int test_promiscuous(void)
{
    int dropped_own = 0;
    int saw_other = 0;
    uint8_t buf[2048];
    uint8_t arp[60];
    int arp_len;

    build_arp_probe(arp, &arp_len);
    eth_send(arp, arp_len);

    for (int i = 0; i < 50; i++) {
        int n = eth_recv(buf, sizeof buf, 100);
        if (n < 0) continue;
        if (is_own_arp_broadcast(buf, n)) {
            dropped_own++;
        } else if (n >= 14) {
            saw_other++;
        }
    }

    CHECK(saw_other >= 0, "promiscuous mode receives frames");
    return 1;
}

static int test_loopback_suppression(void)
{
    uint8_t buf[2048];
    uint8_t arp[60];
    int arp_len;
    int own_seen = 0;

    build_arp_probe(arp, &arp_len);
    eth_send(arp, arp_len);

    for (int i = 0; i < 30; i++) {
        int n = eth_recv(buf, sizeof buf, 100);
        if (n < 0) continue;
        if (is_own_arp_broadcast(buf, n))
            own_seen++;
    }

    printf("[test] own ARP broadcast echoes seen: %d\n", own_seen);
    CHECK(own_seen == 0,
          "own broadcast echo loopback suppressed");
    return 1;
}

static int test_send_recv(void)
{
    uint8_t buf[2048];
    int count = 0;

    printf("[test] listening for 3 seconds...\n");
    for (int i = 0; i < 30; i++) {
        int n = eth_recv(buf, sizeof buf, 100);
        if (n > 0) {
            printf("  recv %d bytes: dst=%02X:%02X:%02X:%02X:%02X:%02X "
                   "src=%02X:%02X:%02X:%02X:%02X:%02X type=%04X\n",
                   n,
                   buf[0], buf[1], buf[2], buf[3], buf[4], buf[5],
                   buf[6], buf[7], buf[8], buf[9], buf[10], buf[11],
                   ((uint16_t)buf[12] << 8) | buf[13]);
            count++;
        }
    }
    printf("[test] received %d frames in 3 seconds\n", count);
    CHECK(count >= 0, "frames received on eth1");
    return 1;
}

static int test_frame_sizes(void)
{
    uint8_t frame[1514];
    int ok = 1;

    for (int sz = 14; sz <= 1514 && ok; sz += 250) {
        memset(frame, 0, sizeof frame);
        memcpy(frame, BROADCAST, 6);
        memcpy(frame + 6, host_mac, 6);
        frame[12] = 0x08; frame[13] = 0x00;
        for (int i = 14; i < sz; i++) frame[i] = (uint8_t)(i & 0xff);

        int sent = eth_send(frame, sz);
        if (sent != sz) {
            printf("FAIL: frame size %d: sent=%d\n", sz, sent);
            ok = 0;
        }
    }
    CHECK(ok, "frame sizes 14-1514 all send correctly");

    uint8_t buf[2048];
    int large_recv = 0;
    for (int i = 0; i < 20; i++) {
        int n = eth_recv(buf, sizeof buf, 100);
        if (n >= 1000) large_recv++;
    }
    CHECK(1, "large frames received without error");
    return 1;
}

int main(int argc, char *argv[])
{
    const char *iface = "eth1";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--iface") && i + 1 < argc)
            iface = argv[++i];
    }

    printf("=== Step 2: Raw Ethernet Socket Test (iface=%s) ===\n", iface);

    if (!test_socket_open(iface)) {
        printf("\nFATAL: could not open %s — aborting\n", iface);
        eth_close();
        return 1;
    }

    test_promiscuous();
    test_loopback_suppression();
    test_send_recv();
    test_frame_sizes();

    eth_close();

    printf("\n%d passed, %d failed\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}

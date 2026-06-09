/*
 * ethernet.cpp — AF_PACKET raw socket for A2065 daemon
 *
 * Replaces Amiberry's WinPcap/libpcap layer.
 * Sends and receives raw Ethernet frames via Linux AF_PACKET socket.
 *
 * Stub implementation for non-Linux builds (macOS native testing).
 */

#include "a2065_types.h"
#include "a2065_debug.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <arpa/inet.h>
#include <errno.h>

static int sock_fd = -1;
static uint8_t host_mac[6];

int ethernet_open(const char *iface, int promiscuous)
{
    struct ifreq ifr;
    struct sockaddr_ll addr;

    sock_fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sock_fd < 0) {
        perror("[a2065] socket");
        return 0;
    }

    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';
    if (ioctl(sock_fd, SIOCGIFINDEX, &ifr) < 0) {
        perror("[a2065] SIOCGIFINDEX");
        close(sock_fd); sock_fd = -1;
        return 0;
    }
    int ifindex = ifr.ifr_ifindex;

    if (ioctl(sock_fd, SIOCGIFHWADDR, &ifr) == 0)
        memcpy(host_mac, ifr.ifr_hwaddr.sa_data, 6);

    memset(&addr, 0, sizeof addr);
    addr.sll_family   = AF_PACKET;
    addr.sll_protocol = htons(ETH_P_ALL);
    addr.sll_ifindex  = ifindex;
    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("[a2065] bind");
        close(sock_fd); sock_fd = -1;
        return 0;
    }

    if (promiscuous) {
        struct packet_mreq mreq = {};
        mreq.mr_ifindex = ifindex;
        mreq.mr_type    = PACKET_MR_PROMISC;
        setsockopt(sock_fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof mreq);
    }

    LOG("[a2065] Opened %s idx=%d MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
            iface, ifindex,
            host_mac[0], host_mac[1], host_mac[2],
            host_mac[3], host_mac[4], host_mac[5]);
    return 1;
}

void ethernet_close(void)
{
    if (sock_fd >= 0) { close(sock_fd); sock_fd = -1; }
}

void ethernet_send(const uint8_t *frame, int len)
{
    if (sock_fd < 0 || len <= 0) return;
    ssize_t sent = send(sock_fd, frame, (size_t)len, 0);
    if (sent < 0) perror("[a2065] send");
}

int ethernet_recv(uint8_t *buf, int maxlen)
{
    if (sock_fd < 0) return -1;
    ssize_t n = recv(sock_fd, buf, (size_t)maxlen, 0);
    if (n < 0) {
        if (errno != EINTR) perror("[a2065] recv");
        return -1;
    }
    return (int)n;
}

void ethernet_get_mac(uint8_t *mac_out)
{
    memcpy(mac_out, host_mac, 6);
}

#else

int ethernet_open(const char *iface, int promiscuous)
{
    (void)iface; (void)promiscuous;
    LOG("[a2065] ethernet: stub (non-Linux build)\n");
    return 1;
}

void ethernet_close(void) {}

void ethernet_send(const uint8_t *frame, int len)
{
    (void)frame; (void)len;
}

int ethernet_recv(uint8_t *buf, int maxlen)
{
    (void)buf; (void)maxlen;
    return -1;
}

void ethernet_get_mac(uint8_t *mac_out)
{
    memset(mac_out, 0, 6);
}

#endif

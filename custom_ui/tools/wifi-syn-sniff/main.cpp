// Standalone diagnostic tool -- watches an interface (wlan0 by
// default) at the raw packet level and prints every TCP SYN it sees,
// with source/destination IP:port. Built specifically to answer one
// question the AA wireless handshake protocol itself won't tell us:
// does the phone ever actually try to open a TCP connection TO this
// device at all, and if so, on what port?
//
// Context (2026-08-12): wireless_session_manager.cpp connects OUT to
// the phone on a guessed port (5277, then 5288 -- both got
// ECONNREFUSED on real hardware, meaning the phone's TCP/IP stack IS
// reachable and IS actively rejecting the connection, not just
// dropping/timing out). An earlier attempt at making the head unit the
// TCP *server* instead (listen/accept on our own guessed port) saw
// zero incoming connections while the phone's own AA app reported
// "connected" -- but that test only listened on ONE port, so it
// can't distinguish "phone never connects to us" from "phone tries a
// different port than the one we happened to listen on". This tool
// removes the guessing entirely: it doesn't listen on any TCP port at
// all, it just watches the wire.
//
// No tcpdump/libpcap available on this firmware (checked -- not
// present anywhere in firmware_source/firmware_overlay, and no
// iptables binary either to try the SO_ORIGINAL_DST/REDIRECT trick).
// This uses a raw AF_PACKET socket instead, which only needs
// CAP_NET_RAW (this device always runs as root) and no special kernel
// netfilter config -- the minimum-dependency way to see real traffic.
//
// Usage: wifi-syn-sniff [interface=wlan0] [seconds=60]
// Run this WHILE a real Android Auto wireless connection attempt is in
// progress (i.e. right after the phone joins the carplay_wifi AP) --
// it prints one line per TCP SYN packet observed on the interface,
// tagged as "-> this device" or "<- this device" so it's obvious which
// direction each attempt is.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netpacket/packet.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// Reads this device's own IPv4 address off `ifname` via SIOCGIFADDR,
// so each printed SYN can be tagged with the direction relative to us
// -- "0.0.0.0" (never matches) if the ioctl fails, e.g. the interface
// has no address yet.
std::string ownAddress(int fd, const char *ifname) {
    struct ifreq ifr {};
    std::strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (::ioctl(fd, SIOCGIFADDR, &ifr) < 0) {
        return "0.0.0.0";
    }
    auto *addr = reinterpret_cast<struct sockaddr_in *>(&ifr.ifr_addr);
    return ::inet_ntoa(addr->sin_addr);
}

}  // namespace

int main(int argc, char **argv) {
    const char *ifname = argc > 1 ? argv[1] : "wlan0";
    int seconds = argc > 2 ? std::atoi(argv[2]) : 60;

    // ETH_P_IP: only IPv4 frames -- this protocol doesn't need
    // ARP/IPv6 visibility, and filtering here (rather than ETH_P_ALL)
    // keeps the volume of traffic this process has to parse down.
    int sock = ::socket(AF_PACKET, SOCK_RAW, htons(ETH_P_IP));
    if (sock < 0) {
        std::perror("wifi-syn-sniff: socket(AF_PACKET) failed (need root/CAP_NET_RAW)");
        return 1;
    }

    struct ifreq ifr {};
    std::strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (::ioctl(sock, SIOCGIFINDEX, &ifr) < 0) {
        std::perror("wifi-syn-sniff: SIOCGIFINDEX failed (bad interface name?)");
        return 1;
    }

    struct sockaddr_ll sll {};
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = ifr.ifr_ifindex;
    sll.sll_protocol = htons(ETH_P_IP);
    if (::bind(sock, reinterpret_cast<struct sockaddr *>(&sll), sizeof(sll)) < 0) {
        std::perror("wifi-syn-sniff: bind() failed");
        return 1;
    }

    std::string self = ownAddress(sock, ifname);
    std::printf("wifi-syn-sniff: watching %s (this device's own address: %s) for %ds, "
                "Ctrl-C to stop early\n", ifname, self.c_str(), seconds);

    struct timeval tv {};
    tv.tv_sec = seconds;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    unsigned char buf[2048];
    int synCount = 0;
    for (;;) {
        ssize_t n = ::recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;  // timeout or error -- either way, done watching
        }
        // 2026-08-12 FIX: this originally read straight from buf,
        // wrongly assuming AF_PACKET strips the link-layer header when
        // filtered to ETH_P_IP. It does NOT for a real Ethernet-type
        // interface (wlan0 presents as ARPHRD_ETHER, standard for
        // managed/AP-mode WiFi) -- the protocol filter only controls
        // which frames get delivered, not whether the 14-byte Ethernet
        // header (dest MAC, src MAC, ethertype) is stripped. Every
        // packet was silently being misparsed starting mid-header,
        // which is why a first real hardware run captured ZERO SYNs at
        // all -- including this device's OWN two outbound connect()
        // attempts, which definitely sent real SYN packets (each one
        // got a real ECONNREFUSED/RST back). That's proof the bug was
        // in this tool, not evidence about the phone's behavior.
        if (static_cast<size_t>(n) < ETH_HLEN + static_cast<ssize_t>(sizeof(struct iphdr))) continue;
        auto *ip = reinterpret_cast<struct iphdr *>(buf + ETH_HLEN);
        if (ip->protocol != IPPROTO_TCP) continue;
        size_t ipHeaderLen = ip->ihl * 4u;
        if (static_cast<size_t>(n) < ETH_HLEN + ipHeaderLen + sizeof(struct tcphdr)) continue;
        auto *tcp = reinterpret_cast<struct tcphdr *>(buf + ETH_HLEN + ipHeaderLen);
        if (!tcp->syn) continue;  // only SYN (connection attempts), not every packet

        // inet_ntoa() reuses one static buffer -- copy each result out
        // to its own fixed buffer immediately rather than holding two
        // std::strings built from overlapping calls.
        struct in_addr srcAddr {ip->saddr};
        char srcBuf[INET_ADDRSTRLEN];
        std::snprintf(srcBuf, sizeof(srcBuf), "%s", ::inet_ntoa(srcAddr));
        struct in_addr dstAddr {ip->daddr};
        char dstBuf[INET_ADDRSTRLEN];
        std::snprintf(dstBuf, sizeof(dstBuf), "%s", ::inet_ntoa(dstAddr));
        std::string src = srcBuf;
        std::string dst = dstBuf;

        std::uint16_t srcPort = ntohs(tcp->source);
        std::uint16_t dstPort = ntohs(tcp->dest);
        bool towardsUs = (dst == self);
        ++synCount;
        std::printf("SYN #%d: %s:%u -> %s:%u  %s%s\n", synCount, src.c_str(), srcPort,
                    dst.c_str(), dstPort, towardsUs ? "(-> this device)" : "",
                    (src == self) ? "(<- from this device)" : "");
    }

    if (synCount == 0) {
        std::printf("wifi-syn-sniff: no TCP SYN packets observed in %ds\n", seconds);
    } else {
        std::printf("wifi-syn-sniff: done, %d SYN packet(s) observed\n", synCount);
    }
    ::close(sock);
    return 0;
}

#include "net.h"
#include "e1000.h"
#include "console.h"
#include "kmem.h"

static uint32_t g_my_ip = 0x0F02000A; // 10.0.2.15 (Big Endian)
static uint8_t g_my_mac[6];
static uint8_t g_gateway_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56}; // Default QEMU gateway MAC if ARP fails
static int g_gateway_mac_resolved = 0;

// Endianness helpers
static inline uint16_t swap16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

static inline uint32_t swap32(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v & 0xFF0000) >> 8) | ((v >> 24) & 0xFF);
}

#define htons(v) swap16(v)
#define ntohs(v) swap16(v)
#define htonl(v) swap32(v)
#define ntohl(v) swap32(v)

static uint16_t checksum(void *data, int len) {
    uint32_t sum = 0;
    uint16_t *ptr = (uint16_t *)data;
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    if (len > 0) sum += *(uint8_t *)ptr;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

void net_init(void) {
    if (e1000_init()) {
        e1000_get_mac(g_my_mac);
    }
}

static void handle_arp(arp_packet_t *arp) {
    if (ntohs(arp->opcode) == ARP_OP_REQUEST && arp->target_ip == g_my_ip) {
        // Send ARP Reply
        eth_header_t eth;
        for (int i = 0; i < 6; i++) {
            eth.dest[i] = arp->sender_mac[i];
            eth.src[i] = g_my_mac[i];
        }
        eth.type = htons(ETH_TYPE_ARP);

        arp_packet_t reply;
        reply.hw_type = htons(1);
        reply.proto_type = htons(ETH_TYPE_IPV4);
        reply.hw_size = 6;
        reply.proto_size = 4;
        reply.opcode = htons(ARP_OP_REPLY);
        for (int i = 0; i < 6; i++) {
            reply.sender_mac[i] = g_my_mac[i];
            reply.target_mac[i] = arp->sender_mac[i];
        }
        reply.sender_ip = g_my_ip;
        reply.target_ip = arp->sender_ip;

        uint8_t buffer[sizeof(eth_header_t) + sizeof(arp_packet_t)];
        kmem_memcpy(buffer, &eth, sizeof(eth));
        kmem_memcpy(buffer + sizeof(eth), &reply, sizeof(reply));
        e1000_send_packet(buffer, sizeof(buffer));
    } else if (ntohs(arp->opcode) == ARP_OP_REPLY) {
        // Cache gateway MAC if it matches our gateway IP (10.0.2.2)
        if (arp->sender_ip == 0x0202000A) { // 10.0.2.2
            for (int i = 0; i < 6; i++) g_gateway_mac[i] = arp->sender_mac[i];
            g_gateway_mac_resolved = 1;
        }
    }
}

static void handle_icmp(ip_header_t *ip, icmp_header_t *icmp, int len) {
    if (icmp->type == ICMP_TYPE_ECHO_REQUEST) {
        // Send Echo Reply
        icmp->type = ICMP_TYPE_ECHO_REPLY;
        icmp->checksum = 0;
        icmp->checksum = checksum(icmp, len);

        eth_header_t eth;
        // In a real stack we'd look up the MAC, but for a simple reply we can use the source MAC of the IP packet
        // Wait, IP header doesn't have MAC. We'd need to look up in ARP table or use the Ethernet header from the packet.
        // I'll modify net_poll to pass the Ethernet header.
    } else if (icmp->type == ICMP_TYPE_ECHO_REPLY) {
        puts("Ping reply from ");
        net_print_ip(ntohl(ip->src_ip));
        puts(": seq=");
        // Print sequence
        char buf[10];
        int seq = ntohs(icmp->seq);
        int pos = 0;
        if (seq == 0) buf[pos++] = '0';
        else {
            while (seq > 0) {
                buf[pos++] = '0' + (seq % 10);
                seq /= 10;
            }
        }
        while (pos > 0) putchar(buf[--pos]);
        putchar('\n');
    }
}

static void handle_ip(ip_header_t *ip, int len) {
    if (ip->dest_ip != g_my_ip && ip->dest_ip != 0xFFFFFFFF) return;

    if (ip->protocol == IP_PROTO_ICMP) {
        handle_icmp(ip, (icmp_header_t *)((uint8_t *)ip + (ip->version_ihl & 0x0F) * 4), ntohs(ip->length) - (ip->version_ihl & 0x0F) * 4);
    }
}

void net_poll(void) {
    uint8_t buffer[PACKET_SIZE];
    uint16_t length;

    while (e1000_receive_packet(buffer, &length)) {
        if (length < sizeof(eth_header_t)) continue;

        eth_header_t *eth = (eth_header_t *)buffer;
        uint16_t type = ntohs(eth->type);

        if (type == ETH_TYPE_ARP) {
            handle_arp((arp_packet_t *)(buffer + sizeof(eth_header_t)));
        } else if (type == ETH_TYPE_IPV4) {
            handle_ip((ip_header_t *)(buffer + sizeof(eth_header_t)), length - sizeof(eth_header_t));
        }
    }
}

void net_ping(uint32_t dest_ip) {
    if (dest_ip == 0) {
        puts("ping: invalid IP address\n");
        return;
    }
    // Send ARP request for gateway if not resolved
    if (!g_gateway_mac_resolved) {
        eth_header_t eth;
        for (int i = 0; i < 6; i++) {
            eth.dest[i] = 0xFF;
            eth.src[i] = g_my_mac[i];
        }
        eth.type = htons(ETH_TYPE_ARP);

        arp_packet_t arp;
        arp.hw_type = htons(1);
        arp.proto_type = htons(ETH_TYPE_IPV4);
        arp.hw_size = 6;
        arp.proto_size = 4;
        arp.opcode = htons(ARP_OP_REQUEST);
        for (int i = 0; i < 6; i++) {
            arp.sender_mac[i] = g_my_mac[i];
            arp.target_mac[i] = 0;
        }
        arp.sender_ip = g_my_ip;
        arp.target_ip = 0x0202000A; // 10.0.2.2 (Gateway)

        uint8_t buffer[sizeof(eth_header_t) + sizeof(arp_packet_t)];
        kmem_memcpy(buffer, &eth, sizeof(eth));
        kmem_memcpy(buffer + sizeof(eth), &arp, sizeof(arp));
        e1000_send_packet(buffer, sizeof(buffer));
        
        // Wait a bit for reply
        for (int i = 0; i < 1000000; i++) net_poll();
    }

    // Send ICMP Echo Request
    eth_header_t eth;
    for (int i = 0; i < 6; i++) {
        eth.dest[i] = g_gateway_mac[i]; // Send to gateway
        eth.src[i] = g_my_mac[i];
    }
    eth.type = htons(ETH_TYPE_IPV4);

    ip_header_t ip;
    ip.version_ihl = 0x45;
    ip.tos = 0;
    ip.length = htons(sizeof(ip_header_t) + sizeof(icmp_header_t));
    ip.id = htons(1);
    ip.flags_fragment = 0;
    ip.ttl = 64;
    ip.protocol = IP_PROTO_ICMP;
    ip.checksum = 0;
    ip.src_ip = g_my_ip;
    ip.dest_ip = htonl(dest_ip);
    ip.checksum = checksum(&ip, sizeof(ip));

    icmp_header_t icmp;
    icmp.type = ICMP_TYPE_ECHO_REQUEST;
    icmp.code = 0;
    icmp.checksum = 0;
    icmp.id = htons(1234);
    icmp.seq = htons(1);
    icmp.checksum = checksum(&icmp, sizeof(icmp));

    uint8_t buffer[sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(icmp_header_t)];
    kmem_memcpy(buffer, &eth, sizeof(eth));
    kmem_memcpy(buffer + sizeof(eth), &ip, sizeof(ip));
    kmem_memcpy(buffer + sizeof(eth) + sizeof(ip), &icmp, sizeof(icmp));

    puts("Pinging ");
    net_print_ip(dest_ip);
    puts("...\n");

    e1000_send_packet(buffer, sizeof(buffer));
}

uint32_t net_parse_ip(const char *ip_str) {
    uint32_t res = 0;
    int part = 0;
    int current = 0;
    int digit_count = 0;
    
    if (!ip_str || !ip_str[0]) return 0;
    
    for (int i = 0; ip_str[i]; i++) {
        if (ip_str[i] == '.') {
            if (digit_count == 0 || part >= 3) return 0;
            res = (res << 8) | current;
            current = 0;
            digit_count = 0;
            part++;
        } else if (ip_str[i] >= '0' && ip_str[i] <= '9') {
            current = current * 10 + (ip_str[i] - '0');
            if (current > 255) return 0;
            digit_count++;
        } else {
            return 0; // Invalid character
        }
    }
    
    if (part != 3 || digit_count == 0) return 0;
    res = (res << 8) | current;
    return res;
}

void net_print_ip(uint32_t ip) {
    for (int i = 0; i < 4; i++) {
        uint8_t val = (uint8_t)((ip >> ((3 - i) * 8)) & 0xFF);
        char buf[4];
        int pos = 0;
        if (val == 0) buf[pos++] = '0';
        else {
            while (val > 0) {
                buf[pos++] = '0' + (val % 10);
                val /= 10;
            }
        }
        while (pos > 0) putchar(buf[--pos]);
        if (i < 3) putchar('.');
    }
}

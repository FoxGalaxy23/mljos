#ifndef NET_H
#define NET_H

#include "common.h"

// Ethernet Header
typedef struct __attribute__((packed)) {
    uint8_t dest[6];
    uint8_t src[6];
    uint16_t type;
} eth_header_t;

#define ETH_TYPE_IPV4 0x0800
#define ETH_TYPE_ARP  0x0806

// ARP Packet
typedef struct __attribute__((packed)) {
    uint16_t hw_type;
    uint16_t proto_type;
    uint8_t hw_size;
    uint8_t proto_size;
    uint16_t opcode;
    uint8_t sender_mac[6];
    uint32_t sender_ip;
    uint8_t target_mac[6];
    uint32_t target_ip;
} arp_packet_t;

#define ARP_OP_REQUEST 0x0001
#define ARP_OP_REPLY   0x0002

// IP Header
typedef struct __attribute__((packed)) {
    uint8_t version_ihl;
    uint8_t tos;
    uint16_t length;
    uint16_t id;
    uint16_t flags_fragment;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dest_ip;
} ip_header_t;

#define IP_PROTO_ICMP 0x01
#define IP_PROTO_TCP  0x06
#define IP_PROTO_UDP  0x11

// ICMP Header
typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} icmp_header_t;

#define ICMP_TYPE_ECHO_REPLY   0x00
#define ICMP_TYPE_ECHO_REQUEST 0x08

void net_init(void);
void net_poll(void);
void net_ping(uint32_t dest_ip);
uint32_t net_parse_ip(const char *ip_str);
void net_print_ip(uint32_t ip);

#endif

#ifndef E1000_H
#define E1000_H

#include "common.h"

// Intel E1000 Registers
#define E1000_REG_CTRL      0x0000   // Device Control Register
#define E1000_REG_STATUS    0x0008   // Device Status Register
#define E1000_REG_EERD      0x0014   // EEPROM Read Register
#define E1000_REG_ICR       0x00C0   // Interrupt Cause Read Register
#define E1000_REG_IMS       0x00D0   // Interrupt Mask Set/Read Register
#define E1000_REG_IMC       0x00D8   // Interrupt Mask Clear Register
#define E1000_REG_RCTL      0x0100   // Receive Control Register
#define E1000_REG_TCTL      0x0400   // Transmit Control Register
#define E1000_REG_TIPG      0x0410   // Transmit IPG Register
#define E1000_REG_RDBAL     0x2800   // Receive Descriptor Base Address Low
#define E1000_REG_RDBAH     0x2804   // Receive Descriptor Base Address High
#define E1000_REG_RDLEN     0x2808   // Receive Descriptor Length
#define E1000_REG_RDH       0x2810   // Receive Descriptor Head
#define E1000_REG_RDT       0x2814   // Receive Descriptor Tail
#define E1000_REG_TDBAL     0x3800   // Transmit Descriptor Base Address Low
#define E1000_REG_TDBAH     0x3804   // Transmit Descriptor Base Address High
#define E1000_REG_TDLEN     0x3808   // Transmit Descriptor Length
#define E1000_REG_TDH       0x3810   // Transmit Descriptor Head
#define E1000_REG_TDT       0x3814   // Transmit Descriptor Tail
#define E1000_REG_RAL       0x5400   // Receive Address Low
#define E1000_REG_RAH       0x5404   // Receive Address High

// RCTL bits
#define E1000_RCTL_EN       (1 << 1)    // Receiver Enable
#define E1000_RCTL_SBP      (1 << 2)    // Store Bad Packets
#define E1000_RCTL_UPE      (1 << 3)    // Unicast Promiscuous Enabled
#define E1000_RCTL_MPE      (1 << 4)    // Multicast Promiscuous Enabled
#define E1000_RCTL_LPE      (1 << 5)    // Long Packet Reception Enable
#define E1000_RCTL_LBM_NONE (0 << 6)    // No Loopback
#define E1000_RCTL_RDMTS_HALF (0 << 8)  // Free Buffer Threshold is 1/2 of RDLEN
#define E1000_RCTL_BAM      (1 << 15)   // Broadcast Accept Mode
#define E1000_RCTL_BSIZE_2048 (0 << 16) // Buffer size 2048
#define E1000_RCTL_SECRC    (1 << 26)   // Strip Ethernet CRC

// TCTL bits
#define E1000_TCTL_EN       (1 << 1)    // Transmit Enable
#define E1000_TCTL_PSP      (1 << 3)    // Pad Short Packets
#define E1000_TCTL_CT_SHIFT 4           // Collision Threshold Shift
#define E1000_TCTL_COLD_SHIFT 12        // Collision Distance Shift

// Descriptors
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} e1000_rx_desc_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} e1000_tx_desc_t;

#define PACKET_SIZE   2048

int e1000_init(void);
int e1000_send_packet(const void *data, uint16_t length);
int e1000_receive_packet(void *buffer, uint16_t *length);
void e1000_get_mac(uint8_t mac[6]);

#endif

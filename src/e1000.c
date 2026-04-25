#include "e1000.h"
#include "console.h"
#include "io.h"
#include "kmem.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#define E1000_VENDOR_ID    0x8086
#define E1000_DEVICE_ID    0x100E // QEMU default
#define E1000_DEVICE_I217  0x153A
#define E1000_DEVICE_82577 0x10EA

static uint32_t g_e1000_mmio = 0;
static uint8_t g_e1000_mac[6];

#define RX_DESC_COUNT 32
#define TX_DESC_COUNT 32

static e1000_rx_desc_t *g_rx_descs;
static e1000_tx_desc_t *g_tx_descs;
static uint8_t *g_rx_buffers;
static uint8_t *g_tx_buffers;
static uint16_t g_rx_cur = 0;
static uint16_t g_tx_cur = 0;

static uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t address = 0x80000000U
        | ((uint32_t)bus << 16)
        | ((uint32_t)device << 11)
        | ((uint32_t)function << 8)
        | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void pci_config_write32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t val) {
    uint32_t address = 0x80000000U
        | ((uint32_t)bus << 16)
        | ((uint32_t)device << 11)
        | ((uint32_t)function << 8)
        | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, val);
}

static void e1000_write(uint32_t reg, uint32_t val) {
    if (!g_e1000_mmio) return;
    *(volatile uint32_t*)(uintptr_t)(g_e1000_mmio + reg) = val;
}

static uint32_t e1000_read(uint32_t reg) {
    if (!g_e1000_mmio) return 0;
    return *(volatile uint32_t*)(uintptr_t)(g_e1000_mmio + reg);
}

static int e1000_eeprom_read(uint8_t addr) {
    uint32_t val = 0;
    e1000_write(E1000_REG_EERD, 1 | ((uint32_t)addr << 8));
    for (int i = 0; i < 100000; i++) {
        val = e1000_read(E1000_REG_EERD);
        if (val & (1 << 4)) return (uint16_t)((val >> 16) & 0xFFFF);
    }
    return 0;
}

static void e1000_read_mac(void) {
    uint16_t val;
    val = e1000_eeprom_read(0);
    g_e1000_mac[0] = val & 0xFF;
    g_e1000_mac[1] = val >> 8;
    val = e1000_eeprom_read(1);
    g_e1000_mac[2] = val & 0xFF;
    g_e1000_mac[3] = val >> 8;
    val = e1000_eeprom_read(2);
    g_e1000_mac[4] = val & 0xFF;
    g_e1000_mac[5] = val >> 8;

    // Fallback if EEPROM doesn't work (some QEMU versions use RAL/RAH directly)
    if (g_e1000_mac[0] == 0 && g_e1000_mac[1] == 0 && g_e1000_mac[2] == 0) {
        uint32_t ral = e1000_read(E1000_REG_RAL);
        uint32_t rah = e1000_read(E1000_REG_RAH);
        g_e1000_mac[0] = ral & 0xFF;
        g_e1000_mac[1] = (ral >> 8) & 0xFF;
        g_e1000_mac[2] = (ral >> 16) & 0xFF;
        g_e1000_mac[3] = (ral >> 24) & 0xFF;
        g_e1000_mac[4] = rah & 0xFF;
        g_e1000_mac[5] = (rah >> 8) & 0xFF;
    }
}

void e1000_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = g_e1000_mac[i];
}

static void e1000_init_rx(void) {
    g_rx_descs = (e1000_rx_desc_t *)kmem_alloc(sizeof(e1000_rx_desc_t) * RX_DESC_COUNT, 16);
    g_rx_buffers = (uint8_t *)kmem_alloc(PACKET_SIZE * RX_DESC_COUNT, 16);

    for (int i = 0; i < RX_DESC_COUNT; i++) {
        g_rx_descs[i].addr = (uint64_t)(uintptr_t)(g_rx_buffers + i * PACKET_SIZE);
        g_rx_descs[i].status = 0;
    }

    e1000_write(E1000_REG_RDBAL, (uint32_t)(uintptr_t)g_rx_descs);
    e1000_write(E1000_REG_RDBAH, 0);
    e1000_write(E1000_REG_RDLEN, RX_DESC_COUNT * sizeof(e1000_rx_desc_t));
    e1000_write(E1000_REG_RDH, 0);
    e1000_write(E1000_REG_RDT, RX_DESC_COUNT - 1);
    e1000_write(E1000_REG_RCTL, E1000_RCTL_EN | E1000_RCTL_SBP | E1000_RCTL_BAM | E1000_RCTL_SECRC | E1000_RCTL_BSIZE_2048);
}

static void e1000_init_tx(void) {
    g_tx_descs = (e1000_tx_desc_t *)kmem_alloc(sizeof(e1000_tx_desc_t) * TX_DESC_COUNT, 16);
    g_tx_buffers = (uint8_t *)kmem_alloc(PACKET_SIZE * TX_DESC_COUNT, 16);

    for (int i = 0; i < TX_DESC_COUNT; i++) {
        g_tx_descs[i].addr = (uint64_t)(uintptr_t)(g_tx_buffers + i * PACKET_SIZE);
        g_tx_descs[i].status = 0;
    }

    e1000_write(E1000_REG_TDBAL, (uint32_t)(uintptr_t)g_tx_descs);
    e1000_write(E1000_REG_TDBAH, 0);
    e1000_write(E1000_REG_TDLEN, TX_DESC_COUNT * sizeof(e1000_tx_desc_t));
    e1000_write(E1000_REG_TDH, 0);
    e1000_write(E1000_REG_TDT, 0);
    e1000_write(E1000_REG_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP | (0x0F << E1000_TCTL_CT_SHIFT) | (0x40 << E1000_TCTL_COLD_SHIFT));
    e1000_write(E1000_REG_TIPG, 0x0060200A); // Default IPG values
}

int e1000_init(void) {
    int found = 0;
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            uint32_t id = pci_config_read32((uint8_t)bus, device, 0, 0);
            uint16_t vendor = (uint16_t)(id & 0xFFFF);
            uint16_t devid = (uint16_t)(id >> 16);
            if (vendor == E1000_VENDOR_ID && (devid == E1000_DEVICE_ID || devid == E1000_DEVICE_I217 || devid == E1000_DEVICE_82577)) {
                // Found it! Get BAR0
                g_e1000_mmio = pci_config_read32((uint8_t)bus, device, 0, 0x10) & ~0xF;
                
                // Enable Bus Mastering
                uint32_t cmd = pci_config_read32((uint8_t)bus, device, 0, 0x04);
                cmd |= 0x07; // IO | Memory | Bus Master
                pci_config_write32((uint8_t)bus, device, 0, 0x04, cmd);

                found = 1;
                break;
            }
        }
        if (found) break;
    }

    if (!found) {
        puts("e1000: device not found\n");
        return 0;
    }

    e1000_read_mac();
    puts("e1000: found device, MAC: ");
    for (int i = 0; i < 6; i++) {
        uint8_t digit = (g_e1000_mac[i] >> 4) & 0xF;
        putchar(digit < 10 ? '0' + digit : 'A' + digit - 10);
        digit = g_e1000_mac[i] & 0xF;
        putchar(digit < 10 ? '0' + digit : 'A' + digit - 10);
        if (i < 5) putchar(':');
    }
    putchar('\n');

    e1000_init_rx();
    e1000_init_tx();

    return 1;
}

int e1000_send_packet(const void *data, uint16_t length) {
    if (!g_e1000_mmio || length > PACKET_SIZE) return 0;

    // Check if the descriptor is free (status bit 0 - DD)
    // For simplicity in this first version, we just copy and send.
    // In a real driver we would wait if the ring is full.
    
    kmem_memcpy(g_tx_buffers + g_tx_cur * PACKET_SIZE, data, length);
    g_tx_descs[g_tx_cur].length = length;
    g_tx_descs[g_tx_cur].status = 0;
    g_tx_descs[g_tx_cur].cmd = (1 << 0) | (1 << 1) | (1 << 3); // EOP | IFCS | RS (Report Status)

    g_tx_cur = (g_tx_cur + 1) % TX_DESC_COUNT;
    e1000_write(E1000_REG_TDT, g_tx_cur);

    // Wait for send to complete (optional, but safer for simple logic)
    for (int i = 0; i < 1000000; i++) {
        if (g_tx_descs[(g_tx_cur + TX_DESC_COUNT - 1) % TX_DESC_COUNT].status & 0x01) return 1;
    }

    return 0;
}

int e1000_receive_packet(void *buffer, uint16_t *length) {
    if (!g_e1000_mmio || !(g_rx_descs[g_rx_cur].status & 0x01)) return 0; // No packet

    *length = g_rx_descs[g_rx_cur].length;
    kmem_memcpy(buffer, g_rx_buffers + g_rx_cur * PACKET_SIZE, *length);

    g_rx_descs[g_rx_cur].status = 0;
    uint16_t old_cur = g_rx_cur;
    g_rx_cur = (g_rx_cur + 1) % RX_DESC_COUNT;
    e1000_write(E1000_REG_RDT, old_cur);

    return 1;
}

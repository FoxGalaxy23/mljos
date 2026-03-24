#include "usb.h"
#include "console.h"
#include "io.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC
#define USB_MAX_CONTROLLERS 16
#define UHCI_USBCMD        0x00
#define UHCI_USBSTS        0x02
#define UHCI_USBINTR       0x04
#define UHCI_FRNUM         0x06
#define UHCI_FLBASEADD     0x08
#define UHCI_SOFMOD        0x0C
#define UHCI_PORTSC_BASE   0x10
#define UHCI_PORTSC_COUNT  2
#define UHCI_CMD_RS        0x0001
#define UHCI_CMD_HCRESET   0x0002
#define UHCI_CMD_GRESET    0x0004
#define UHCI_STS_HCHALTED  0x0020
#define UHCI_PORT_CCS      0x0001
#define UHCI_PORT_CSC      0x0002
#define UHCI_PORT_EN       0x0004
#define UHCI_PORT_ENC      0x0008
#define UHCI_PORT_RESET    0x0200
#define UHCI_PORT_LSDA     0x0100
#define UHCI_PID_OUT       0xE1
#define UHCI_PID_IN        0x69
#define UHCI_PID_SETUP     0x2D
#define UHCI_PTR_T         0x00000001U
#define UHCI_PTR_QH        0x00000002U
#define UHCI_TD_CTRL_ACTIVE 0x00800000U
#define UHCI_TD_CTRL_SPD   0x20000000U
#define UHCI_TD_CTRL_CERR3 0x18000000U
#define UHCI_TD_CTRL_LS    0x00040000U
#define UHCI_TD_CTRL_IOC   0x01000000U
#define UHCI_TD_TOKEN_D_SHIFT 19
#define UHCI_TD_TOKEN_MAXLEN_SHIFT 21
#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_ADDRESS    0x05
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_DESC_DEVICE        0x01
#define USB_DESC_CONFIGURATION 0x02
#define USB_DESC_INTERFACE     0x04
#define USB_DESC_ENDPOINT      0x05

#define USB_CLASS_MASS_STORAGE 0x08
#define USB_DIR_IN             0x80
#define USB_EP_ATTR_BULK       0x02
#define USB_MASS_PROTO_BULK_ONLY 0x50
#define USB_MASS_TAG            0x4D4C4A31U
#define USB_CBW_SIGNATURE       0x43425355U
#define USB_CSW_SIGNATURE       0x53425355U
#define USB_MAX_STORAGE_DEVICES 4
#define UHCI_MAX_TDS 32

typedef struct __attribute__((packed, aligned(16))) {
    uint32_t link_ptr;
    uint32_t element_ptr;
} uhci_qh_t;

typedef struct __attribute__((packed, aligned(16))) {
    uint32_t link_ptr;
    uint32_t ctrl_status;
    uint32_t token;
    uint32_t buffer_ptr;
} uhci_td_t;

typedef struct __attribute__((packed)) {
    uint8_t bm_request_type;
    uint8_t b_request;
    uint16_t w_value;
    uint16_t w_index;
    uint16_t w_length;
} usb_setup_packet_t;

typedef struct __attribute__((packed)) {
    uint8_t b_length;
    uint8_t b_descriptor_type;
    uint16_t bcd_usb;
    uint8_t b_device_class;
    uint8_t b_device_subclass;
    uint8_t b_device_protocol;
    uint8_t b_max_packet_size0;
    uint16_t id_vendor;
    uint16_t id_product;
    uint16_t bcd_device;
    uint8_t i_manufacturer;
    uint8_t i_product;
    uint8_t i_serial_number;
    uint8_t b_num_configurations;
} usb_device_descriptor_t;

typedef struct __attribute__((packed)) {
    uint8_t b_length;
    uint8_t b_descriptor_type;
    uint16_t w_total_length;
    uint8_t b_num_interfaces;
    uint8_t b_configuration_value;
    uint8_t i_configuration;
    uint8_t bm_attributes;
    uint8_t b_max_power;
} usb_configuration_descriptor_t;

typedef struct __attribute__((packed)) {
    uint8_t b_length;
    uint8_t b_descriptor_type;
    uint8_t b_interface_number;
    uint8_t b_alternate_setting;
    uint8_t b_num_endpoints;
    uint8_t b_interface_class;
    uint8_t b_interface_subclass;
    uint8_t b_interface_protocol;
    uint8_t i_interface;
} usb_interface_descriptor_t;

typedef struct __attribute__((packed)) {
    uint8_t b_length;
    uint8_t b_descriptor_type;
    uint8_t b_endpoint_address;
    uint8_t bm_attributes;
    uint16_t w_max_packet_size;
    uint8_t b_interval;
} usb_endpoint_descriptor_t;

typedef struct {
    int found;
    uint8_t interface_number;
    uint8_t configuration_value;
    uint8_t subclass;
    uint8_t protocol;
    uint8_t bulk_in_endpoint;
    uint8_t bulk_out_endpoint;
    uint16_t bulk_in_max_packet;
    uint16_t bulk_out_max_packet;
} usb_mass_storage_info_t;

typedef struct __attribute__((packed)) {
    uint32_t d_cbw_signature;
    uint32_t d_cbw_tag;
    uint32_t d_cbw_data_transfer_length;
    uint8_t bm_cbw_flags;
    uint8_t b_cbw_lun;
    uint8_t b_cbw_cb_length;
    uint8_t cbwcb[16];
} usb_mass_cbw_t;

typedef struct __attribute__((packed)) {
    uint32_t d_csw_signature;
    uint32_t d_csw_tag;
    uint32_t d_csw_data_residue;
    uint8_t b_csw_status;
} usb_mass_csw_t;

typedef struct {
    usb_controller_info_t controller;
    uint8_t low_speed;
    uint8_t address;
    uint8_t bulk_in_toggle;
    uint8_t bulk_out_toggle;
    usb_device_descriptor_t device_desc;
    usb_mass_storage_info_t storage;
} usb_mass_storage_session_t;

static usb_controller_info_t g_usb_controllers[USB_MAX_CONTROLLERS];
static int g_usb_scan_done = 0;
static int g_usb_controller_count = 0;
static usb_storage_device_info_t g_usb_storage_devices[USB_MAX_STORAGE_DEVICES];
static usb_mass_storage_session_t g_usb_storage_sessions[USB_MAX_STORAGE_DEVICES];
static int g_usb_storage_scan_done = 0;
static int g_usb_storage_device_count_cached = 0;
static uint32_t g_uhci_frame_list[1024] __attribute__((aligned(4096)));
static uhci_qh_t g_uhci_qh __attribute__((aligned(16)));
static uhci_td_t g_uhci_tds[UHCI_MAX_TDS] __attribute__((aligned(16)));
static usb_setup_packet_t g_uhci_setup_packet __attribute__((aligned(16)));
static uint8_t g_uhci_data_buffer[512] __attribute__((aligned(16)));
static int g_uhci_initialized[USB_MAX_CONTROLLERS] = {0};

static void print_uint(uint32_t value) {
    char buf[11];
    int pos = 10;

    buf[pos] = '\0';
    do {
        buf[--pos] = (char)('0' + (value % 10));
        value /= 10;
    } while (value > 0 && pos > 0);

    puts(&buf[pos]);
}

static void print_hex_digit(uint8_t value) {
    value &= 0x0F;
    putchar(value < 10 ? (char)('0' + value) : (char)('A' + (value - 10)));
}

static void print_hex8(uint8_t value) {
    print_hex_digit((uint8_t)(value >> 4));
    print_hex_digit(value);
}

static void print_hex16(uint16_t value) {
    print_hex8((uint8_t)(value >> 8));
    print_hex8((uint8_t)value);
}

static void *kmemset(void *dst, int value, uint32_t n) {
    uint8_t *d = (uint8_t*)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = (uint8_t)value;
    return dst;
}

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0]
        | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24)
        | ((uint32_t)p[1] << 16)
        | ((uint32_t)p[2] << 8)
        | (uint32_t)p[3];
}

static void write_le32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value & 0xFF);
    p[1] = (uint8_t)((value >> 8) & 0xFF);
    p[2] = (uint8_t)((value >> 16) & 0xFF);
    p[3] = (uint8_t)((value >> 24) & 0xFF);
}

static void write_be32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)((value >> 24) & 0xFF);
    p[1] = (uint8_t)((value >> 16) & 0xFF);
    p[2] = (uint8_t)((value >> 8) & 0xFF);
    p[3] = (uint8_t)(value & 0xFF);
}

static void write_be16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)((value >> 8) & 0xFF);
    p[1] = (uint8_t)(value & 0xFF);
}

static uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t address = 0x80000000U
        | ((uint32_t)bus << 16)
        | ((uint32_t)device << 11)
        | ((uint32_t)function << 8)
        | (offset & 0xFC);

    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static uint16_t pci_vendor_id(uint8_t bus, uint8_t device, uint8_t function) {
    return (uint16_t)(pci_config_read32(bus, device, function, 0x00) & 0xFFFF);
}

static uint8_t pci_irq_line(uint8_t bus, uint8_t device, uint8_t function) {
    return (uint8_t)(pci_config_read32(bus, device, function, 0x3C) & 0xFF);
}

static uint8_t pci_header_type(uint8_t bus, uint8_t device, uint8_t function) {
    return (uint8_t)((pci_config_read32(bus, device, function, 0x0C) >> 16) & 0xFF);
}

static uint16_t pci_find_io_base(uint8_t bus, uint8_t device, uint8_t function) {
    for (uint8_t offset = 0x10; offset <= 0x24; offset += 4) {
        uint32_t bar = pci_config_read32(bus, device, function, offset);
        if ((bar & 0x01) == 0) continue;
        return (uint16_t)(bar & 0xFFFC);
    }
    return 0;
}

static void usb_scan_controllers(void) {
    if (g_usb_scan_done) return;

    g_usb_controller_count = 0;
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            uint16_t vendor = pci_vendor_id((uint8_t)bus, device, 0);
            uint8_t function_limit = 1;

            if (vendor == 0xFFFF) continue;
            if (pci_header_type((uint8_t)bus, device, 0) & 0x80) function_limit = 8;

            for (uint8_t function = 0; function < function_limit; function++) {
                uint32_t class_reg;
                uint8_t class_code;
                uint8_t subclass;
                uint16_t function_vendor;

                function_vendor = pci_vendor_id((uint8_t)bus, device, function);
                if (function_vendor == 0xFFFF) continue;

                class_reg = pci_config_read32((uint8_t)bus, device, function, 0x08);
                class_code = (uint8_t)(class_reg >> 24);
                subclass = (uint8_t)(class_reg >> 16);
                if (class_code != 0x0C || subclass != 0x03) continue;
                if (g_usb_controller_count >= USB_MAX_CONTROLLERS) continue;

                g_usb_controllers[g_usb_controller_count].bus = (uint8_t)bus;
                g_usb_controllers[g_usb_controller_count].device = device;
                g_usb_controllers[g_usb_controller_count].function = function;
                g_usb_controllers[g_usb_controller_count].prog_if = (uint8_t)(class_reg >> 8);
                g_usb_controllers[g_usb_controller_count].vendor_id = function_vendor;
                g_usb_controllers[g_usb_controller_count].device_id = (uint16_t)(pci_config_read32((uint8_t)bus, device, function, 0x00) >> 16);
                g_usb_controllers[g_usb_controller_count].irq_line = pci_irq_line((uint8_t)bus, device, function);
                g_usb_controllers[g_usb_controller_count].io_base = pci_find_io_base((uint8_t)bus, device, function);
                g_usb_controller_count++;
            }
        }
    }

    g_usb_scan_done = 1;
}

static const char *usb_controller_type(uint8_t prog_if) {
    if (prog_if == 0x00) return "UHCI";
    if (prog_if == 0x10) return "OHCI";
    if (prog_if == 0x20) return "EHCI";
    if (prog_if == 0x30) return "xHCI";
    return "USB";
}

static void usb_delay(void) {
    for (uint32_t i = 0; i < 500000; i++) {
        __asm__ volatile ("nop");
    }
}

static void usb_delay_long(void) {
    for (uint32_t i = 0; i < 5000000; i++) {
        __asm__ volatile ("nop");
    }
}

static uint16_t uhci_portsc(const usb_controller_info_t *info, int port_index) {
    if (!info || !info->io_base || port_index < 0 || port_index >= UHCI_PORTSC_COUNT) return 0;
    return inw((uint16_t)(info->io_base + UHCI_PORTSC_BASE + (port_index * 2)));
}

static void uhci_write_portsc(const usb_controller_info_t *info, int port_index, uint16_t value) {
    if (!info || !info->io_base || port_index < 0 || port_index >= UHCI_PORTSC_COUNT) return;
    outw((uint16_t)(info->io_base + UHCI_PORTSC_BASE + (port_index * 2)), value);
}

static uint16_t uhci_read_reg16(const usb_controller_info_t *info, uint16_t reg) {
    if (!info || !info->io_base) return 0;
    return inw((uint16_t)(info->io_base + reg));
}

static void uhci_write_reg16(const usb_controller_info_t *info, uint16_t reg, uint16_t value) {
    if (!info || !info->io_base) return;
    outw((uint16_t)(info->io_base + reg), value);
}

static uint32_t uhci_read_reg32(const usb_controller_info_t *info, uint16_t reg) {
    if (!info || !info->io_base) return 0;
    return inl((uint16_t)(info->io_base + reg));
}

static void uhci_write_reg32(const usb_controller_info_t *info, uint16_t reg, uint32_t value) {
    if (!info || !info->io_base) return;
    outl((uint16_t)(info->io_base + reg), value);
}

static void print_port_status_flags(uint16_t status) {
    puts(status & UHCI_PORT_CCS ? "connected " : "empty ");
    puts(status & UHCI_PORT_EN ? "enabled " : "disabled ");
    puts(status & UHCI_PORT_LSDA ? "low-speed " : "full-speed ");
    if (status & UHCI_PORT_CSC) puts("changed ");
}

static uint32_t phys_addr(const void *ptr) {
    return (uint32_t)(uintptr_t)ptr;
}

static uint32_t uhci_make_token(uint8_t pid, uint8_t addr, uint8_t endp, uint8_t toggle, uint16_t max_len) {
    uint32_t token = pid;
    uint16_t encoded_len = max_len ? (uint16_t)(max_len - 1) : 0x7FF;

    token |= ((uint32_t)addr << 8);
    token |= ((uint32_t)endp << 15);
    token |= ((uint32_t)(toggle & 1) << UHCI_TD_TOKEN_D_SHIFT);
    token |= ((uint32_t)(encoded_len & 0x7FF) << UHCI_TD_TOKEN_MAXLEN_SHIFT);
    return token;
}

static int uhci_wait_for_reg_clear16(const usb_controller_info_t *info, uint16_t reg, uint16_t mask) {
    for (uint32_t i = 0; i < 1000000; i++) {
        if (!(uhci_read_reg16(info, reg) & mask)) return 1;
    }
    return 0;
}

static int uhci_init_controller(int controller_index, const usb_controller_info_t *info) {
    uint32_t qh_ptr = phys_addr(&g_uhci_qh);

    if (!info || info->prog_if != 0x00 || !info->io_base) return 0;
    if (controller_index >= 0 && controller_index < USB_MAX_CONTROLLERS && g_uhci_initialized[controller_index]) return 1;

    uhci_write_reg16(info, UHCI_USBCMD, 0);
    uhci_write_reg16(info, UHCI_USBCMD, UHCI_CMD_GRESET);
    usb_delay();
    uhci_write_reg16(info, UHCI_USBCMD, 0);
    uhci_write_reg16(info, UHCI_USBCMD, UHCI_CMD_HCRESET);
    if (!uhci_wait_for_reg_clear16(info, UHCI_USBCMD, UHCI_CMD_HCRESET)) return 0;

    kmemset(g_uhci_frame_list, 0, sizeof(g_uhci_frame_list));
    kmemset(&g_uhci_qh, 0, sizeof(g_uhci_qh));
    kmemset(g_uhci_tds, 0, sizeof(g_uhci_tds));
    g_uhci_qh.link_ptr = UHCI_PTR_T;
    g_uhci_qh.element_ptr = UHCI_PTR_T;

    for (int i = 0; i < 1024; i++) g_uhci_frame_list[i] = qh_ptr | UHCI_PTR_QH;

    uhci_write_reg16(info, UHCI_USBINTR, 0);
    uhci_write_reg16(info, UHCI_FRNUM, 0);
    uhci_write_reg32(info, UHCI_FLBASEADD, phys_addr(g_uhci_frame_list));
    (void)uhci_read_reg32(info, UHCI_FLBASEADD);
    outb((uint16_t)(info->io_base + UHCI_SOFMOD), 64);
    uhci_write_reg16(info, UHCI_USBSTS, 0xFFFF);
    uhci_write_reg16(info, UHCI_USBCMD, UHCI_CMD_RS);

    for (uint32_t i = 0; i < 1000000; i++) {
        if (!(uhci_read_reg16(info, UHCI_USBSTS) & UHCI_STS_HCHALTED)) {
            if (controller_index >= 0 && controller_index < USB_MAX_CONTROLLERS) g_uhci_initialized[controller_index] = 1;
            return 1;
        }
    }
    return 0;
}

static int uhci_prepare_port(const usb_controller_info_t *info, int port_index) {
    uint16_t status;
    int port = port_index - 1;

    if (!info || port < 0 || port >= UHCI_PORTSC_COUNT) return 0;
    status = uhci_portsc(info, port);
    if (!(status & UHCI_PORT_CCS)) return 0;

    uhci_write_portsc(info, port, (uint16_t)(status | UHCI_PORT_CSC | UHCI_PORT_ENC));
    status = uhci_portsc(info, port);
    uhci_write_portsc(info, port, (uint16_t)(status | UHCI_PORT_RESET));
    usb_delay_long();
    status = uhci_portsc(info, port);
    uhci_write_portsc(info, port, (uint16_t)(status & ~UHCI_PORT_RESET));
    usb_delay();
    status = uhci_portsc(info, port);
    uhci_write_portsc(info, port, (uint16_t)(status | UHCI_PORT_CSC | UHCI_PORT_ENC));
    status = uhci_portsc(info, port);
    if (!(status & UHCI_PORT_EN)) {
        uhci_write_portsc(info, port, (uint16_t)(status | UHCI_PORT_EN | UHCI_PORT_CSC | UHCI_PORT_ENC));
        usb_delay();
        status = uhci_portsc(info, port);
    }

    return (status & UHCI_PORT_EN) != 0;
}

static int uhci_run_td_chain(const usb_controller_info_t *info, uhci_td_t *first_td, uhci_td_t *last_td) {
    g_uhci_qh.element_ptr = phys_addr(first_td);

    for (uint32_t i = 0; i < 2000000; i++) {
        if (!(last_td->ctrl_status & UHCI_TD_CTRL_ACTIVE)) {
            g_uhci_qh.element_ptr = UHCI_PTR_T;
            return 1;
        }
    }

    g_uhci_qh.element_ptr = UHCI_PTR_T;
    return 0;
}

static int uhci_single_transaction(
    const usb_controller_info_t *info,
    uint8_t low_speed,
    uint8_t pid,
    uint8_t address,
    uint8_t endpoint,
    uint8_t toggle,
    void *data,
    uint16_t data_length
) {
    uint32_t status = UHCI_TD_CTRL_ACTIVE | UHCI_TD_CTRL_IOC | UHCI_TD_CTRL_CERR3;

    if (low_speed) status |= UHCI_TD_CTRL_LS;
    kmemset(g_uhci_tds, 0, sizeof(g_uhci_tds));
    g_uhci_tds[0].link_ptr = UHCI_PTR_T;
    g_uhci_tds[0].ctrl_status = status;
    g_uhci_tds[0].token = uhci_make_token(pid, address, endpoint, toggle, data_length);
    g_uhci_tds[0].buffer_ptr = data_length > 0 ? phys_addr(data) : 0;
    if (pid == UHCI_PID_IN) g_uhci_tds[0].ctrl_status |= UHCI_TD_CTRL_SPD;
    return uhci_run_td_chain(info, &g_uhci_tds[0], &g_uhci_tds[0]) && !(g_uhci_tds[0].ctrl_status & UHCI_TD_CTRL_ACTIVE);
}

static int uhci_control_transfer(
    const usb_controller_info_t *info,
    uint8_t low_speed,
    uint8_t address,
    uint8_t max_packet0,
    const usb_setup_packet_t *setup,
    void *data,
    uint16_t data_length,
    int direction_in
) {
    uint32_t common_status = UHCI_TD_CTRL_ACTIVE | UHCI_TD_CTRL_CERR3;
    uint8_t *data_ptr = (uint8_t*)data;
    uint16_t remaining = data_length;
    uint16_t chunk;
    int td_index = 0;
    int data_toggle = 1;

    if (low_speed) common_status |= UHCI_TD_CTRL_LS;
    kmemset(g_uhci_tds, 0, sizeof(g_uhci_tds));

    if (max_packet0 == 0) max_packet0 = 8;

    g_uhci_tds[td_index].ctrl_status = common_status;
    g_uhci_tds[td_index].token = uhci_make_token(UHCI_PID_SETUP, address, 0, 0, sizeof(*setup));
    g_uhci_tds[td_index].buffer_ptr = phys_addr(setup);
    td_index++;

    while (remaining > 0 && td_index < UHCI_MAX_TDS - 1) {
        chunk = remaining > max_packet0 ? max_packet0 : remaining;
        g_uhci_tds[td_index].ctrl_status = common_status;
        if (direction_in) g_uhci_tds[td_index].ctrl_status |= UHCI_TD_CTRL_SPD;
        g_uhci_tds[td_index].token = uhci_make_token(direction_in ? UHCI_PID_IN : UHCI_PID_OUT, address, 0, (uint8_t)data_toggle, chunk);
        g_uhci_tds[td_index].buffer_ptr = phys_addr(data_ptr);
        data_ptr += chunk;
        remaining = (uint16_t)(remaining - chunk);
        data_toggle ^= 1;
        td_index++;
    }

    if (remaining > 0 || td_index >= UHCI_MAX_TDS) return 0;

    g_uhci_tds[td_index].ctrl_status = common_status | UHCI_TD_CTRL_IOC;
    g_uhci_tds[td_index].token = uhci_make_token(direction_in ? UHCI_PID_OUT : UHCI_PID_IN, address, 0, 1, 0);
    g_uhci_tds[td_index].buffer_ptr = 0;

    for (int i = 0; i < td_index; i++) g_uhci_tds[i].link_ptr = phys_addr(&g_uhci_tds[i + 1]);
    g_uhci_tds[td_index].link_ptr = UHCI_PTR_T;

    return uhci_run_td_chain(info, &g_uhci_tds[0], &g_uhci_tds[td_index]) && !(g_uhci_tds[td_index].ctrl_status & UHCI_TD_CTRL_ACTIVE);
}

static int uhci_get_device_descriptor(
    const usb_controller_info_t *info,
    uint8_t low_speed,
    uint8_t address,
    uint8_t max_packet0,
    usb_device_descriptor_t *out_desc,
    uint16_t length
) {
    usb_setup_packet_t *setup = &g_uhci_setup_packet;

    if (!out_desc || length > sizeof(g_uhci_data_buffer)) return 0;
    kmemset(out_desc, 0, sizeof(*out_desc));
    kmemset(g_uhci_data_buffer, 0, sizeof(g_uhci_data_buffer));
    setup->bm_request_type = 0x80;
    setup->b_request = USB_REQ_GET_DESCRIPTOR;
    setup->w_value = (uint16_t)(USB_DESC_DEVICE << 8);
    setup->w_index = 0;
    setup->w_length = length;

    if (!uhci_control_transfer(info, low_speed, address, max_packet0, setup, g_uhci_data_buffer, length, 1)) return 0;
    for (uint16_t i = 0; i < length; i++) ((uint8_t*)out_desc)[i] = g_uhci_data_buffer[i];
    return 1;
}

static int uhci_set_address(const usb_controller_info_t *info, uint8_t low_speed, uint8_t new_address) {
    usb_setup_packet_t *setup = &g_uhci_setup_packet;

    setup->bm_request_type = 0x00;
    setup->b_request = USB_REQ_SET_ADDRESS;
    setup->w_value = new_address;
    setup->w_index = 0;
    setup->w_length = 0;

    if (!uhci_control_transfer(info, low_speed, 0, 8, setup, 0, 0, 0)) return 0;
    usb_delay();
    return 1;
}

static int uhci_set_configuration(const usb_controller_info_t *info, uint8_t low_speed, uint8_t address, uint8_t configuration_value) {
    usb_setup_packet_t *setup = &g_uhci_setup_packet;

    setup->bm_request_type = 0x00;
    setup->b_request = USB_REQ_SET_CONFIGURATION;
    setup->w_value = configuration_value;
    setup->w_index = 0;
    setup->w_length = 0;

    if (!uhci_control_transfer(info, low_speed, address, 8, setup, 0, 0, 0)) return 0;
    usb_delay();
    return 1;
}

static int uhci_get_configuration_descriptor(
    const usb_controller_info_t *info,
    uint8_t low_speed,
    uint8_t address,
    uint8_t max_packet0,
    uint8_t configuration_index,
    uint8_t *out,
    uint16_t length
) {
    usb_setup_packet_t *setup = &g_uhci_setup_packet;

    if (!out || length > sizeof(g_uhci_data_buffer)) return 0;
    kmemset(g_uhci_data_buffer, 0, sizeof(g_uhci_data_buffer));
    setup->bm_request_type = 0x80;
    setup->b_request = USB_REQ_GET_DESCRIPTOR;
    setup->w_value = (uint16_t)((USB_DESC_CONFIGURATION << 8) | configuration_index);
    setup->w_index = 0;
    setup->w_length = length;

    if (!uhci_control_transfer(info, low_speed, address, max_packet0, setup, g_uhci_data_buffer, length, 1)) return 0;
    for (uint16_t i = 0; i < length; i++) out[i] = g_uhci_data_buffer[i];
    return 1;
}

static void usb_print_endpoint_direction(uint8_t endpoint_address) {
    puts((endpoint_address & USB_DIR_IN) ? "IN" : "OUT");
}

static void usb_print_descriptor_interfaces(const uint8_t *buffer, uint16_t total_length, usb_mass_storage_info_t *ms_info, int verbose) {
    uint16_t offset = 0;
    int current_mass_storage = 0;

    if (ms_info) kmemset(ms_info, 0, sizeof(*ms_info));

    while (offset + 2 <= total_length) {
        uint8_t desc_len = buffer[offset];
        uint8_t desc_type = buffer[offset + 1];

        if (desc_len < 2 || offset + desc_len > total_length) break;

        if (desc_type == USB_DESC_INTERFACE && desc_len >= sizeof(usb_interface_descriptor_t)) {
            const usb_interface_descriptor_t *iface = (const usb_interface_descriptor_t*)(buffer + offset);

            current_mass_storage = 0;
            if (verbose) {
                puts("  interface ");
                print_uint(iface->b_interface_number);
                puts(": class 0x");
                print_hex8(iface->b_interface_class);
                puts(" subclass 0x");
                print_hex8(iface->b_interface_subclass);
                puts(" protocol 0x");
                print_hex8(iface->b_interface_protocol);
                putchar('\n');
            }

            if (iface->b_interface_class == USB_CLASS_MASS_STORAGE) {
                current_mass_storage = 1;
                if (ms_info) {
                    ms_info->found = 1;
                    ms_info->interface_number = iface->b_interface_number;
                    ms_info->subclass = iface->b_interface_subclass;
                    ms_info->protocol = iface->b_interface_protocol;
                    ms_info->bulk_in_endpoint = 0;
                    ms_info->bulk_out_endpoint = 0;
                    ms_info->bulk_in_max_packet = 0;
                    ms_info->bulk_out_max_packet = 0;
                }
            }
        } else if (desc_type == USB_DESC_ENDPOINT && desc_len >= sizeof(usb_endpoint_descriptor_t)) {
            const usb_endpoint_descriptor_t *ep = (const usb_endpoint_descriptor_t*)(buffer + offset);
            uint16_t max_packet = read_le16((const uint8_t*)&ep->w_max_packet_size);

            if (verbose) {
                puts("    endpoint 0x");
                print_hex8(ep->b_endpoint_address);
                puts(" ");
                usb_print_endpoint_direction(ep->b_endpoint_address);
                puts(" attr 0x");
                print_hex8(ep->bm_attributes);
                puts(" max ");
                print_uint(max_packet);
                putchar('\n');
            }

            if (current_mass_storage && ms_info && ((ep->bm_attributes & 0x03) == USB_EP_ATTR_BULK)) {
                if (ep->b_endpoint_address & USB_DIR_IN) {
                    ms_info->bulk_in_endpoint = ep->b_endpoint_address;
                    ms_info->bulk_in_max_packet = max_packet;
                } else {
                    ms_info->bulk_out_endpoint = ep->b_endpoint_address;
                    ms_info->bulk_out_max_packet = max_packet;
                }
            }
        }

        offset = (uint16_t)(offset + desc_len);
    }
}

static int usb_enumerate_mass_storage_device(
    int controller_index,
    int port_index,
    usb_mass_storage_session_t *session,
    int verbose
) {
    usb_controller_info_t info;
    usb_configuration_descriptor_t config_desc;
    usb_mass_storage_info_t ms_info;
    usb_device_descriptor_t desc;
    uint16_t status;
    uint16_t total_length;
    uint8_t low_speed;

    if (!session) return 0;
    if (!usb_get_controller(controller_index, &info)) {
        if (verbose) puts("usb: controller not found\n");
        return 0;
    }
    if (info.prog_if != 0x00) {
        if (verbose) puts("usb: only UHCI storage enumeration is implemented right now\n");
        return 0;
    }
    if (!info.io_base) {
        if (verbose) puts("usb: controller has no I/O base BAR\n");
        return 0;
    }
    if (!uhci_init_controller(controller_index, &info)) {
        if (verbose) puts("usb: failed to initialize UHCI controller\n");
        return 0;
    }
    if (!uhci_prepare_port(&info, port_index)) {
        if (verbose) puts("usb: no enabled device on that port\n");
        return 0;
    }

    status = uhci_portsc(&info, port_index - 1);
    low_speed = (status & UHCI_PORT_LSDA) ? 1 : 0;
    if (!uhci_get_device_descriptor(&info, low_speed, 0, 8, &desc, 8)) {
        if (verbose) puts("usb: failed to read initial device descriptor\n");
        return 0;
    }
    if (!uhci_set_address(&info, low_speed, 1)) {
        if (verbose) puts("usb: failed to assign USB address\n");
        return 0;
    }
    if (!uhci_get_device_descriptor(&info, low_speed, 1, desc.b_max_packet_size0, &desc, sizeof(desc))) {
        if (verbose) puts("usb: failed to read full device descriptor\n");
        return 0;
    }
    if (!uhci_get_configuration_descriptor(&info, low_speed, 1, desc.b_max_packet_size0, 0, (uint8_t*)&config_desc, sizeof(config_desc))) {
        if (verbose) puts("usb: failed to read configuration header\n");
        return 0;
    }
    total_length = config_desc.w_total_length;
    if (total_length > sizeof(g_uhci_data_buffer)) total_length = sizeof(g_uhci_data_buffer);
    if (!uhci_get_configuration_descriptor(&info, low_speed, 1, desc.b_max_packet_size0, 0, g_uhci_data_buffer, total_length)) {
        if (verbose) puts("usb: failed to read full configuration descriptor\n");
        return 0;
    }

    kmemset(&ms_info, 0, sizeof(ms_info));
    ms_info.configuration_value = config_desc.b_configuration_value;

    if (verbose) {
        puts("usb probe: device descriptor read successfully\n");
        puts("  speed: ");
        puts(low_speed ? "low-speed\n" : "full-speed\n");
        puts("  max packet: ");
        print_uint(desc.b_max_packet_size0);
        putchar('\n');
        puts("  vendor: 0x");
        print_hex16(desc.id_vendor);
        putchar('\n');
        puts("  product: 0x");
        print_hex16(desc.id_product);
        putchar('\n');
        puts("  class: 0x");
        print_hex8(desc.b_device_class);
        puts(" subclass: 0x");
        print_hex8(desc.b_device_subclass);
        puts(" protocol: 0x");
        print_hex8(desc.b_device_protocol);
        putchar('\n');
        puts("  configurations: ");
        print_uint(desc.b_num_configurations);
        putchar('\n');
        puts("  active config candidate: ");
        print_uint(config_desc.b_configuration_value);
        putchar('\n');
        puts("  interfaces:\n");
    }

    usb_print_descriptor_interfaces(g_uhci_data_buffer, total_length, &ms_info, verbose);
    if (!ms_info.found) {
        if (verbose) puts("  mass storage: no\n");
        return 0;
    }
    if (verbose) {
        puts("  mass storage: yes\n");
        puts("    subclass: 0x");
        print_hex8(ms_info.subclass);
        puts(" protocol: 0x");
        print_hex8(ms_info.protocol);
        putchar('\n');
        puts("    bulk-in: 0x");
        print_hex8(ms_info.bulk_in_endpoint);
        puts(" bulk-out: 0x");
        print_hex8(ms_info.bulk_out_endpoint);
        putchar('\n');
    }
    if (!uhci_set_configuration(&info, low_speed, 1, config_desc.b_configuration_value)) {
        if (verbose) puts("  set configuration: failed\n");
        return 0;
    }
    if (verbose) puts("  set configuration: ok\n");

    session->controller = info;
    session->low_speed = low_speed;
    session->address = 1;
    session->bulk_in_toggle = 0;
    session->bulk_out_toggle = 0;
    session->device_desc = desc;
    session->storage = ms_info;
    return 1;
}

static int usb_mass_bulk_transfer(
    usb_mass_storage_session_t *session,
    uint8_t endpoint_address,
    void *buffer,
    uint16_t length,
    uint8_t max_packet
) {
    uint8_t pid = (endpoint_address & USB_DIR_IN) ? UHCI_PID_IN : UHCI_PID_OUT;
    uint8_t endpoint = endpoint_address & 0x0F;
    uint8_t *toggle = (endpoint_address & USB_DIR_IN) ? &session->bulk_in_toggle : &session->bulk_out_toggle;
    uint8_t *bytes = (uint8_t*)buffer;
    uint16_t remaining = length;

    if (!session || !toggle || max_packet == 0) return 0;
    if (length == 0) {
        if (!uhci_single_transaction(&session->controller, session->low_speed, pid, session->address, endpoint, *toggle, 0, 0)) return 0;
        *toggle ^= 1;
        return 1;
    }

    while (remaining > 0) {
        uint16_t chunk = remaining > max_packet ? max_packet : remaining;
        if (!uhci_single_transaction(&session->controller, session->low_speed, pid, session->address, endpoint, *toggle, bytes, chunk)) {
            return 0;
        }
        *toggle ^= 1;
        bytes += chunk;
        remaining = (uint16_t)(remaining - chunk);
    }

    return 1;
}

static int usb_mass_bot_command(
    usb_mass_storage_session_t *session,
    const uint8_t *cdb,
    uint8_t cdb_length,
    int data_in,
    void *data,
    uint16_t data_length
) {
    usb_mass_cbw_t cbw;
    usb_mass_csw_t csw;

    if (!session || !cdb || cdb_length > 16) return 0;
    if (!session->storage.bulk_in_endpoint || !session->storage.bulk_out_endpoint) return 0;

    kmemset(&cbw, 0, sizeof(cbw));
    kmemset(&csw, 0, sizeof(csw));
    write_le32((uint8_t*)&cbw.d_cbw_signature, USB_CBW_SIGNATURE);
    write_le32((uint8_t*)&cbw.d_cbw_tag, USB_MASS_TAG);
    write_le32((uint8_t*)&cbw.d_cbw_data_transfer_length, data_length);
    cbw.bm_cbw_flags = data_in ? USB_DIR_IN : 0;
    cbw.b_cbw_lun = 0;
    cbw.b_cbw_cb_length = cdb_length;
    for (uint8_t i = 0; i < cdb_length; i++) cbw.cbwcb[i] = cdb[i];

    if (!usb_mass_bulk_transfer(session, session->storage.bulk_out_endpoint, &cbw, sizeof(cbw), (uint8_t)session->storage.bulk_out_max_packet)) return 0;
    if (data_length > 0) {
        if (!usb_mass_bulk_transfer(
            session,
            data_in ? session->storage.bulk_in_endpoint : session->storage.bulk_out_endpoint,
            data,
            data_length,
            data_in ? (uint8_t)session->storage.bulk_in_max_packet : (uint8_t)session->storage.bulk_out_max_packet
        )) {
            return 0;
        }
    }
    if (!usb_mass_bulk_transfer(session, session->storage.bulk_in_endpoint, &csw, sizeof(csw), (uint8_t)session->storage.bulk_in_max_packet)) return 0;
    if (read_le32((const uint8_t*)&csw.d_csw_signature) != USB_CSW_SIGNATURE) return 0;
    if (read_le32((const uint8_t*)&csw.d_csw_tag) != USB_MASS_TAG) return 0;
    if (csw.b_csw_status != 0) return 0;
    return 1;
}

static int usb_mass_inquiry(usb_mass_storage_session_t *session, uint8_t *out, uint16_t out_len) {
    uint8_t cdb[6];

    if (!out || out_len < 36) return 0;
    kmemset(cdb, 0, sizeof(cdb));
    kmemset(out, 0, out_len);
    cdb[0] = 0x12;
    cdb[4] = 36;
    return usb_mass_bot_command(session, cdb, sizeof(cdb), 1, out, 36);
}

static int usb_mass_read_capacity10(usb_mass_storage_session_t *session, uint8_t *out, uint16_t out_len) {
    uint8_t cdb[10];

    if (!out || out_len < 8) return 0;
    kmemset(cdb, 0, sizeof(cdb));
    kmemset(out, 0, out_len);
    cdb[0] = 0x25;
    return usb_mass_bot_command(session, cdb, sizeof(cdb), 1, out, 8);
}

static int usb_mass_read10(usb_mass_storage_session_t *session, uint32_t lba, uint16_t blocks, uint8_t *out, uint16_t out_len) {
    uint8_t cdb[10];
    uint32_t transfer_len = (uint32_t)blocks * 512U;

    if (!out || out_len < transfer_len || blocks == 0) return 0;
    kmemset(cdb, 0, sizeof(cdb));
    kmemset(out, 0, transfer_len);
    cdb[0] = 0x28;
    write_be32(&cdb[2], lba);
    write_be16(&cdb[7], blocks);
    return usb_mass_bot_command(session, cdb, sizeof(cdb), 1, out, (uint16_t)transfer_len);
}

static void usb_print_ascii_trimmed(const uint8_t *text, uint16_t len) {
    int started = 0;
    int last = -1;

    for (uint16_t i = 0; i < len; i++) {
        char c = (char)text[i];
        if (c != ' ') last = (int)i;
    }
    if (last < 0) {
        puts("(empty)");
        return;
    }
    for (int i = 0; i <= last; i++) {
        char c = (char)text[i];
        if (!started && c == ' ') continue;
        started = 1;
        putchar((c >= 32 && c <= 126) ? c : '?');
    }
}

static void usb_storage_reset_cache(void) {
    g_usb_storage_scan_done = 0;
    g_usb_storage_device_count_cached = 0;
    kmemset(g_usb_storage_devices, 0, sizeof(g_usb_storage_devices));
    kmemset(g_usb_storage_sessions, 0, sizeof(g_usb_storage_sessions));
}

static void usb_print_hex_line(const uint8_t *data, uint32_t offset, uint16_t count) {
    print_hex16((uint16_t)offset);
    puts(": ");
    for (uint16_t i = 0; i < count; i++) {
        print_hex8(data[i]);
        putchar(' ');
    }
    putchar('\n');
}

static void usb_print_sector_summary(const uint8_t *sector, uint16_t size) {
    if (!sector || size < 512) return;

    puts("usb read: first 64 bytes\n");
    for (uint16_t off = 0; off < 64; off += 16) {
        usb_print_hex_line(sector + off, off, 16);
    }

    if (sector[510] == 0x55 && sector[511] == 0xAA) {
        puts("usb read: sector has 0x55AA signature\n");
    } else {
        puts("usb read: sector does not have 0x55AA signature\n");
    }

    if (sector[54] == 'F' && sector[55] == 'A' && sector[56] == 'T') {
        puts("usb read: looks like FAT boot sector (FAT16)\n");
    } else if (sector[82] == 'F' && sector[83] == 'A' && sector[84] == 'T') {
        puts("usb read: looks like FAT boot sector (FAT32)\n");
    } else {
        uint8_t part_type = sector[450];
        if (part_type == 0x0B || part_type == 0x0C || part_type == 0x06 || part_type == 0x0E) {
            puts("usb read: looks like MBR with FAT partition entry\n");
        } else {
            puts("usb read: unknown sector layout\n");
        }
    }
}

int usb_controller_count(void) {
    usb_scan_controllers();
    return g_usb_controller_count;
}

int usb_get_controller(int index, usb_controller_info_t *out) {
    usb_scan_controllers();
    if (index < 0 || index >= g_usb_controller_count || !out) return 0;
    *out = g_usb_controllers[index];
    return 1;
}

void cmd_usb_list(void) {
    usb_scan_controllers();

    if (g_usb_controller_count == 0) {
        puts("usb: no PCI USB controllers detected\n");
        return;
    }

    puts("USB controllers:\n");
    for (int i = 0; i < g_usb_controller_count; i++) {
        usb_controller_info_t *info = &g_usb_controllers[i];

        puts("  ");
        print_uint((uint32_t)i);
        puts(": ");
        puts(usb_controller_type(info->prog_if));
        puts(" bus ");
        print_uint(info->bus);
        puts(" dev ");
        print_uint(info->device);
        puts(" fn ");
        print_uint(info->function);
        puts(" vendor 0x");
        print_hex16(info->vendor_id);
        puts(" device 0x");
        print_hex16(info->device_id);
        if (info->io_base) {
            puts(" io 0x");
            print_hex16(info->io_base);
        }
        if (info->irq_line) {
            puts(" irq ");
            print_uint(info->irq_line);
        }
        putchar('\n');
    }

    puts("usb: controller detection and UHCI root-port inspection are available; USB mass-storage driver is not implemented yet\n");
}

void cmd_usb_ports(int controller_index) {
    usb_controller_info_t info;

    if (!usb_get_controller(controller_index, &info)) {
        puts("usb ports: controller not found\n");
        return;
    }

    puts("usb ports for controller ");
    print_uint((uint32_t)controller_index);
    puts(" (");
    puts(usb_controller_type(info.prog_if));
    puts(")\n");

    if (info.prog_if != 0x00) {
        puts("usb ports: detailed root-port inspection is implemented only for UHCI right now\n");
        return;
    }
    if (!info.io_base) {
        puts("usb ports: controller has no I/O base BAR\n");
        return;
    }

    for (int port = 0; port < UHCI_PORTSC_COUNT; port++) {
        uint16_t status = uhci_portsc(&info, port);

        puts("  port ");
        print_uint((uint32_t)(port + 1));
        puts(": ");
        print_port_status_flags(status);
        puts("status 0x");
        print_hex16(status);
        putchar('\n');
    }
}

void cmd_usb_reset(int controller_index, int port_index) {
    usb_controller_info_t info;
    uint16_t status;
    int port = port_index - 1;

    if (!usb_get_controller(controller_index, &info)) {
        puts("usb reset: controller not found\n");
        return;
    }
    if (info.prog_if != 0x00) {
        puts("usb reset: port reset is implemented only for UHCI right now\n");
        return;
    }
    if (!info.io_base) {
        puts("usb reset: controller has no I/O base BAR\n");
        return;
    }
    if (port < 0 || port >= UHCI_PORTSC_COUNT) {
        puts("usb reset: port out of range\n");
        return;
    }

    status = uhci_portsc(&info, port);
    uhci_write_portsc(&info, port, (uint16_t)(status | UHCI_PORT_RESET));
    usb_delay();
    status = uhci_portsc(&info, port);
    uhci_write_portsc(&info, port, (uint16_t)(status & ~UHCI_PORT_RESET));
    usb_delay();
    status = uhci_portsc(&info, port);
    uhci_write_portsc(&info, port, (uint16_t)(status | UHCI_PORT_CSC));

    puts("usb reset: controller ");
    print_uint((uint32_t)controller_index);
    puts(" port ");
    print_uint((uint32_t)port_index);
    puts(" now ");
    print_port_status_flags(uhci_portsc(&info, port));
    putchar('\n');
    usb_storage_reset_cache();
}

static void usb_storage_scan_devices(void) {
    usb_mass_storage_session_t session;
    uint8_t capacity[8];
    int count = 0;
    int controllers = usb_controller_count();

    if (g_usb_storage_scan_done) return;

    usb_storage_reset_cache();
    for (int controller = 0; controller < controllers && count < USB_MAX_STORAGE_DEVICES; controller++) {
        for (int port = 1; port <= UHCI_PORTSC_COUNT && count < USB_MAX_STORAGE_DEVICES; port++) {
            if (!usb_enumerate_mass_storage_device(controller, port, &session, 0)) continue;
            if (session.storage.protocol != USB_MASS_PROTO_BULK_ONLY) continue;
            if (!usb_mass_read_capacity10(&session, capacity, sizeof(capacity))) continue;

            g_usb_storage_devices[count].present = 1;
            g_usb_storage_devices[count].controller_index = (uint8_t)controller;
            g_usb_storage_devices[count].port_index = (uint8_t)port;
            g_usb_storage_devices[count].vendor_id = session.device_desc.id_vendor;
            g_usb_storage_devices[count].product_id = session.device_desc.id_product;
            g_usb_storage_devices[count].sector_count = read_be32(capacity) + 1U;
            g_usb_storage_devices[count].sector_size = read_be32(capacity + 4);
            g_usb_storage_sessions[count] = session;
            count++;
        }
    }

    g_usb_storage_device_count_cached = count;
    g_usb_storage_scan_done = 1;
}

int usb_storage_device_count(void) {
    usb_storage_scan_devices();
    return g_usb_storage_device_count_cached;
}

int usb_storage_get_device(int index, usb_storage_device_info_t *out) {
    usb_storage_scan_devices();
    if (index < 0 || index >= g_usb_storage_device_count_cached || !out) return 0;
    *out = g_usb_storage_devices[index];
    return 1;
}

int usb_storage_read_sector(int index, uint32_t lba, uint8_t *buffer) {
    usb_storage_scan_devices();
    if (index < 0 || index >= g_usb_storage_device_count_cached || !buffer) return 0;
    if (g_usb_storage_devices[index].sector_size != 512U) return 0;
    if (lba >= g_usb_storage_devices[index].sector_count) return 0;
    return usb_mass_read10(&g_usb_storage_sessions[index], lba, 1, buffer, 512);
}

void cmd_usb_probe(int controller_index, int port_index) {
    usb_mass_storage_session_t session;
    if (!usb_enumerate_mass_storage_device(controller_index, port_index, &session, 1)) {
        puts("usb probe: no usable mass-storage session\n");
    }
}

void cmd_usb_storage(int controller_index, int port_index) {
    usb_mass_storage_session_t session;
    uint8_t inquiry[36];
    uint8_t capacity[8];
    uint32_t last_lba;
    uint32_t block_size;

    if (!usb_enumerate_mass_storage_device(controller_index, port_index, &session, 0)) {
        puts("usb storage: unable to enumerate USB mass-storage device\n");
        return;
    }
    if (session.storage.protocol != USB_MASS_PROTO_BULK_ONLY) {
        puts("usb storage: only Bulk-Only Transport is implemented right now\n");
        return;
    }
    if (!usb_mass_inquiry(&session, inquiry, sizeof(inquiry))) {
        puts("usb storage: SCSI INQUIRY failed\n");
        return;
    }
    if (!usb_mass_read_capacity10(&session, capacity, sizeof(capacity))) {
        puts("usb storage: READ CAPACITY(10) failed\n");
        return;
    }

    last_lba = read_be32(capacity);
    block_size = read_be32(capacity + 4);

    puts("usb storage: BOT/SCSI communication succeeded\n");
    puts("  vendor: ");
    usb_print_ascii_trimmed(inquiry + 8, 8);
    putchar('\n');
    puts("  product: ");
    usb_print_ascii_trimmed(inquiry + 16, 16);
    putchar('\n');
    puts("  revision: ");
    usb_print_ascii_trimmed(inquiry + 32, 4);
    putchar('\n');
    puts("  block size: ");
    print_uint(block_size);
    putchar('\n');
    puts("  last lba: ");
    print_uint(last_lba);
    putchar('\n');
    puts("  capacity bytes: ");
    print_uint((last_lba + 1U) * block_size);
    putchar('\n');
}

void cmd_usb_read(int controller_index, int port_index, uint32_t lba) {
    usb_mass_storage_session_t session;
    uint8_t sector[512];

    if (!usb_enumerate_mass_storage_device(controller_index, port_index, &session, 0)) {
        puts("usb read: unable to enumerate USB mass-storage device\n");
        return;
    }
    if (session.storage.protocol != USB_MASS_PROTO_BULK_ONLY) {
        puts("usb read: only Bulk-Only Transport is implemented right now\n");
        return;
    }
    if (!usb_mass_read10(&session, lba, 1, sector, sizeof(sector))) {
        puts("usb read: READ(10) failed\n");
        return;
    }

    puts("usb read: READ(10) succeeded for LBA ");
    print_uint(lba);
    putchar('\n');
    usb_print_sector_summary(sector, sizeof(sector));
}

#ifndef USB_H
#define USB_H

#include "common.h"

#define USB_MAX_STORAGE_DEVICES 4

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t prog_if;
    uint8_t irq_line;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t io_base;
} usb_controller_info_t;

typedef struct {
    int present;
    uint8_t controller_index;
    uint8_t port_index;
    uint16_t vendor_id;
    uint16_t product_id;
    uint32_t sector_count;
    uint32_t sector_size;
} usb_storage_device_info_t;

int usb_controller_count(void);
int usb_get_controller(int index, usb_controller_info_t *out);
int usb_storage_device_count(void);
int usb_storage_get_device(int index, usb_storage_device_info_t *out);
int usb_storage_read_sector(int index, uint32_t lba, uint8_t *buffer);
void cmd_usb_list(void);
void cmd_usb_ports(int controller_index);
void cmd_usb_reset(int controller_index, int port_index);
void cmd_usb_probe(int controller_index, int port_index);
void cmd_usb_storage(int controller_index, int port_index);
void cmd_usb_read(int controller_index, int port_index, uint32_t lba);

#endif

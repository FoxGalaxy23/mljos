#include "disk.h"
#include "console.h"
#include "io.h"
#include "kstring.h"
#include "shell.h"
#include "sdk/mljos_api.h"
#include "usb.h"

typedef void (*app_entry_t)(mljos_api_t*);

extern uint8_t _kernel_start[];
extern uint8_t _kernel_end[];

#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_LFN       0x0F
#define FAT32_EOC            0x0FFFFFFF
#define FAT32_MIN_CLUSTERS   65525U
#define FAT32_MAX_CLUSTERS   0x0FFFFFEFU
#define ATA_POLL_TIMEOUT     1000000U
#define ATA_MAX_DEVICES      4
#define DISK_MAX_DEVICES     (ATA_MAX_DEVICES + USB_MAX_STORAGE_DEVICES)

typedef struct __attribute__((packed)) {
    uint8_t name[11];
    uint8_t attr;
    uint8_t ntres;
    uint8_t crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t last_access_date;
    uint16_t first_cluster_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t first_cluster_lo;
    uint32_t file_size;
} fat32_dir_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t order;
    uint16_t name1[5];
    uint8_t attr;
    uint8_t type;
    uint8_t checksum;
    uint16_t name2[6];
    uint16_t first_cluster_lo;
    uint16_t name3[2];
} fat32_lfn_entry_t;

typedef struct {
    int mounted;
    uint32_t partition_lba;
    uint32_t total_sectors;
    uint32_t fat_start_lba;
    uint32_t data_start_lba;
    uint32_t fat_size_sectors;
    uint32_t total_clusters;
    uint32_t root_cluster;
    uint16_t reserved_sector_count;
    uint8_t num_fats;
    uint8_t sectors_per_cluster;
    char current_path[128];
} fat32_volume_t;

typedef struct {
    uint32_t sector_lba;
    uint16_t offset;
} fat32_dir_slot_t;

typedef struct {
    fat32_dir_entry_t entry;
    fat32_dir_slot_t slot;
    fat32_dir_slot_t lfn_slots[20];
    int lfn_count;
    char display_name[128];
    int has_long_name;
} fat32_lookup_result_t;

typedef struct {
    uint16_t io_base;
    uint8_t drive_select;
    uint8_t identify_select;
    uint32_t total_sectors;
    int present;
    const char *name;
} ata_device_t;

typedef enum {
    DISK_BACKEND_NONE = 0,
    DISK_BACKEND_ATA = 1,
    DISK_BACKEND_USB = 2
} disk_backend_type_t;

typedef struct {
    disk_backend_type_t type;
    int backend_index;
    uint32_t total_sectors;
    int writable;
    char label[16];
} disk_device_t;

static fat32_volume_t g_fat32_volumes[DISK_MAX_DEVICES] = {0};
static ata_device_t g_ata_devices[ATA_MAX_DEVICES] = {
    {0x1F0, 0xE0, 0xA0, 0, 0, "ata0"},
    {0x1F0, 0xF0, 0xB0, 0, 0, "ata1"},
    {0x170, 0xE0, 0xA0, 0, 0, "ata2"},
    {0x170, 0xF0, 0xB0, 0, 0, "ata3"}
};
static disk_device_t g_disk_devices[DISK_MAX_DEVICES] = {0};
static int g_disk_active_index = 0;
static int g_disk_devices_probed = 0;
static int g_disk_ever_probed = 0;
static int g_disk_device_count = 0;
static int g_disk_io_error = 0;
static int g_disk_system_index = -1;

static int fat32_build_short_name(const char *name, uint8_t out[11]);
static void disk_probe_devices(void);
static fat32_volume_t *disk_current_volume(void);
static disk_device_t *disk_current_device(void);
static ata_device_t *disk_current_ata_device(void);
static int ata_read_sector(uint32_t lba, uint8_t *buffer);
static int ata_write_sector(uint32_t lba, const uint8_t *buffer);

#define g_fat32 (*disk_current_volume())

static int disk_is_elf_image(const char *data, uint32_t size) {
    return size >= 4
        && (uint8_t)data[0] == 0x7F
        && data[1] == 'E'
        && data[2] == 'L'
        && data[3] == 'F';
}

static int fat32_ascii_equal(const char *a, const char *b) {
    int i = 0;
    while (a[i] && b[i]) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static int fat32_utf8_decode_char(const char *src, uint32_t *codepoint, int *bytes_used) {
    uint8_t c0 = (uint8_t)src[0];

    if (c0 < 0x80) {
        *codepoint = c0;
        *bytes_used = 1;
        return 1;
    }

    if ((c0 & 0xE0) == 0xC0) {
        uint8_t c1 = (uint8_t)src[1];
        uint32_t value;

        if ((c1 & 0xC0) != 0x80) return 0;
        value = ((uint32_t)(c0 & 0x1F) << 6) | (uint32_t)(c1 & 0x3F);
        if (value < 0x80) return 0;
        *codepoint = value;
        *bytes_used = 2;
        return 1;
    }

    if ((c0 & 0xF0) == 0xE0) {
        uint8_t c1 = (uint8_t)src[1];
        uint8_t c2 = (uint8_t)src[2];
        uint32_t value;

        if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) return 0;
        value = ((uint32_t)(c0 & 0x0F) << 12)
            | ((uint32_t)(c1 & 0x3F) << 6)
            | (uint32_t)(c2 & 0x3F);
        if (value < 0x800 || (value >= 0xD800 && value <= 0xDFFF)) return 0;
        *codepoint = value;
        *bytes_used = 3;
        return 1;
    }

    if ((c0 & 0xF8) == 0xF0) {
        uint8_t c1 = (uint8_t)src[1];
        uint8_t c2 = (uint8_t)src[2];
        uint8_t c3 = (uint8_t)src[3];
        uint32_t value;

        if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) return 0;
        value = ((uint32_t)(c0 & 0x07) << 18)
            | ((uint32_t)(c1 & 0x3F) << 12)
            | ((uint32_t)(c2 & 0x3F) << 6)
            | (uint32_t)(c3 & 0x3F);
        if (value < 0x10000 || value > 0x10FFFF) return 0;
        *codepoint = value;
        *bytes_used = 4;
        return 1;
    }

    return 0;
}

static int fat32_utf16_from_utf8(const char *src, uint16_t *out, int max_units) {
    int pos = 0;
    int out_pos = 0;

    while (src[pos]) {
        uint32_t codepoint = 0;
        int bytes_used = 0;

        if (!fat32_utf8_decode_char(src + pos, &codepoint, &bytes_used)) return -1;
        if (codepoint <= 0xFFFF) {
            if (out_pos >= max_units) return -1;
            out[out_pos++] = (uint16_t)codepoint;
        } else {
            uint32_t surrogate = codepoint - 0x10000;
            if (out_pos + 1 >= max_units) return -1;
            out[out_pos++] = (uint16_t)(0xD800 + (surrogate >> 10));
            out[out_pos++] = (uint16_t)(0xDC00 + (surrogate & 0x3FF));
        }
        pos += bytes_used;
    }

    return out_pos;
}

static int fat32_utf8_encode_char(uint32_t codepoint, char *out, int maxlen) {
    if (codepoint <= 0x7F) {
        if (maxlen < 1) return 0;
        out[0] = (char)codepoint;
        return 1;
    }

    if (codepoint <= 0x7FF) {
        if (maxlen < 2) return 0;
        out[0] = (char)(0xC0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    }

    if (codepoint <= 0xFFFF) {
        if (maxlen < 3) return 0;
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    }

    if (codepoint <= 0x10FFFF) {
        if (maxlen < 4) return 0;
        out[0] = (char)(0xF0 | (codepoint >> 18));
        out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[3] = (char)(0x80 | (codepoint & 0x3F));
        return 4;
    }

    return 0;
}

static void fat32_path_copy(char *dst, const char *src) {
    int i = 0;
    while (src[i] && i < 127) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void fat32_reset_cwd(void) {
    g_fat32.current_path[0] = '/';
    g_fat32.current_path[1] = '\0';
}

static void *kmemset(void *dst, int value, uint32_t n) {
    uint8_t *d = (uint8_t*)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = (uint8_t)value;
    return dst;
}

static void *kmemcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t*)dst;
    const uint8_t *s = (const uint8_t*)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

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

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0]
        | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

static void write_le16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value & 0xFF);
    p[1] = (uint8_t)((value >> 8) & 0xFF);
}

static void write_le32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value & 0xFF);
    p[1] = (uint8_t)((value >> 8) & 0xFF);
    p[2] = (uint8_t)((value >> 16) & 0xFF);
    p[3] = (uint8_t)((value >> 24) & 0xFF);
}

static int is_fat32_eoc(uint32_t value) {
    return (value & 0x0FFFFFFF) >= 0x0FFFFFF8;
}

static fat32_volume_t *disk_current_volume(void) {
    if (g_disk_active_index < 0 || g_disk_active_index >= DISK_MAX_DEVICES) g_disk_active_index = 0;
    return &g_fat32_volumes[g_disk_active_index];
}

static int disk_pick_default_device(void) {
    for (int i = 0; i < g_disk_device_count; i++) {
        if (g_disk_devices[i].type != DISK_BACKEND_NONE) {
            g_disk_active_index = i;
            return 1;
        }
    }
    g_disk_active_index = 0;
    return 0;
}

static disk_device_t *disk_current_device(void) {
    disk_probe_devices();
    if (g_disk_active_index >= 0
        && g_disk_active_index < g_disk_device_count
        && g_disk_devices[g_disk_active_index].type != DISK_BACKEND_NONE) {
        return &g_disk_devices[g_disk_active_index];
    }
    if (!disk_pick_default_device()) return 0;
    return &g_disk_devices[g_disk_active_index];
}

static ata_device_t *disk_current_ata_device(void) {
    disk_device_t *device = disk_current_device();

    if (!device || device->type != DISK_BACKEND_ATA) return 0;
    if (device->backend_index < 0 || device->backend_index >= ATA_MAX_DEVICES) return 0;
    if (!g_ata_devices[device->backend_index].present) return 0;
    return &g_ata_devices[device->backend_index];
}

static int ata_wait_bsy(uint16_t io_base) {
    for (uint32_t i = 0; i < ATA_POLL_TIMEOUT; i++) {
        if (!(inb(io_base + 7) & 0x80)) return 1;
    }
    return 0;
}

static int ata_wait_drq_or_err(uint16_t io_base) {
    for (uint32_t i = 0; i < ATA_POLL_TIMEOUT; i++) {
        uint8_t status = inb(io_base + 7);
        if (status & 0x01) return 0;
        if (!(status & 0x80) && (status & 0x08)) return 1;
    }
    return 0;
}

static int ata_device_read_sector(const ata_device_t *device, uint32_t lba, uint8_t *buffer) {
    uint16_t io_base;

    if (!device || !device->present) return 0;
    io_base = device->io_base;

    if (!ata_wait_bsy(io_base)) return 0;
    outb(io_base + 6, device->drive_select | ((lba >> 24) & 0x0F));
    outb(io_base + 2, 1);
    outb(io_base + 3, (uint8_t)lba);
    outb(io_base + 4, (uint8_t)(lba >> 8));
    outb(io_base + 5, (uint8_t)(lba >> 16));
    outb(io_base + 7, 0x20);
    if (!ata_wait_bsy(io_base)) return 0;
    if (!ata_wait_drq_or_err(io_base)) return 0;

    for (int i = 0; i < 256; i++) {
        uint16_t data = inw(io_base);
        buffer[i * 2] = (uint8_t)(data & 0xFF);
        buffer[i * 2 + 1] = (uint8_t)(data >> 8);
    }
    return 1;
}

static int ata_device_write_sector(const ata_device_t *device, uint32_t lba, const uint8_t *buffer) {
    uint8_t status;
    uint16_t io_base;

    if (!device || !device->present) return 0;
    io_base = device->io_base;

    if (!ata_wait_bsy(io_base)) return 0;
    outb(io_base + 6, device->drive_select | ((lba >> 24) & 0x0F));
    outb(io_base + 2, 1);
    outb(io_base + 3, (uint8_t)lba);
    outb(io_base + 4, (uint8_t)(lba >> 8));
    outb(io_base + 5, (uint8_t)(lba >> 16));
    outb(io_base + 7, 0x30);
    if (!ata_wait_bsy(io_base)) return 0;
    if (!ata_wait_drq_or_err(io_base)) return 0;

    for (int i = 0; i < 256; i++) {
        uint16_t data = buffer[i * 2] | ((uint16_t)buffer[i * 2 + 1] << 8);
        outw(io_base, data);
    }

    if (!ata_wait_bsy(io_base)) return 0;
    status = inb(io_base + 7);
    if (status & 0x01) return 0;
    return 1;
}

static uint32_t ata_identify_total_sectors(ata_device_t *device) {
    uint8_t identify[512];
    uint16_t io_base;

    if (!device) return 0;
    io_base = device->io_base;

    outb(io_base + 6, device->identify_select);
    outb(io_base + 2, 0);
    outb(io_base + 3, 0);
    outb(io_base + 4, 0);
    outb(io_base + 5, 0);
    outb(io_base + 7, 0xEC);

    if (inb(io_base + 7) == 0) return 0;
    if (inb(io_base + 4) != 0 || inb(io_base + 5) != 0) return 0;
    if (!ata_wait_drq_or_err(io_base)) return 0;

    for (int i = 0; i < 256; i++) {
        uint16_t word = inw(io_base);
        identify[i * 2] = (uint8_t)(word & 0xFF);
        identify[i * 2 + 1] = (uint8_t)(word >> 8);
    }

    // Check for LBA48 support (word 83, bit 10)
    uint16_t feat = (uint16_t)identify[166] | ((uint16_t)identify[167] << 8);
    if (feat & (1 << 10)) {
        // Return 32-bit field from LBA48 capacity (words 100-103)
        // Since our OS uses 32-bit LBA internally, we cap it at 4 billion sectors (2TB)
        return read_le32(&identify[200]);
    }

    return read_le32(&identify[120]);
}

static void disk_probe_devices(void) {
    if (g_disk_devices_probed) return;

    int first_run = !g_disk_ever_probed;
    g_disk_device_count = 0;
    kmemset(g_disk_devices, 0, sizeof(g_disk_devices));
    for (int i = 0; i < ATA_MAX_DEVICES; i++) {
        uint8_t identify[512];
        uint16_t io_base = g_ata_devices[i].io_base;
        
        // Low-level check for ATAPI (CD-ROM)
        int is_atapi = 0;
        outb(io_base + 6, g_ata_devices[i].identify_select);
        usb_delay(); // reuse usb_delay for a tiny wait
        if (inb(io_base + 4) == 0x14 && inb(io_base + 5) == 0xEB) is_atapi = 1;

        g_ata_devices[i].total_sectors = ata_identify_total_sectors(&g_ata_devices[i]);
        g_ata_devices[i].present = g_ata_devices[i].total_sectors > 0;
        
        if (g_ata_devices[i].present && g_disk_device_count < DISK_MAX_DEVICES) {
            disk_device_t *device = &g_disk_devices[g_disk_device_count];

            device->type = DISK_BACKEND_ATA;
            device->backend_index = i;
            device->total_sectors = g_ata_devices[i].total_sectors;
            device->writable = is_atapi ? 0 : 1;
            strcpy(device->label, g_ata_devices[i].name);
            if (first_run && !g_fat32_volumes[g_disk_device_count].current_path[0]) {
                g_disk_active_index = g_disk_device_count;
                fat32_reset_cwd();
            }
            g_disk_device_count++;
        }
    }


    for (int i = 0; i < usb_storage_device_count() && g_disk_device_count < DISK_MAX_DEVICES; i++) {
        usb_storage_device_info_t usb_info;
        disk_device_t *device;

        if (!usb_storage_get_device(i, &usb_info)) continue;
        if (!usb_info.present || usb_info.sector_size != 512U) continue;

        device = &g_disk_devices[g_disk_device_count];
        device->type = DISK_BACKEND_USB;
        device->backend_index = i;
        device->total_sectors = usb_info.sector_count;
        device->writable = 1;
        strcpy(device->label, "usb");
        if (first_run && !g_fat32_volumes[g_disk_device_count].current_path[0]) {
            g_disk_active_index = g_disk_device_count;
            fat32_reset_cwd();
        }
        g_disk_device_count++;
    }

    g_disk_devices_probed = 1;
    g_disk_ever_probed = 1;
    if (first_run) (void)disk_pick_default_device();
}

void disk_probe_devices_reset(void) {
    g_disk_devices_probed = 0;
}

int disk_get_system_device(void) {
    return g_disk_system_index;
}

void disk_set_system_device(int index) {
    if (index >= 0 && index < DISK_MAX_DEVICES) g_disk_system_index = index;
}

static void ata_write_zero_sectors(uint32_t lba, uint32_t count) {
    uint8_t zero[512];
    disk_device_t *device = disk_current_device();

    if (!device) {
        g_disk_io_error = 1;
        return;
    }

    kmemset(zero, 0, sizeof(zero));
    
    if (device->type == DISK_BACKEND_USB) {
        // Optimized multi-sector zeroing for USB
        // We reuse the same zero sector multiple times in the TD chain
        // but since our writer wants a continuous buffer, we do it in smaller steps
        // to keep memory usage low. 1 sector at a time is slow but reliable if BOT is fast.
        // Wait, I updated usb_mass_write10 to handle any length!
        // But it still needs the data in a buffer.
        
        for (uint32_t i = 0; i < count; i++) {
            if (!usb_storage_write_sector(device->backend_index, lba + i, zero)) {
                g_disk_io_error = 1;
                return;
            }
            if (i % 128 == 0) usb_delay(); // Periodic breather for slow sticks
        }
    } else {
        for (uint32_t i = 0; i < count; i++) {
            if (!ata_write_sector(lba + i, zero)) {
                g_disk_io_error = 1;
                return;
            }
        }
    }
}

static int ata_read_sector(uint32_t lba, uint8_t *buffer) {
    disk_device_t *device = disk_current_device();

    if (!device) return 0;
    if (device->type == DISK_BACKEND_ATA) return ata_device_read_sector(disk_current_ata_device(), lba, buffer);
    if (device->type == DISK_BACKEND_USB) return usb_storage_read_sector(device->backend_index, lba, buffer);
    return 0;
}

static int ata_write_sector(uint32_t lba, const uint8_t *buffer) {
    disk_device_t *device = disk_current_device();
    if (!device) return 0;
    if (device->type == DISK_BACKEND_ATA) return ata_device_write_sector(disk_current_ata_device(), lba, buffer);
    if (device->type == DISK_BACKEND_USB) return usb_storage_write_sector(device->backend_index, lba, buffer);
    return 0;
}

static uint32_t fat32_cluster_to_lba(uint32_t cluster) {
    return g_fat32.data_start_lba + ((cluster - 2) * g_fat32.sectors_per_cluster);
}

static uint32_t fat32_dir_first_cluster(const fat32_dir_entry_t *entry) {
    return ((uint32_t)entry->first_cluster_hi << 16) | entry->first_cluster_lo;
}

static void fat32_set_dir_first_cluster(fat32_dir_entry_t *entry, uint32_t cluster) {
    entry->first_cluster_hi = (uint16_t)(cluster >> 16);
    entry->first_cluster_lo = (uint16_t)(cluster & 0xFFFF);
}

static void fat32_read_cluster(uint32_t cluster, uint8_t *buffer) {
    uint32_t first_lba = fat32_cluster_to_lba(cluster);
    for (uint8_t i = 0; i < g_fat32.sectors_per_cluster; i++) {
        if (!ata_read_sector(first_lba + i, buffer + (i * 512))) {
            g_disk_io_error = 1;
            return;
        }
    }
}

static void fat32_write_cluster(uint32_t cluster, const uint8_t *buffer) {
    uint32_t first_lba = fat32_cluster_to_lba(cluster);
    for (uint8_t i = 0; i < g_fat32.sectors_per_cluster; i++) {
        if (!ata_write_sector(first_lba + i, buffer + (i * 512))) {
            g_disk_io_error = 1;
            return;
        }
    }
}

static int fat32_mount(void) {
    uint8_t sector[512];
    uint32_t boot_lba = 0;

    if (!ata_read_sector(0, sector)) return 0;
    if (sector[510] != 0x55 || sector[511] != 0xAA) return 0;

    if (read_le16(&sector[11]) != 512 || read_le32(&sector[36]) == 0) {
        uint32_t part_lba = read_le32(&sector[454]);
        if (part_lba == 0) return 0;
        if (!ata_read_sector(part_lba, sector)) return 0;
        if (sector[510] != 0x55 || sector[511] != 0xAA) return 0;
        if (read_le16(&sector[11]) != 512 || read_le32(&sector[36]) == 0) return 0;
        boot_lba = part_lba;
    }

    g_fat32.partition_lba = boot_lba;
    g_fat32.total_sectors = read_le32(&sector[32]);
    g_fat32.reserved_sector_count = read_le16(&sector[14]);
    g_fat32.num_fats = sector[16];
    g_fat32.sectors_per_cluster = sector[13];
    g_fat32.fat_size_sectors = read_le32(&sector[36]);
    g_fat32.root_cluster = read_le32(&sector[44]);
    g_fat32.fat_start_lba = g_fat32.partition_lba + g_fat32.reserved_sector_count;
    g_fat32.data_start_lba = g_fat32.partition_lba + g_fat32.reserved_sector_count + (g_fat32.num_fats * g_fat32.fat_size_sectors);
    g_fat32.total_clusters = (g_fat32.total_sectors - (g_fat32.data_start_lba - g_fat32.partition_lba)) / g_fat32.sectors_per_cluster;
    g_fat32.mounted = 1;
    if (!g_fat32.current_path[0]) fat32_reset_cwd();

    if (g_fat32.total_clusters < FAT32_MIN_CLUSTERS) {
        g_fat32.mounted = 0;
        return 0;
    }

    return 1;
}

static uint32_t fat32_read_fat_entry(uint32_t cluster) {
    uint8_t sector[512];
    uint32_t fat_offset = cluster * 4;
    uint32_t sector_lba = g_fat32.fat_start_lba + (fat_offset / 512);
    uint16_t offset = (uint16_t)(fat_offset % 512);

    if (!ata_read_sector(sector_lba, sector)) {
        g_disk_io_error = 1;
        return 0;
    }
    return read_le32(&sector[offset]) & 0x0FFFFFFF;
}

static void fat32_write_fat_entry(uint32_t cluster, uint32_t value) {
    uint8_t sector[512];
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_value = value & 0x0FFFFFFF;

    for (uint8_t fat = 0; fat < g_fat32.num_fats; fat++) {
        uint32_t sector_lba = g_fat32.fat_start_lba + (fat * g_fat32.fat_size_sectors) + (fat_offset / 512);
        uint16_t offset = (uint16_t)(fat_offset % 512);
        if (!ata_read_sector(sector_lba, sector)) {
            g_disk_io_error = 1;
            return;
        }
        fat_value |= read_le32(&sector[offset]) & 0xF0000000;
        write_le32(&sector[offset], fat_value);
        if (!ata_write_sector(sector_lba, sector)) {
            g_disk_io_error = 1;
            return;
        }
        fat_value = value & 0x0FFFFFFF;
    }
}

static void fat32_format_name_for_output(const uint8_t short_name[11], char *out) {
    int pos = 0;
    int has_ext = 0;

    for (int i = 0; i < 8 && short_name[i] != ' '; i++) out[pos++] = (char)short_name[i];
    for (int i = 8; i < 11; i++) {
        if (short_name[i] != ' ') {
            has_ext = 1;
            break;
        }
    }

    if (has_ext) {
        out[pos++] = '.';
        for (int i = 8; i < 11 && short_name[i] != ' '; i++) out[pos++] = (char)short_name[i];
    }

    out[pos] = '\0';
}

static uint8_t fat32_lfn_checksum(const uint8_t short_name[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + short_name[i];
    }
    return sum;
}

static int fat32_is_lfn_char_acceptable(char c) {
    return c >= 32 && c <= 126 && c != '/' && c != '\\';
}

static void fat32_append_char(char *out, int *len, char c, int maxlen) {
    if (*len < maxlen - 1) {
        out[*len] = c;
        (*len)++;
        out[*len] = '\0';
    }
}

static void fat32_decode_lfn_entry(const fat32_lfn_entry_t *lfn, char *out) {
    int len = 0;
    uint16_t chars[13];

    out[0] = '\0';
    for (int i = 0; i < 5; i++) chars[i] = lfn->name1[i];
    for (int i = 0; i < 6; i++) chars[5 + i] = lfn->name2[i];
    for (int i = 0; i < 2; i++) chars[11 + i] = lfn->name3[i];

    for (int i = 0; i < 13; i++) {
        uint16_t ch = chars[i];
        uint32_t codepoint = ch;
        char utf8[4];
        int written;

        if (ch == 0x0000 || ch == 0xFFFF) return;
        if (ch >= 0xD800 && ch <= 0xDBFF) {
            if (i + 1 >= 13) return;
            if (chars[i + 1] < 0xDC00 || chars[i + 1] > 0xDFFF) return;
            codepoint = 0x10000 + (((uint32_t)(ch - 0xD800) << 10) | (uint32_t)(chars[i + 1] - 0xDC00));
            i++;
        } else if (ch >= 0xDC00 && ch <= 0xDFFF) {
            return;
        }

        if (codepoint <= 0x7F && !fat32_is_lfn_char_acceptable((char)codepoint)) return;
        written = fat32_utf8_encode_char(codepoint, utf8, sizeof(utf8));
        if (written <= 0) return;
        for (int j = 0; j < written; j++) fat32_append_char(out, &len, utf8[j], 40);
    }
}

static void fat32_copy_string(char *dst, const char *src, int maxlen) {
    int i = 0;
    while (src[i] && i < maxlen - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int fat32_lfn_utf16_length(const char *name) {
    uint16_t utf16[128];
    return fat32_utf16_from_utf8(name, utf16, 128);
}

static int fat32_name_needs_lfn(const char *name) {
    uint8_t short_name[11];
    if (!fat32_build_short_name(name, short_name)) return 1;
    char short_output[32];
    fat32_format_name_for_output(short_name, short_output);
    return !fat32_ascii_equal(name, short_output);
}

static int fat32_to_upper_char(char c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}

static int fat32_is_valid_name_char(char c) {
    c = (char)fat32_to_upper_char(c);
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    if (c == '_' || c == '-' || c == '$' || c == '~') return 1;
    return 0;
}

static int fat32_build_short_name(const char *name, uint8_t out[11]) {
    int dot_seen = 0;
    int base_len = 0;
    int ext_len = 0;

    kmemset(out, ' ', 11);
    if (!name || !name[0]) return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;

    for (int i = 0; name[i]; i++) {
        char c = name[i];
        if (c == '.') {
            if (dot_seen) return 0;
            dot_seen = 1;
            continue;
        }
        if (c == '/' || c == '\\' || c == ' ') return 0;
        if (!fat32_is_valid_name_char(c)) return 0;

        if (!dot_seen) {
            if (base_len >= 8) return 0;
            out[base_len++] = (uint8_t)fat32_to_upper_char(c);
        } else {
            if (ext_len >= 3) return 0;
            out[8 + ext_len++] = (uint8_t)fat32_to_upper_char(c);
        }
    }

    return base_len > 0;
}

static int fat32_next_path_component(const char *path, int *index, char *component) {
    int pos = 0;

    while (path[*index] == '/') (*index)++;
    if (!path[*index]) return 0;

    while (path[*index] && path[*index] != '/') {
        if (pos >= 95) return -1;
        component[pos++] = path[*index];
        (*index)++;
    }
    component[pos] = '\0';
    return 1;
}

static int fat32_normalize_path(const char *path, char *out) {
    char stack[16][96];
    int depth = 0;
    int index = 0;
    int status;
    char component[96];
    const char *base = path;

    if (!path || !path[0]) {
        fat32_path_copy(out, g_fat32.current_path);
        return 1;
    }

    if (path[0] != '/') {
        base = g_fat32.current_path;
        while ((status = fat32_next_path_component(base, &index, component)) > 0) {
            if (depth >= 16) return 0;
            fat32_path_copy(stack[depth++], component);
        }
        index = 0;
        base = path;
    }

    while ((status = fat32_next_path_component(base, &index, component)) > 0) {
        if (strcmp(component, ".") == 0) continue;
        if (strcmp(component, "..") == 0) {
            if (depth > 0) depth--;
            continue;
        }
        if (depth >= 16) return 0;
        fat32_path_copy(stack[depth++], component);
    }

    if (status == -1) return 0;

    if (depth == 0) {
        out[0] = '/';
        out[1] = '\0';
        return 1;
    }

    int pos = 0;
    out[pos++] = '/';
    for (int i = 0; i < depth; i++) {
        for (int j = 0; stack[i][j] && pos < 127; j++) out[pos++] = stack[i][j];
        if (i != depth - 1 && pos < 127) out[pos++] = '/';
    }
    out[pos] = '\0';
    return 1;
}

static int fat32_name_matches_entry(const char *name, const fat32_lookup_result_t *result) {
    if (result->has_long_name && fat32_ascii_equal(name, result->display_name)) return 1;

    {
        char short_name[32];
        fat32_format_name_for_output(result->entry.name, short_name);
        return fat32_ascii_equal(name, short_name);
    }
}

static void fat32_lookup_set_short_name(fat32_lookup_result_t *result) {
    fat32_format_name_for_output(result->entry.name, result->display_name);
    result->has_long_name = 0;
}

static int fat32_find_in_directory(uint32_t dir_cluster, const char *name, fat32_lookup_result_t *out_result) {
    uint8_t sector[512];
    uint32_t cluster = dir_cluster;
    char lfn_parts[20][40];
    fat32_dir_slot_t lfn_slots[20];
    int lfn_count = 0;
    uint8_t lfn_checksum = 0;
    int lfn_valid = 0;

    while (cluster >= 2 && !is_fat32_eoc(cluster)) {
        uint32_t cluster_lba = fat32_cluster_to_lba(cluster);
        for (uint8_t sec = 0; sec < g_fat32.sectors_per_cluster; sec++) {
            if (!ata_read_sector(cluster_lba + sec, sector)) {
                g_disk_io_error = 1;
                return 0;
            }
            for (int offset = 0; offset < 512; offset += 32) {
                fat32_dir_entry_t *entry = (fat32_dir_entry_t*)&sector[offset];
                if (entry->name[0] == 0x00) return 0;
                if (entry->name[0] == 0xE5) {
                    lfn_count = 0;
                    lfn_valid = 0;
                    continue;
                }
                if (entry->attr == FAT32_ATTR_LFN) {
                    fat32_lfn_entry_t *lfn = (fat32_lfn_entry_t*)entry;
                    int seq = lfn->order & 0x1F;
                    if (seq >= 1 && seq <= 20) {
                        fat32_decode_lfn_entry(lfn, lfn_parts[seq - 1]);
                        lfn_slots[seq - 1].sector_lba = cluster_lba + sec;
                        lfn_slots[seq - 1].offset = (uint16_t)offset;
                        if (lfn->order & 0x40) {
                            lfn_count = seq;
                            lfn_checksum = lfn->checksum;
                            lfn_valid = 1;
                        }
                    }
                    continue;
                }

                fat32_lookup_result_t current;
                kmemset(&current, 0, sizeof(current));
                kmemcpy(&current.entry, entry, sizeof(*entry));
                current.slot.sector_lba = cluster_lba + sec;
                current.slot.offset = (uint16_t)offset;

                if (lfn_valid && lfn_count > 0 && lfn_checksum == fat32_lfn_checksum(entry->name)) {
                    int pos = 0;
                    current.has_long_name = 1;
                    current.lfn_count = lfn_count;
                    for (int i = 0; i < lfn_count; i++) current.lfn_slots[i] = lfn_slots[i];
                    current.display_name[0] = '\0';
                    for (int i = 0; i < lfn_count; i++) {
                        for (int j = 0; lfn_parts[i][j] && pos < 127; j++) {
                            current.display_name[pos++] = lfn_parts[i][j];
                        }
                    }
                    current.display_name[pos] = '\0';
                } else {
                    fat32_lookup_set_short_name(&current);
                }

                if (fat32_name_matches_entry(name, &current)) {
                    if (out_result) kmemcpy(out_result, &current, sizeof(current));
                    return 1;
                }

                lfn_count = 0;
                lfn_valid = 0;
            }
        }
        cluster = fat32_read_fat_entry(cluster);
    }

    return 0;
}

static int fat32_write_dir_entry_at(const fat32_dir_slot_t *slot, const fat32_dir_entry_t *entry) {
    uint8_t sector[512];
    if (!ata_read_sector(slot->sector_lba, sector)) {
        g_disk_io_error = 1;
        return 0;
    }
    kmemcpy(&sector[slot->offset], entry, sizeof(*entry));
    if (!ata_write_sector(slot->sector_lba, sector)) {
        g_disk_io_error = 1;
        return 0;
    }
    return 1;
}

static uint32_t fat32_find_free_cluster(void) {
    for (uint32_t cluster = 2; cluster < g_fat32.total_clusters + 2; cluster++) {
        if (fat32_read_fat_entry(cluster) == 0) return cluster;
    }
    return 0;
}

static uint32_t fat32_allocate_cluster_chain(uint32_t count) {
    uint32_t first = 0;
    uint32_t prev = 0;
    uint8_t zero_cluster[4096];
    uint32_t cluster_bytes = (uint32_t)g_fat32.sectors_per_cluster * 512U;

    if (cluster_bytes > sizeof(zero_cluster)) return 0;
    kmemset(zero_cluster, 0, cluster_bytes);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t cluster = fat32_find_free_cluster();
        if (cluster == 0) {
            if (first) {
                uint32_t curr = first;
                while (curr >= 2 && !is_fat32_eoc(curr)) {
                    uint32_t next = fat32_read_fat_entry(curr);
                    fat32_write_fat_entry(curr, 0);
                    if (is_fat32_eoc(next)) break;
                    curr = next;
                }
            }
            return 0;
        }

        fat32_write_fat_entry(cluster, FAT32_EOC);
        fat32_write_cluster(cluster, zero_cluster);

        if (!first) first = cluster;
        if (prev) fat32_write_fat_entry(prev, cluster);
        prev = cluster;
    }

    return first;
}

static void fat32_free_cluster_chain(uint32_t first_cluster) {
    uint32_t cluster = first_cluster;
    while (cluster >= 2 && !is_fat32_eoc(cluster)) {
        uint32_t next = fat32_read_fat_entry(cluster);
        fat32_write_fat_entry(cluster, 0);
        if (is_fat32_eoc(next)) break;
        cluster = next;
    }

    if (cluster >= 2 && cluster < FAT32_EOC) fat32_write_fat_entry(cluster, 0);
}

static int fat32_find_free_dir_slots(uint32_t dir_cluster, int needed, fat32_dir_slot_t *out_slots) {
    uint8_t sector[512];
    uint32_t cluster = dir_cluster;
    int found = 0;

    while (1) {
        uint32_t cluster_lba = fat32_cluster_to_lba(cluster);
        for (uint8_t sec = 0; sec < g_fat32.sectors_per_cluster; sec++) {
            if (!ata_read_sector(cluster_lba + sec, sector)) {
                g_disk_io_error = 1;
                return 0;
            }
            for (int offset = 0; offset < 512; offset += 32) {
                fat32_dir_entry_t *entry = (fat32_dir_entry_t*)&sector[offset];
                if (entry->name[0] == 0x00 || entry->name[0] == 0xE5) {
                    out_slots[found].sector_lba = cluster_lba + sec;
                    out_slots[found].offset = (uint16_t)offset;
                    found++;
                    if (found >= needed) return 1;
                } else {
                    found = 0;
                }
            }
            found = 0;
        }

        {
            uint32_t next = fat32_read_fat_entry(cluster);
            if (is_fat32_eoc(next)) {
                uint32_t extra = fat32_allocate_cluster_chain(1);
                if (!extra) return 0;
                fat32_write_fat_entry(cluster, extra);
                for (int i = 0; i < needed; i++) {
                    out_slots[i].sector_lba = fat32_cluster_to_lba(extra) + (uint32_t)((i * 32) / 512);
                    out_slots[i].offset = (uint16_t)((i * 32) % 512);
                }
                return 1;
            }
            cluster = next;
        }
    }
}

static int fat32_resolve_path(const char *path, uint32_t *out_dir_cluster, fat32_lookup_result_t *out_result) {
    int index = 0;
    int component_status;
    char component[96];
    uint32_t current_cluster = g_fat32.root_cluster;
    fat32_lookup_result_t current_result;

    if (!path || !path[0] || strcmp(path, "/") == 0) {
        if (out_dir_cluster) *out_dir_cluster = g_fat32.root_cluster;
        if (out_result) kmemset(out_result, 0, sizeof(*out_result));
        return 1;
    }

    while ((component_status = fat32_next_path_component(path, &index, component)) > 0) {
        if (!fat32_find_in_directory(current_cluster, component, &current_result)) return 0;

        while (path[index] == '/') index++;
        if (path[index]) {
            if (!(current_result.entry.attr & FAT32_ATTR_DIRECTORY)) return 0;
            current_cluster = fat32_dir_first_cluster(&current_result.entry);
            if (current_cluster < 2) return 0;
        } else {
            if (out_result) kmemcpy(out_result, &current_result, sizeof(*out_result));
            if (out_dir_cluster) *out_dir_cluster = current_cluster;
            return 1;
        }
    }

    return component_status != -1;
}

static int fat32_resolve_parent(const char *path, uint32_t *out_parent_cluster, char *out_name) {
    int index = 0;
    int component_status;
    char component[96];
    char last_component[96];
    uint32_t current_cluster = g_fat32.root_cluster;
    int have_last = 0;

    if (!path || !path[0] || strcmp(path, "/") == 0) return 0;

    while ((component_status = fat32_next_path_component(path, &index, component)) > 0) {
        while (path[index] == '/') index++;
        if (!path[index]) {
            strcpy(last_component, component);
            have_last = 1;
            break;
        }

        fat32_lookup_result_t result;
        if (!fat32_find_in_directory(current_cluster, component, &result)) return 0;
        if (!(result.entry.attr & FAT32_ATTR_DIRECTORY)) return 0;
        current_cluster = fat32_dir_first_cluster(&result.entry);
        if (current_cluster < 2) return 0;
    }

    if (!have_last || component_status == -1) return 0;
    fat32_copy_string(out_name, last_component, 96);
    *out_parent_cluster = current_cluster;
    return 1;
}

static int fat32_directory_is_empty(uint32_t cluster) {
    uint8_t sector[512];
    while (cluster >= 2 && !is_fat32_eoc(cluster)) {
        uint32_t cluster_lba = fat32_cluster_to_lba(cluster);
        for (uint8_t sec = 0; sec < g_fat32.sectors_per_cluster; sec++) {
            if (!ata_read_sector(cluster_lba + sec, sector)) {
                g_disk_io_error = 1;
                return 1;
            }
            for (int offset = 0; offset < 512; offset += 32) {
                fat32_dir_entry_t *entry = (fat32_dir_entry_t*)&sector[offset];
                if (entry->name[0] == 0x00) return 1;
                if (entry->name[0] == 0xE5 || entry->attr == FAT32_ATTR_LFN) continue;
                if (entry->name[0] == '.' && entry->name[1] == ' ') continue;
                if (entry->name[0] == '.' && entry->name[1] == '.') continue;
                return 0;
            }
        }
        cluster = fat32_read_fat_entry(cluster);
    }
    return 1;
}

static int fat32_write_file_data(uint32_t first_cluster, const char *text, uint32_t size) {
    uint8_t cluster_buffer[4096];
    uint32_t cluster_bytes = (uint32_t)g_fat32.sectors_per_cluster * 512U;
    uint32_t cluster = first_cluster;
    uint32_t copied = 0;

    if (cluster_bytes > sizeof(cluster_buffer)) return 0;
    if (size == 0) return 1;

    while (cluster >= 2) {
        kmemset(cluster_buffer, 0, cluster_bytes);
        uint32_t to_copy = size - copied;
        if (to_copy > cluster_bytes) to_copy = cluster_bytes;
        for (uint32_t i = 0; i < to_copy; i++) cluster_buffer[i] = (uint8_t)text[copied + i];
        fat32_write_cluster(cluster, cluster_buffer);
        copied += to_copy;
        if (copied >= size) return 1;

        cluster = fat32_read_fat_entry(cluster);
        if (is_fat32_eoc(cluster)) break;
    }

    return copied >= size;
}

static void fat32_init_subdir_cluster(uint32_t cluster, uint32_t parent_cluster) {
    uint8_t cluster_buffer[4096];
    fat32_dir_entry_t *dot;
    fat32_dir_entry_t *dotdot;
    uint32_t cluster_bytes = (uint32_t)g_fat32.sectors_per_cluster * 512U;

    if (cluster_bytes > sizeof(cluster_buffer)) return;
    kmemset(cluster_buffer, 0, cluster_bytes);

    dot = (fat32_dir_entry_t*)cluster_buffer;
    kmemset(dot, 0, sizeof(*dot));
    kmemset(dot->name, ' ', 11);
    dot->name[0] = '.';
    dot->attr = FAT32_ATTR_DIRECTORY;
    fat32_set_dir_first_cluster(dot, cluster);

    dotdot = (fat32_dir_entry_t*)(cluster_buffer + 32);
    kmemset(dotdot, 0, sizeof(*dotdot));
    kmemset(dotdot->name, ' ', 11);
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dotdot->attr = FAT32_ATTR_DIRECTORY;
    fat32_set_dir_first_cluster(dotdot, parent_cluster);

    fat32_write_cluster(cluster, cluster_buffer);
}

static void fat32_print_entry_name(const fat32_dir_entry_t *entry) {
    char name[14];
    fat32_format_name_for_output(entry->name, name);
    puts(name);
    if (entry->attr & FAT32_ATTR_DIRECTORY) putchar('/');
}

static void fat32_print_lookup_name(const fat32_lookup_result_t *result) {
    puts(result->display_name);
    if (result->entry.attr & FAT32_ATTR_DIRECTORY) putchar('/');
}

static int fat32_name_has_short_conflict(uint32_t dir_cluster, const uint8_t short_name[11]) {
    uint8_t sector[512];
    uint32_t cluster = dir_cluster;

    while (cluster >= 2 && !is_fat32_eoc(cluster)) {
        uint32_t cluster_lba = fat32_cluster_to_lba(cluster);
        for (uint8_t sec = 0; sec < g_fat32.sectors_per_cluster; sec++) {
            if (!ata_read_sector(cluster_lba + sec, sector)) {
                g_disk_io_error = 1;
                return 1;
            }
            for (int offset = 0; offset < 512; offset += 32) {
                fat32_dir_entry_t *entry = (fat32_dir_entry_t*)&sector[offset];
                int same = 1;
                if (entry->name[0] == 0x00) return 0;
                if (entry->name[0] == 0xE5 || entry->attr == FAT32_ATTR_LFN) continue;
                for (int i = 0; i < 11; i++) {
                    if (entry->name[i] != short_name[i]) {
                        same = 0;
                        break;
                    }
                }
                if (same) return 1;
            }
        }
        cluster = fat32_read_fat_entry(cluster);
    }
    return 0;
}

static int fat32_build_short_alias(uint32_t dir_cluster, const char *name, uint8_t out[11]) {
    if (fat32_build_short_name(name, out) && !fat32_name_has_short_conflict(dir_cluster, out)) return 1;

    char base[9];
    char ext[4];
    int base_len = 0;
    int ext_len = 0;
    int dot_pos = -1;

    for (int i = 0; name[i]; i++) {
        if (name[i] == '.') dot_pos = i;
    }

    kmemset(base, 0, sizeof(base));
    kmemset(ext, 0, sizeof(ext));

    for (int i = 0; name[i] && (dot_pos < 0 || i < dot_pos); i++) {
        if (fat32_is_valid_name_char(name[i]) && base_len < 6) base[base_len++] = (char)fat32_to_upper_char(name[i]);
    }
    if (base_len == 0) base[base_len++] = 'F';

    if (dot_pos >= 0) {
        for (int i = dot_pos + 1; name[i] && ext_len < 3; i++) {
            if (fat32_is_valid_name_char(name[i])) ext[ext_len++] = (char)fat32_to_upper_char(name[i]);
        }
    }

    for (int n = 1; n < 100000; n++) {
        int suffix_digits = 0;
        int tmp = n;
        char suffix[6];

        while (tmp > 0 && suffix_digits < 5) {
            suffix[suffix_digits++] = (char)('0' + (tmp % 10));
            tmp /= 10;
        }

        kmemset(out, ' ', 11);
        {
            int max_base = 8 - 1 - suffix_digits;
            int used_base = base_len < max_base ? base_len : max_base;
            if (used_base < 1) used_base = 1;
            for (int i = 0; i < used_base; i++) out[i] = (uint8_t)base[i];
            out[used_base] = '~';
            for (int i = 0; i < suffix_digits; i++) out[used_base + 1 + i] = (uint8_t)suffix[suffix_digits - 1 - i];
        }
        for (int i = 0; i < ext_len; i++) out[8 + i] = (uint8_t)ext[i];
        if (!fat32_name_has_short_conflict(dir_cluster, out)) return 1;
    }

    return 0;
}

static int fat32_write_entry_chain(const fat32_dir_slot_t *slots, int count, const char *long_name, fat32_dir_entry_t *entry) {
    uint8_t short_checksum = fat32_lfn_checksum(entry->name);
    int lfn_count = count - 1;
    uint16_t utf16_name[128];
    int long_len = fat32_utf16_from_utf8(long_name, utf16_name, 128);

    if (long_len < 0) return 0;

    for (int i = 0; i < lfn_count; i++) {
        fat32_lfn_entry_t lfn;
        int seq = lfn_count - i;
        int name_offset = (seq - 1) * 13;
        uint16_t namebuf[13];

        kmemset(&lfn, 0xFF, sizeof(lfn));
        kmemset(namebuf, 0xFF, sizeof(namebuf));
        lfn.order = (uint8_t)seq;
        if (i == 0) lfn.order |= 0x40;
        lfn.attr = FAT32_ATTR_LFN;
        lfn.type = 0;
        lfn.checksum = short_checksum;
        lfn.first_cluster_lo = 0;

        for (int j = 0; j < 13; j++) {
            if (name_offset + j >= long_len) {
                namebuf[j] = 0x0000;
                for (int k = j + 1; k < 13; k++) namebuf[k] = 0xFFFF;
                break;
            }
            namebuf[j] = utf16_name[name_offset + j];
        }

        if (name_offset >= long_len) namebuf[0] = 0x0000;

        for (int j = 0; j < 5; j++) lfn.name1[j] = namebuf[j];
        for (int j = 0; j < 6; j++) lfn.name2[j] = namebuf[5 + j];
        for (int j = 0; j < 2; j++) lfn.name3[j] = namebuf[11 + j];

        if (!fat32_write_dir_entry_at(&slots[i], (fat32_dir_entry_t*)&lfn)) return 0;
    }

    return fat32_write_dir_entry_at(&slots[count - 1], entry);
}

int disk_get_device_count(void) {
    disk_probe_devices();
    return g_disk_device_count;
}

int disk_get_active_device(void) {
    if (!disk_current_device()) return -1;
    return g_disk_active_index;
}

int disk_select_device(int index) {
    disk_probe_devices();
    if (index < 0 || index >= g_disk_device_count || g_disk_devices[index].type == DISK_BACKEND_NONE) return 0;
    g_disk_active_index = index;
    return 1;
}

void cmd_disk_devices(void) {
    disk_probe_devices();
    for (int i = 0; i < g_disk_device_count; i++) {
        uint32_t size_mb = g_disk_devices[i].total_sectors / 2048U;

        puts(i == g_disk_active_index ? "* " : "  ");
        puts("disk");
        putchar('0' + i);
        puts(" (");
        puts(g_disk_devices[i].label);
        puts(") ");
        print_uint(size_mb);
        if (!g_disk_devices[i].writable) puts(" MiB ro\n");
        else puts(" MiB rw\n");
    }

    if (!g_disk_device_count) puts("disk devices: no disks detected\n");
}

static int disk_require_active_device(const char *context) {
    if (disk_current_device()) return 1;
    puts(context);
    puts(": no disks detected\n");
    return 0;
}

static int disk_current_is_writable(void) {
    disk_device_t *device = disk_current_device();
    return device && device->writable;
}

void cmd_disk_format(void) {
    disk_device_t *device = disk_current_device();
    uint32_t total_sectors = device ? device->total_sectors : 0;
    uint32_t partition_lba = 2048;
    uint8_t sector[512];
    uint8_t fsinfo[512];
    uint8_t mbr[512];
    uint32_t sectors_per_cluster;
    uint16_t reserved_sector_count = 32;
    uint8_t num_fats = 2;
    uint32_t fat_size;
    uint32_t cluster_count;
    uint32_t data_sectors;
    uint32_t volume_sectors;

    g_disk_io_error = 0;

    if (!disk_require_active_device("disk format")) return;
    if (!device || !device->writable) {
        puts("disk format: available only for writable disks\n");
        return;
    }

    if (total_sectors == 0) {
        puts("disk format: unable to identify disk\n");
        return;
    }

    if (total_sectors <= partition_lba + 65536U) {
        puts("disk format: disk is too small for MBR + FAT32\n");
        return;
    }

    volume_sectors = total_sectors - partition_lba;

    if (volume_sectors >= 16U * 1024U * 1024U) sectors_per_cluster = 8;
    else if (volume_sectors >= 8U * 1024U * 1024U) sectors_per_cluster = 4;
    else if (volume_sectors >= 512U * 1024U) sectors_per_cluster = 2;
    else sectors_per_cluster = 1;

    fat_size = 1;
    while (1) {
        data_sectors = volume_sectors - reserved_sector_count - (num_fats * fat_size);
        cluster_count = data_sectors / sectors_per_cluster;
        fat_size = ((cluster_count + 2) * 4 + 511) / 512;
        data_sectors = volume_sectors - reserved_sector_count - (num_fats * fat_size);
        cluster_count = data_sectors / sectors_per_cluster;
        {
            uint32_t new_fat_size = ((cluster_count + 2) * 4 + 511) / 512;
            if (new_fat_size == fat_size) break;
            fat_size = new_fat_size;
        }
    }

    if (cluster_count < FAT32_MIN_CLUSTERS || cluster_count > FAT32_MAX_CLUSTERS) {
        puts("disk format: disk geometry is not suitable for FAT32\n");
        return;
    }

    if (device->type == DISK_BACKEND_USB) {
        puts("Waiting for USB device to be ready...\n");
        for (int retry = 0; retry < 10; retry++) {
            if (usb_storage_test_ready(device->backend_index)) break;
            usb_delay();
            if (retry == 9) {
                puts("disk format: USB device not ready (timeout)\n");
                return;
            }
        }
    }

    puts("Formatting disk");
    putchar('0' + g_disk_active_index);
    puts(" (");
    puts(device->label);
    puts(")...\n");

    puts("Clearing MBR and FAT area...\n");
    ata_write_zero_sectors(0, 1);
    ata_write_zero_sectors(partition_lba, reserved_sector_count);
    // Initial zeroing of FAT area - we do this once.
    ata_write_zero_sectors(partition_lba + reserved_sector_count, num_fats * fat_size);
    if (g_disk_io_error) {
        puts("disk format: disk write failed while clearing metadata area (I/O error)\n");
        return;
    }


    kmemset(mbr, 0, sizeof(mbr));
    mbr[446 + 4] = 0xEF; // EFI System Partition (instead of 0x0C)
    write_le32(&mbr[446 + 8], partition_lba);
    write_le32(&mbr[446 + 12], volume_sectors);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    kmemset(sector, 0, sizeof(sector));
    sector[0] = 0xEB;
    sector[1] = 0x58;
    sector[2] = 0x90;
    kmemcpy(&sector[3], "MLJOS5.0", 8);
    write_le16(&sector[11], 512);
    sector[13] = (uint8_t)sectors_per_cluster;
    write_le16(&sector[14], reserved_sector_count);
    sector[16] = num_fats;
    write_le16(&sector[17], 0);
    write_le16(&sector[19], 0);
    sector[21] = 0xF8;
    write_le16(&sector[22], 0);
    write_le16(&sector[24], 63);
    write_le16(&sector[26], 255);
    write_le32(&sector[28], 0);
    write_le32(&sector[32], volume_sectors);
    write_le32(&sector[36], fat_size);
    write_le16(&sector[40], 0);
    write_le16(&sector[42], 0);
    write_le32(&sector[44], 2);
    write_le16(&sector[48], 1);
    write_le16(&sector[50], 6);
    sector[64] = 0x80;
    sector[66] = 0x29;
    write_le32(&sector[67], 0x4D4C4A53);
    kmemcpy(&sector[71], "MLJOS FAT32", 11);
    kmemcpy(&sector[82], "FAT32   ", 8);
    sector[510] = 0x55;
    sector[511] = 0xAA;

    kmemset(fsinfo, 0, sizeof(fsinfo));
    write_le32(&fsinfo[0], 0x41615252);
    write_le32(&fsinfo[484], 0x61417272);
    write_le32(&fsinfo[488], 0xFFFFFFFF);
    write_le32(&fsinfo[492], 3);
    fsinfo[510] = 0x55;
    fsinfo[511] = 0xAA;

    if (!ata_write_sector(0, mbr)
        || !ata_write_sector(partition_lba, sector)
        || !ata_write_sector(partition_lba + 1, fsinfo)
        || !ata_write_sector(partition_lba + 6, sector)
        || !ata_write_sector(partition_lba + 7, fsinfo)) {
        puts("disk format: failed to write FAT32 boot sectors\n");
        return;
    }

    kmemset(sector, 0, sizeof(sector));
    write_le32(&sector[0], 0x0FFFFFF8);
    write_le32(&sector[4], 0xFFFFFFFF);
    write_le32(&sector[8], FAT32_EOC);
    if (!ata_write_sector(partition_lba + reserved_sector_count, sector)
        || !ata_write_sector(partition_lba + reserved_sector_count + fat_size, sector)) {
        puts("disk format: failed to initialize FAT tables\n");
        return;
    }

    // We already zeroed the rest of the FAT area above, so we don't need to do it again here.
    // This redundant zeroing was making USB formatting very slow.
    // Just zero the cluster following the FAT area (root directory first cluster)
    ata_write_zero_sectors(partition_lba + reserved_sector_count + (num_fats * fat_size), sectors_per_cluster);
    if (g_disk_io_error) {
        puts("disk format: disk write failed while zeroing data region (I/O error)\n");
        return;
    }


    g_fat32.mounted = 0;
    fat32_reset_cwd();
    if (!fat32_mount()) {
        puts("disk format: FAT32 mount failed after format\n");
        return;
    }

    puts("Disk formatted as FAT32.\n");
}

void cmd_disk_ls(const char *path) {
    uint32_t dir_cluster = g_fat32.root_cluster;
    fat32_lookup_result_t target;
    fat32_lookup_result_t item;
    uint8_t sector[512];
    uint32_t cluster;
    char resolved_path[128];
    char lfn_parts[20][40];
    fat32_dir_slot_t lfn_slots[20];
    int lfn_count = 0;
    uint8_t lfn_checksum = 0;
    int lfn_valid = 0;

    g_disk_io_error = 0;
    if (!disk_require_active_device("disk ls")) return;
    if (!fat32_mount()) {
        puts("Disk not formatted as FAT32.\n");
        return;
    }

    if (!fat32_normalize_path(path, resolved_path)) {
        puts("disk ls: invalid path\n");
        return;
    }

    if (strcmp(resolved_path, "/") != 0) {
        if (!fat32_resolve_path(resolved_path, &dir_cluster, &target)) {
            if (!path || !path[0]) {
                fat32_reset_cwd();
                fat32_path_copy(resolved_path, "/");
                dir_cluster = g_fat32.root_cluster;
            } else {
                puts("disk ls: path not found\n");
                return;
            }
        }
        if (strcmp(resolved_path, "/") != 0 && !(target.entry.attr & FAT32_ATTR_DIRECTORY)) {
            fat32_print_lookup_name(&target);
            putchar('\n');
            return;
        }
        if (strcmp(resolved_path, "/") != 0) {
            dir_cluster = fat32_dir_first_cluster(&target.entry);
        }
    }

    cluster = dir_cluster;
    while (cluster >= 2 && !is_fat32_eoc(cluster)) {
        uint32_t cluster_lba = fat32_cluster_to_lba(cluster);
        for (uint8_t sec = 0; sec < g_fat32.sectors_per_cluster; sec++) {
            if (!ata_read_sector(cluster_lba + sec, sector)) {
                puts("disk ls: disk read error\n");
                return;
            }
            for (int offset = 0; offset < 512; offset += 32) {
                fat32_dir_entry_t *dir_entry = (fat32_dir_entry_t*)&sector[offset];
                if (dir_entry->name[0] == 0x00) {
                    putchar('\n');
                    return;
                }
                if (dir_entry->name[0] == 0xE5) {
                    lfn_count = 0;
                    lfn_valid = 0;
                    continue;
                }
                if (dir_entry->attr == FAT32_ATTR_LFN) {
                    fat32_lfn_entry_t *lfn = (fat32_lfn_entry_t*)dir_entry;
                    int seq = lfn->order & 0x1F;
                    if (seq >= 1 && seq <= 20) {
                        fat32_decode_lfn_entry(lfn, lfn_parts[seq - 1]);
                        lfn_slots[seq - 1].sector_lba = cluster_lba + sec;
                        lfn_slots[seq - 1].offset = (uint16_t)offset;
                        if (lfn->order & 0x40) {
                            lfn_count = seq;
                            lfn_checksum = lfn->checksum;
                            lfn_valid = 1;
                        }
                    }
                    continue;
                }

                kmemset(&item, 0, sizeof(item));
                kmemcpy(&item.entry, dir_entry, sizeof(*dir_entry));
                if (lfn_valid && lfn_count > 0 && lfn_checksum == fat32_lfn_checksum(dir_entry->name)) {
                    int pos = 0;
                    item.has_long_name = 1;
                    for (int i = 0; i < lfn_count; i++) {
                        for (int j = 0; lfn_parts[i][j] && pos < 127; j++) item.display_name[pos++] = lfn_parts[i][j];
                    }
                    item.display_name[pos] = '\0';
                } else {
                    fat32_lookup_set_short_name(&item);
                }
                if (item.display_name[0] == '.' && (item.display_name[1] == '\0' || (item.display_name[1] == '.' && item.display_name[2] == '\0'))) {
                    lfn_count = 0;
                    lfn_valid = 0;
                    continue;
                }
                fat32_print_lookup_name(&item);
                puts("  ");
                lfn_count = 0;
                lfn_valid = 0;
            }
        }
        cluster = fat32_read_fat_entry(cluster);
    }

    putchar('\n');
}

int disk_list_dir_file_names(const char *path, char *out, int out_size) {
    uint32_t dir_cluster = g_fat32.root_cluster;
    fat32_lookup_result_t target;
    fat32_lookup_result_t item;
    uint8_t sector[512];
    uint32_t cluster;
    char resolved_path[128];
    char lfn_parts[20][40];
    int lfn_count = 0;
    uint8_t lfn_checksum = 0;
    int lfn_valid = 0;
    int pos = 0;

    out[0] = '\0';
    if (!out_size || !out) return 0;
    if (!path) path = "/";

    g_disk_io_error = 0;
    if (!disk_require_active_device("disk ls")) return 0;
    if (!fat32_mount()) return 0;

    if (!fat32_normalize_path(path, resolved_path)) return 0;

    if (strcmp(resolved_path, "/") != 0) {
        if (!fat32_resolve_path(resolved_path, &dir_cluster, &target)) return 0;
        if (!(target.entry.attr & FAT32_ATTR_DIRECTORY)) return 0;
        dir_cluster = fat32_dir_first_cluster(&target.entry);
    }

    cluster = dir_cluster;
    while (cluster >= 2 && !is_fat32_eoc(cluster)) {
        uint32_t cluster_lba = fat32_cluster_to_lba(cluster);
        for (uint8_t sec = 0; sec < g_fat32.sectors_per_cluster; sec++) {
            if (!ata_read_sector(cluster_lba + sec, sector)) {
                g_disk_io_error = 1;
                return 0;
            }
            for (int offset = 0; offset < 512; offset += 32) {
                fat32_dir_entry_t *dir_entry = (fat32_dir_entry_t *)&sector[offset];
                if (dir_entry->name[0] == 0x00) {
                    out[pos] = '\0';
                    return 1;
                }
                if (dir_entry->name[0] == 0xE5) {
                    lfn_count = 0;
                    lfn_valid = 0;
                    continue;
                }
                if (dir_entry->attr == FAT32_ATTR_LFN) {
                    fat32_lfn_entry_t *lfn = (fat32_lfn_entry_t *)dir_entry;
                    int seq = lfn->order & 0x1F;
                    if (seq >= 1 && seq <= 20) {
                        fat32_decode_lfn_entry(lfn, lfn_parts[seq - 1]);
                        if (lfn->order & 0x40) {
                            lfn_count = seq;
                            lfn_checksum = lfn->checksum;
                            lfn_valid = 1;
                        }
                    }
                    continue;
                }

                kmemset(&item, 0, sizeof(item));
                kmemcpy(&item.entry, dir_entry, sizeof(*dir_entry));

                if (lfn_valid && lfn_count > 0 && lfn_checksum == fat32_lfn_checksum(dir_entry->name)) {
                    int p = 0;
                    item.has_long_name = 1;
                    for (int i = 0; i < lfn_count; i++) {
                        for (int j = 0; lfn_parts[i][j] && p < 127; j++) item.display_name[p++] = lfn_parts[i][j];
                    }
                    item.display_name[p] = '\0';
                } else {
                    fat32_lookup_set_short_name(&item);
                }

                lfn_count = 0;
                lfn_valid = 0;

                if (item.display_name[0] == '.' &&
                    (item.display_name[1] == '\0' ||
                     (item.display_name[1] == '.' && item.display_name[2] == '\0'))) {
                    continue;
                }

                if (!item.display_name[0]) continue;

                for (int i = 0; item.display_name[i] && pos < out_size - 1; i++) out[pos++] = item.display_name[i];
                if (pos < out_size - 1) out[pos++] = '\0';
                if (pos >= out_size - 1) {
                    out[pos] = '\0';
                    return 1;
                }
            }
        }
        cluster = fat32_read_fat_entry(cluster);
        if (g_disk_io_error) return 0;
    }

    out[pos] = '\0';
    return 1;
}

void disk_prepare_session(void) {
    (void)disk_current_device();
    fat32_reset_cwd();
    g_disk_io_error = 0;
    g_fat32.mounted = 0;
    (void)fat32_mount();
}

void cmd_disk_cd(const char *path) {
    char resolved_path[128];
    fat32_lookup_result_t entry;

    g_disk_io_error = 0;
    if (!disk_require_active_device("disk cd")) return;
    if (!fat32_mount()) {
        puts("Disk not formatted as FAT32.\n");
        return;
    }

    if (!fat32_normalize_path(path, resolved_path)) {
        puts("disk cd: invalid path\n");
        return;
    }

    if (strcmp(resolved_path, "/") == 0) {
        fat32_reset_cwd();
        return;
    }

    if (!fat32_resolve_path(resolved_path, NULL, &entry)) {
        puts("disk cd: directory not found\n");
        return;
    }

    if (!(entry.entry.attr & FAT32_ATTR_DIRECTORY)) {
        puts("disk cd: target is not a directory\n");
        return;
    }

    fat32_path_copy(g_fat32.current_path, resolved_path);
}

void cmd_disk_pwd(void) {
    g_disk_io_error = 0;
    if (!disk_require_active_device("disk pwd")) return;
    if (!fat32_mount()) {
        puts("Disk not formatted as FAT32.\n");
        return;
    }
    puts(g_fat32.current_path[0] ? g_fat32.current_path : "/");
    putchar('\n');
}

void cmd_disk_mkdir(const char *path) {
    uint32_t parent_cluster;
    char leaf_name[96];
    uint8_t short_name[11];
    fat32_lookup_result_t existing;
    fat32_dir_entry_t new_entry;
    fat32_dir_slot_t slots[21];
    uint32_t new_cluster;
    char resolved_path[128];
    int entry_count;

    g_disk_io_error = 0;
    if (!disk_require_active_device("disk mkdir")) return;
    if (!disk_current_is_writable()) {
        puts("disk mkdir: device is read-only\n");
        return;
    }
    if (!fat32_mount()) {
        puts("Disk not formatted as FAT32.\n");
        return;
    }

    if (!fat32_normalize_path(path, resolved_path) || !fat32_resolve_parent(resolved_path, &parent_cluster, leaf_name)) {
        puts("disk mkdir: invalid path\n");
        return;
    }

    if (fat32_find_in_directory(parent_cluster, leaf_name, &existing)) {
        puts("disk mkdir: already exists\n");
        return;
    }

    if (!fat32_build_short_alias(parent_cluster, leaf_name, short_name)) {
        puts("disk mkdir: could not build short alias\n");
        return;
    }

    new_cluster = fat32_allocate_cluster_chain(1);
    if (!new_cluster) {
        puts("disk mkdir: out of clusters\n");
        return;
    }

    fat32_init_subdir_cluster(new_cluster, parent_cluster);

    {
        int needs_lfn = fat32_name_needs_lfn(leaf_name);
        int lfn_len = needs_lfn ? fat32_lfn_utf16_length(leaf_name) : 0;

        if (needs_lfn && lfn_len < 0) {
            fat32_free_cluster_chain(new_cluster);
            puts("disk mkdir: invalid UTF-8 filename\n");
            return;
        }
        entry_count = needs_lfn ? ((lfn_len + 12) / 13) + 1 : 1;
    }
    if (!fat32_find_free_dir_slots(parent_cluster, entry_count, slots)) {
        fat32_free_cluster_chain(new_cluster);
        puts("disk mkdir: directory is full\n");
        return;
    }

    kmemset(&new_entry, 0, sizeof(new_entry));
    kmemcpy(new_entry.name, short_name, 11);
    new_entry.attr = FAT32_ATTR_DIRECTORY;
    fat32_set_dir_first_cluster(&new_entry, new_cluster);
    if (!fat32_write_entry_chain(slots, entry_count, leaf_name, &new_entry) || g_disk_io_error) {
        fat32_free_cluster_chain(new_cluster);
        puts("disk mkdir: failed to write directory entry\n");
    }
}

void cmd_disk_write(const char *path, const char *text) {
    uint32_t parent_cluster;
    char leaf_name[96];
    uint8_t short_name[11];
    fat32_lookup_result_t existing;
    fat32_dir_entry_t entry;
    fat32_dir_slot_t slots[21];
    uint32_t file_size;
    uint32_t cluster_bytes;
    uint32_t clusters_needed;
    uint32_t first_cluster = 0;
    int exists = 0;
    char resolved_path[128];
    int entry_count;

    g_disk_io_error = 0;
    if (!disk_require_active_device("disk write")) return;
    if (!disk_current_is_writable()) {
        puts("disk write: device is read-only\n");
        return;
    }
    if (!fat32_mount()) {
        puts("Disk not formatted as FAT32.\n");
        return;
    }

    if (!fat32_normalize_path(path, resolved_path) || !fat32_resolve_parent(resolved_path, &parent_cluster, leaf_name)) {
        puts("disk write: invalid path\n");
        return;
    }

    exists = fat32_find_in_directory(parent_cluster, leaf_name, &existing);
    if (exists && (existing.entry.attr & FAT32_ATTR_DIRECTORY)) {
        puts("disk write: target is a directory\n");
        return;
    }

    if (!exists && !fat32_build_short_alias(parent_cluster, leaf_name, short_name)) {
        puts("disk write: could not build short alias\n");
        return;
    }

    file_size = text ? strlen(text) : 0;
    cluster_bytes = (uint32_t)g_fat32.sectors_per_cluster * 512U;
    clusters_needed = (file_size + cluster_bytes - 1) / cluster_bytes;

    if (clusters_needed > 0) {
        first_cluster = fat32_allocate_cluster_chain(clusters_needed);
        if (!first_cluster) {
            puts("disk write: out of clusters\n");
            return;
        }
        if (!fat32_write_file_data(first_cluster, text, file_size)) {
            fat32_free_cluster_chain(first_cluster);
            puts("disk write: failed to write data\n");
            return;
        }
    }

    if (!exists) {
        {
            int needs_lfn = fat32_name_needs_lfn(leaf_name);
            int lfn_len = needs_lfn ? fat32_lfn_utf16_length(leaf_name) : 0;

            if (needs_lfn && lfn_len < 0) {
                if (first_cluster) fat32_free_cluster_chain(first_cluster);
                puts("disk write: invalid UTF-8 filename\n");
                return;
            }
            entry_count = needs_lfn ? ((lfn_len + 12) / 13) + 1 : 1;
        }
        if (!fat32_find_free_dir_slots(parent_cluster, entry_count, slots)) {
            if (first_cluster) fat32_free_cluster_chain(first_cluster);
            puts("disk write: parent directory is full\n");
            return;
        }
        kmemset(&entry, 0, sizeof(entry));
        kmemcpy(entry.name, short_name, 11);
        entry.attr = 0x20;
    } else {
        entry = existing.entry;
        entry_count = existing.lfn_count + 1;
        for (int i = 0; i < existing.lfn_count; i++) slots[i] = existing.lfn_slots[i];
        slots[entry_count - 1] = existing.slot;
        if (fat32_name_needs_lfn(leaf_name) && !existing.has_long_name) {
            puts("disk write: renaming 8.3 entries to LFN is not supported yet\n");
            if (first_cluster >= 2) fat32_free_cluster_chain(first_cluster);
            return;
        }
        uint32_t old_cluster = fat32_dir_first_cluster(&entry);
        if (old_cluster >= 2) fat32_free_cluster_chain(old_cluster);
    }

    fat32_set_dir_first_cluster(&entry, first_cluster);
    entry.file_size = file_size;
    if (!fat32_write_entry_chain(slots, entry_count, exists ? existing.display_name : leaf_name, &entry) || g_disk_io_error) {
        if (!exists && first_cluster >= 2) fat32_free_cluster_chain(first_cluster);
        puts("disk write: failed to update directory entry\n");
    }
}

static int fat32_ensure_directory_quiet(const char *path) {
    uint32_t parent_cluster;
    char leaf_name[96];
    uint8_t short_name[11];
    fat32_lookup_result_t existing;
    fat32_dir_entry_t new_entry;
    fat32_dir_slot_t slots[21];
    uint32_t new_cluster;
    char resolved_path[128];
    int entry_count;

    if (!fat32_normalize_path(path, resolved_path)) return 0;
    if (strcmp(resolved_path, "/") == 0) return 1;

    if (fat32_resolve_path(resolved_path, NULL, &existing)) {
        return (existing.entry.attr & FAT32_ATTR_DIRECTORY) != 0;
    }

    if (!fat32_resolve_parent(resolved_path, &parent_cluster, leaf_name)) return 0;
    if (!fat32_build_short_alias(parent_cluster, leaf_name, short_name)) return 0;

    new_cluster = fat32_allocate_cluster_chain(1);
    if (!new_cluster) return 0;

    fat32_init_subdir_cluster(new_cluster, parent_cluster);
    {
        int needs_lfn = fat32_name_needs_lfn(leaf_name);
        int lfn_len = needs_lfn ? fat32_lfn_utf16_length(leaf_name) : 0;

        if (needs_lfn && lfn_len < 0) {
            fat32_free_cluster_chain(new_cluster);
            return 0;
        }
        entry_count = needs_lfn ? ((lfn_len + 12) / 13) + 1 : 1;
    }
    if (!fat32_find_free_dir_slots(parent_cluster, entry_count, slots)) {
        fat32_free_cluster_chain(new_cluster);
        return 0;
    }

    kmemset(&new_entry, 0, sizeof(new_entry));
    kmemcpy(new_entry.name, short_name, 11);
    new_entry.attr = FAT32_ATTR_DIRECTORY;
    fat32_set_dir_first_cluster(&new_entry, new_cluster);
    if (!fat32_write_entry_chain(slots, entry_count, leaf_name, &new_entry) || g_disk_io_error) {
        fat32_free_cluster_chain(new_cluster);
        return 0;
    }

    return 1;
}

static int disk_write_file_internal(const char *path, const char *data, uint32_t file_size) {
    uint32_t parent_cluster;
    char leaf_name[96];
    uint8_t short_name[11];
    fat32_lookup_result_t existing;
    fat32_dir_entry_t entry;
    fat32_dir_slot_t slots[21];
    uint32_t cluster_bytes;
    uint32_t clusters_needed;
    uint32_t first_cluster = 0;
    int exists = 0;
    char resolved_path[128];
    int entry_count;

    if (!fat32_normalize_path(path, resolved_path) || !fat32_resolve_parent(resolved_path, &parent_cluster, leaf_name)) return 0;

    exists = fat32_find_in_directory(parent_cluster, leaf_name, &existing);
    if (exists && (existing.entry.attr & FAT32_ATTR_DIRECTORY)) return 0;
    if (!exists && !fat32_build_short_alias(parent_cluster, leaf_name, short_name)) return 0;

    cluster_bytes = (uint32_t)g_fat32.sectors_per_cluster * 512U;
    clusters_needed = (file_size + cluster_bytes - 1) / cluster_bytes;

    if (clusters_needed > 0) {
        first_cluster = fat32_allocate_cluster_chain(clusters_needed);
        if (!first_cluster) return 0;
        if (!fat32_write_file_data(first_cluster, data, file_size)) {
            fat32_free_cluster_chain(first_cluster);
            return 0;
        }
    }

    if (!exists) {
        {
            int needs_lfn = fat32_name_needs_lfn(leaf_name);
            int lfn_len = needs_lfn ? fat32_lfn_utf16_length(leaf_name) : 0;

            if (needs_lfn && lfn_len < 0) {
                if (first_cluster) fat32_free_cluster_chain(first_cluster);
                return 0;
            }
            entry_count = needs_lfn ? ((lfn_len + 12) / 13) + 1 : 1;
        }
        if (!fat32_find_free_dir_slots(parent_cluster, entry_count, slots)) {
            if (first_cluster) fat32_free_cluster_chain(first_cluster);
            return 0;
        }
        kmemset(&entry, 0, sizeof(entry));
        kmemcpy(entry.name, short_name, 11);
        entry.attr = 0x20;
    } else {
        entry = existing.entry;
        entry_count = existing.lfn_count + 1;
        for (int i = 0; i < existing.lfn_count; i++) slots[i] = existing.lfn_slots[i];
        slots[entry_count - 1] = existing.slot;
        if (fat32_name_needs_lfn(leaf_name) && !existing.has_long_name) {
            if (first_cluster >= 2) fat32_free_cluster_chain(first_cluster);
            return 0;
        }
        {
            uint32_t old_cluster = fat32_dir_first_cluster(&entry);
            if (old_cluster >= 2) fat32_free_cluster_chain(old_cluster);
        }
    }

    fat32_set_dir_first_cluster(&entry, first_cluster);
    entry.file_size = file_size;
    if (!fat32_write_entry_chain(slots, entry_count, exists ? existing.display_name : leaf_name, &entry) || g_disk_io_error) {
        if (!exists && first_cluster >= 2) fat32_free_cluster_chain(first_cluster);
        return 0;
    }

    return 1;
}

static int disk_read_text_file_internal(const char *path, char *out, int maxlen) {
    fat32_lookup_result_t entry;
    uint32_t file_cluster;
    uint32_t remaining;
    uint8_t cluster_buffer[4096];
    uint32_t cluster_bytes;
    char resolved_path[128];
    int offset = 0;

    if (!fat32_normalize_path(path, resolved_path) || strcmp(resolved_path, "/") == 0) return 0;
    if (!fat32_resolve_path(resolved_path, NULL, &entry)) return 0;
    if (entry.entry.attr & FAT32_ATTR_DIRECTORY) return 0;

    file_cluster = fat32_dir_first_cluster(&entry.entry);
    remaining = entry.entry.file_size;
    cluster_bytes = (uint32_t)g_fat32.sectors_per_cluster * 512U;
    if (cluster_bytes > sizeof(cluster_buffer)) return 0;
    if ((int)remaining >= maxlen) return 0;

    while (remaining > 0 && file_cluster >= 2) {
        fat32_read_cluster(file_cluster, cluster_buffer);
        if (g_disk_io_error) return 0;

        {
            uint32_t chunk = remaining > cluster_bytes ? cluster_bytes : remaining;
            for (uint32_t i = 0; i < chunk && offset < maxlen - 1; i++) out[offset++] = (char)cluster_buffer[i];
            remaining -= chunk;
        }

        if (remaining == 0) break;
        file_cluster = fat32_read_fat_entry(file_cluster);
        if (is_fat32_eoc(file_cluster)) break;
    }

    out[offset] = '\0';
    return 1;
}

int disk_read_file(const char *path, char *out, int maxlen, uint32_t *size_out) {
    fat32_lookup_result_t entry;
    uint32_t file_cluster;
    uint32_t remaining;
    uint8_t cluster_buffer[4096];
    uint32_t cluster_bytes;
    char resolved_path[128];
    int offset = 0;

    g_disk_io_error = 0;
    if (!fat32_mount()) return 0;
    if (!fat32_normalize_path(path, resolved_path) || strcmp(resolved_path, "/") == 0) return 0;
    if (!fat32_resolve_path(resolved_path, NULL, &entry)) return 0;
    if (entry.entry.attr & FAT32_ATTR_DIRECTORY) return 0;

    file_cluster = fat32_dir_first_cluster(&entry.entry);
    remaining = entry.entry.file_size;
    cluster_bytes = (uint32_t)g_fat32.sectors_per_cluster * 512U;
    if (cluster_bytes > sizeof(cluster_buffer)) return 0;
    if ((int)remaining > maxlen) return 0;

    while (remaining > 0 && file_cluster >= 2) {
        fat32_read_cluster(file_cluster, cluster_buffer);
        if (g_disk_io_error) return 0;

        {
            uint32_t chunk = remaining > cluster_bytes ? cluster_bytes : remaining;
            for (uint32_t i = 0; i < chunk && offset < maxlen; i++) out[offset++] = (char)cluster_buffer[i];
            remaining -= chunk;
        }

        if (remaining == 0) break;
        file_cluster = fat32_read_fat_entry(file_cluster);
        if (is_fat32_eoc(file_cluster)) break;
    }

    if (size_out) *size_out = entry.entry.file_size;
    return 1;
}

int disk_load_user_config(char *out, int maxlen) {
    g_disk_io_error = 0;
    if (!fat32_mount()) return 0;
    return disk_read_text_file_internal("/system/users.cfg", out, maxlen);
}

int disk_save_user_config(const char *text) {
    g_disk_io_error = 0;
    if (!disk_current_is_writable()) return 0;
    if (!fat32_mount()) return 0;
    if (!fat32_ensure_directory_quiet("/system")) return 0;
    return disk_write_file_internal("/system/users.cfg", text ? text : "", text ? strlen(text) : 0);
}

int disk_ensure_directory(const char *path) {
    g_disk_io_error = 0;
    if (!disk_current_is_writable()) return 0;
    if (!fat32_mount()) return 0;
    return fat32_ensure_directory_quiet(path);
}

int disk_write_file(const char *path, const char *data, uint32_t size) {
    g_disk_io_error = 0;
    if (!disk_current_is_writable()) return 0;
    if (!fat32_mount()) return 0;
    return disk_write_file_internal(path, data ? data : "", size);
}

int disk_touch_file(const char *path) {
    return disk_write_file(path, "", 0);
}

int disk_copy_file(const char *src_path, const char *dst_path) {
    char buffer[8192];
    uint32_t size = 0;

    if (!disk_read_file(src_path, buffer, sizeof(buffer), &size)) return 0;
    return disk_write_file(dst_path, buffer, size);
}

void cmd_disk_cat(const char *path) {
    fat32_lookup_result_t entry;
    uint32_t file_cluster;
    uint32_t remaining;
    uint8_t cluster_buffer[4096];
    uint32_t cluster_bytes;
    char resolved_path[128];

    g_disk_io_error = 0;
    if (!disk_require_active_device("disk cat")) return;
    if (!fat32_mount()) {
        puts("Disk not formatted as FAT32.\n");
        return;
    }

    if (!fat32_normalize_path(path, resolved_path) || strcmp(resolved_path, "/") == 0) {
        puts("disk cat: path must point to a file\n");
        return;
    }

    if (!fat32_resolve_path(resolved_path, NULL, &entry)) {
        puts("disk cat: file not found\n");
        return;
    }

    if (entry.entry.attr & FAT32_ATTR_DIRECTORY) {
        puts("disk cat: target is a directory\n");
        return;
    }

    file_cluster = fat32_dir_first_cluster(&entry.entry);
    remaining = entry.entry.file_size;
    cluster_bytes = (uint32_t)g_fat32.sectors_per_cluster * 512U;
    if (cluster_bytes > sizeof(cluster_buffer)) {
        puts("disk cat: cluster size is unsupported\n");
        return;
    }

    while (remaining > 0 && file_cluster >= 2) {
        fat32_read_cluster(file_cluster, cluster_buffer);
        if (g_disk_io_error) {
            puts("disk cat: disk read error\n");
            return;
        }
        uint32_t chunk = remaining > cluster_bytes ? cluster_bytes : remaining;
        for (uint32_t i = 0; i < chunk; i++) putchar((char)cluster_buffer[i]);
        remaining -= chunk;
        if (remaining == 0) break;
        file_cluster = fat32_read_fat_entry(file_cluster);
        if (is_fat32_eoc(file_cluster)) break;
    }

    putchar('\n');
}

void cmd_disk_rm(const char *path) {
    fat32_lookup_result_t entry;
    uint8_t sector[512];
    uint32_t first_cluster;
    char resolved_path[128];

    g_disk_io_error = 0;
    if (!disk_require_active_device("disk rm")) return;
    if (!disk_current_is_writable()) {
        puts("disk rm: device is read-only\n");
        return;
    }
    if (!fat32_mount()) {
        puts("Disk not formatted as FAT32.\n");
        return;
    }

    if (!fat32_normalize_path(path, resolved_path) || strcmp(resolved_path, "/") == 0) {
        puts("disk rm: refusing to remove FAT32 root\n");
        return;
    }

    if (!fat32_resolve_path(resolved_path, NULL, &entry)) {
        puts("disk rm: path not found\n");
        return;
    }

    first_cluster = fat32_dir_first_cluster(&entry.entry);
    if ((entry.entry.attr & FAT32_ATTR_DIRECTORY) && first_cluster >= 2 && !fat32_directory_is_empty(first_cluster)) {
        puts("disk rm: directory is not empty\n");
        return;
    }

    if (first_cluster >= 2) fat32_free_cluster_chain(first_cluster);

    for (int i = 0; i < entry.lfn_count; i++) {
        if (!ata_read_sector(entry.lfn_slots[i].sector_lba, sector)) {
            puts("disk rm: disk read error\n");
            return;
        }
        sector[entry.lfn_slots[i].offset] = 0xE5;
        if (!ata_write_sector(entry.lfn_slots[i].sector_lba, sector)) {
            puts("disk rm: disk write error\n");
            return;
        }
    }
    if (!ata_read_sector(entry.slot.sector_lba, sector)) {
        puts("disk rm: ATA read error\n");
        return;
    }
    sector[entry.slot.offset] = 0xE5;
    if (!ata_write_sector(entry.slot.sector_lba, sector)) {
        puts("disk rm: ATA write error\n");
    }
}

const char *disk_get_cwd_path(void) {
    return g_fat32.current_path[0] ? g_fat32.current_path : "/";
}

void cmd_disk_install(void) {
    uintptr_t kernel_start = (uintptr_t)_kernel_start;
    uint32_t kernel_size = (uint32_t)((uintptr_t)_kernel_end - kernel_start);
    uint32_t num_sectors = (kernel_size + 511) / 512;
    uint8_t sector0[512];

    if (!disk_require_active_device("disk install")) return;
    if (!disk_current_is_writable()) {
        puts("disk install: available only for writable disks\n");
        return;
    }

    puts("Installing OS to disk...");
    putchar('\n');

    puts("Preparing installation files...\n");
    // BIOS/Legacy: Write kernel to sector 1+
    // UEFI: Files will be synced from RAM FS to FAT32

    uint8_t *kmem = (uint8_t *)kernel_start;
    for (uint32_t i = 0; i < num_sectors; i++) {
        if (!ata_write_sector(1 + i, kmem + (i * 512))) {
            puts("Failed to write kernel data to disk\n");
            return;
        }
    }

    puts("Install complete! You can now boot directly from this hard disk.\n");
}

void cmd_disk_exec(const char *path) {
    fat32_lookup_result_t entry;
    uint32_t file_cluster;
    uint32_t remaining;
    uint8_t cluster_buffer[4096];
    uint32_t cluster_bytes;
    char resolved_path[128];

    g_disk_io_error = 0;
    if (!disk_require_active_device("disk exec")) return;
    if (!fat32_mount()) {
        puts("Disk not formatted as FAT32.\n");
        return;
    }

    if (!fat32_normalize_path(path, resolved_path) || strcmp(resolved_path, "/") == 0) {
        puts("exec: path must point to a file\n");
        return;
    }

    if (!fat32_resolve_path(resolved_path, NULL, &entry)) {
        puts("exec: file not found\n");
        return;
    }

    if (entry.entry.attr & FAT32_ATTR_DIRECTORY) {
        puts("exec: target is a directory\n");
        return;
    }

    file_cluster = fat32_dir_first_cluster(&entry.entry);
    remaining = entry.entry.file_size;
    cluster_bytes = (uint32_t)g_fat32.sectors_per_cluster * 512U;
    
    if (remaining == 0) {
        puts("exec: file is empty\n");
        return;
    }

    char *app_start = (char *)0x800000;
    uint32_t offset = 0;

    while (remaining > 0 && file_cluster >= 2) {
        fat32_read_cluster(file_cluster, cluster_buffer);
        if (g_disk_io_error) {
            puts("exec: disk read error\n");
            return;
        }
        uint32_t chunk = remaining > cluster_bytes ? cluster_bytes : remaining;
        for (uint32_t i = 0; i < chunk; i++) {
            app_start[offset++] = (char)cluster_buffer[i];
        }
        remaining -= chunk;
        if (remaining == 0) break;
        file_cluster = fat32_read_fat_entry(file_cluster);
        if (is_fat32_eoc(file_cluster)) break;
    }

    if (disk_is_elf_image(app_start, offset)) {
        puts("exec: unsupported ELF app format\n");
        return;
    }

    app_entry_t app = (app_entry_t)app_start;
    app(&os_api);
}

int disk_can_exec_path(const char *path) {
    fat32_lookup_result_t entry;
    char resolved_path[128];

    g_disk_io_error = 0;
    if (!fat32_mount()) return 0;
    if (!fat32_normalize_path(path, resolved_path) || strcmp(resolved_path, "/") == 0) return 0;
    if (!fat32_resolve_path(resolved_path, NULL, &entry)) return 0;
    if (entry.entry.attr & FAT32_ATTR_DIRECTORY) return 0;
    return entry.entry.file_size > 0;
}

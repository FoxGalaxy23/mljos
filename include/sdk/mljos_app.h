#ifndef MLJOS_APP_H
#define MLJOS_APP_H

#include "common.h"

// .app header:
// - Placed at MLJOS_APP_VADDR (start of the image)
// - Reserves the first 4KiB page
// - App entrypoint is at MLJOS_APP_VADDR + entry_offset (default 0x1000)

#define MLJOS_APP_MAGIC_0 'M'
#define MLJOS_APP_MAGIC_1 'L'
#define MLJOS_APP_MAGIC_2 'J'
#define MLJOS_APP_MAGIC_3 'A'
#define MLJOS_APP_MAGIC_4 'P'
#define MLJOS_APP_MAGIC_5 'P'
#define MLJOS_APP_MAGIC_6 '\0'
#define MLJOS_APP_MAGIC_7 '\0'

#define MLJOS_APP_HEADER_VERSION 1

// App capability flags (bitmask)
#define MLJOS_APP_FLAG_TUI    (1u << 0)
#define MLJOS_APP_FLAG_GUI    (1u << 1)
#define MLJOS_APP_FLAG_HIDDEN (1u << 2)

typedef struct __attribute__((packed)) mljos_app_header_v1 {
    char magic[8];           // "MLJAPP\0\0"
    uint16_t version;        // 1
    uint16_t header_size;    // sizeof(mljos_app_header_v1)
    uint32_t entry_offset;   // default 0x1000
    uint32_t flags;          // MLJOS_APP_FLAG_*
    char title[64];          // NUL-terminated when possible
} mljos_app_header_v1_t;

static inline int mljos_app_header_v1_valid(const mljos_app_header_v1_t *h) {
    if (!h) return 0;
    if (h->magic[0] != MLJOS_APP_MAGIC_0) return 0;
    if (h->magic[1] != MLJOS_APP_MAGIC_1) return 0;
    if (h->magic[2] != MLJOS_APP_MAGIC_2) return 0;
    if (h->magic[3] != MLJOS_APP_MAGIC_3) return 0;
    if (h->magic[4] != MLJOS_APP_MAGIC_4) return 0;
    if (h->magic[5] != MLJOS_APP_MAGIC_5) return 0;
    if (h->magic[6] != MLJOS_APP_MAGIC_6) return 0;
    if (h->magic[7] != MLJOS_APP_MAGIC_7) return 0;
    if (h->version != MLJOS_APP_HEADER_VERSION) return 0;
    if (h->header_size != (uint16_t)sizeof(mljos_app_header_v1_t)) return 0;
    return 1;
}

static inline uint32_t mljos_app_entry_offset_from_image(const void *image, uint32_t image_size) {
    (void)image_size;
    const mljos_app_header_v1_t *h = (const mljos_app_header_v1_t *)image;
    if (!mljos_app_header_v1_valid(h)) return 0;
    // Minimal sanity: must be within first 2MiB mapping; and 4KiB-aligned default.
    if (h->entry_offset >= (2u * 1024u * 1024u)) return 0;
    return h->entry_offset;
}

static inline uint32_t mljos_app_flags_from_image(const void *image, uint32_t image_size) {
    (void)image_size;
    const mljos_app_header_v1_t *h = (const mljos_app_header_v1_t *)image;
    if (!mljos_app_header_v1_valid(h)) return MLJOS_APP_FLAG_TUI;
    return h->flags;
}

// Define the header in the app image. Linker script places this at 0x40000000 and
// pads to 4KiB so that .text starts at +0x1000.
#define MLJOS_APP_DEFINE(app_title, app_flags) \
    __attribute__((section(".mljos_hdr"), used)) \
    const mljos_app_header_v1_t g_mljos_app_header = { \
        .magic = { MLJOS_APP_MAGIC_0, MLJOS_APP_MAGIC_1, MLJOS_APP_MAGIC_2, MLJOS_APP_MAGIC_3, MLJOS_APP_MAGIC_4, MLJOS_APP_MAGIC_5, MLJOS_APP_MAGIC_6, MLJOS_APP_MAGIC_7 }, \
        .version = MLJOS_APP_HEADER_VERSION, \
        .header_size = (uint16_t)sizeof(mljos_app_header_v1_t), \
        .entry_offset = 0x1000u, \
        .flags = (uint32_t)(app_flags), \
        .title = app_title, \
    }

// Mark the app entrypoint so the linker script can place it first at +0x1000.
#define MLJOS_APP_ENTRY __attribute__((section(".text.mljos_entry"), used))

#endif

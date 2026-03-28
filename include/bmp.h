#ifndef BMP_H
#define BMP_H

#include "common.h"

// Decodes a BMP image into packed 0x00RRGGBB pixels.
// Supports uncompressed BMP (BI_RGB) with 24bpp, 32bpp, and 8bpp (paletted).
// Allocates the output pixel buffer with kmem_alloc(); caller owns the pointer
// for the lifetime of the kernel (no free API).
int bmp_decode_rgb32(const void *data, uint32_t size, uint32_t **out_px, int *out_w, int *out_h);

// Nearest-neighbor scaling of packed 0x00RRGGBB pixels.
// Allocates output with kmem_alloc().
int bmp_scale_nearest_rgb32(const uint32_t *src, int src_w, int src_h, uint32_t **out_px, int dst_w, int dst_h);

#endif


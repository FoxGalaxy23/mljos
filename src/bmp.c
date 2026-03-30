#include "bmp.h"

#include "kmem.h"

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t rd_s32(const uint8_t *p) {
    return (int32_t)rd_u32(p);
}

int bmp_scale_nearest_rgb32(const uint32_t *src, int src_w, int src_h, uint32_t **out_px, int dst_w, int dst_h) {
    if (!src || !out_px || dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) return 0;

    uint64_t bytes = (uint64_t)dst_w * (uint64_t)dst_h * 4ULL;
    uint32_t *dst = (uint32_t *)kmem_alloc(bytes, 16);
    if (!dst) return 0;

    for (int y = 0; y < dst_h; ++y) {
        int sy = (int)((uint64_t)y * (uint64_t)src_h / (uint64_t)dst_h);
        if (sy < 0) sy = 0;
        if (sy >= src_h) sy = src_h - 1;
        for (int x = 0; x < dst_w; ++x) {
            int sx = (int)((uint64_t)x * (uint64_t)src_w / (uint64_t)dst_w);
            if (sx < 0) sx = 0;
            if (sx >= src_w) sx = src_w - 1;
            dst[y * dst_w + x] = src[sy * src_w + sx];
        }
    }

    *out_px = dst;
    return 1;
}

int bmp_scale_bilinear_rgb32(const uint32_t *src, int src_w, int src_h, uint32_t **out_px, int dst_w, int dst_h) {
    if (!src || !out_px || dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) return 0;

    uint64_t bytes = (uint64_t)dst_w * (uint64_t)dst_h * 4ULL;
    uint32_t *dst = (uint32_t *)kmem_alloc(bytes, 16);
    if (!dst) return 0;

    for (int y = 0; y < dst_h; ++y) {
        float v = (dst_h == 1) ? 0.0f : (float)y * (float)(src_h - 1) / (float)(dst_h - 1);
        int y0 = (int)v;
        int y1 = (y0 + 1 < src_h) ? (y0 + 1) : y0;
        float fy = v - (float)y0;
        for (int x = 0; x < dst_w; ++x) {
            float u = (dst_w == 1) ? 0.0f : (float)x * (float)(src_w - 1) / (float)(dst_w - 1);
            int x0 = (int)u;
            int x1 = (x0 + 1 < src_w) ? (x0 + 1) : x0;
            float fx = u - (float)x0;

            uint32_t c00 = src[y0 * src_w + x0];
            uint32_t c10 = src[y0 * src_w + x1];
            uint32_t c01 = src[y1 * src_w + x0];
            uint32_t c11 = src[y1 * src_w + x1];

            float w00 = (1.0f - fx) * (1.0f - fy);
            float w10 = fx * (1.0f - fy);
            float w01 = (1.0f - fx) * fy;
            float w11 = fx * fy;

            float r = ((float)((c00 >> 16) & 0xFF) * w00) +
                      ((float)((c10 >> 16) & 0xFF) * w10) +
                      ((float)((c01 >> 16) & 0xFF) * w01) +
                      ((float)((c11 >> 16) & 0xFF) * w11);
            float g = ((float)((c00 >> 8) & 0xFF) * w00) +
                      ((float)((c10 >> 8) & 0xFF) * w10) +
                      ((float)((c01 >> 8) & 0xFF) * w01) +
                      ((float)((c11 >> 8) & 0xFF) * w11);
            float b = ((float)(c00 & 0xFF) * w00) +
                      ((float)(c10 & 0xFF) * w10) +
                      ((float)(c01 & 0xFF) * w01) +
                      ((float)(c11 & 0xFF) * w11);

            uint32_t rr = (uint32_t)(r + 0.5f);
            uint32_t gg = (uint32_t)(g + 0.5f);
            uint32_t bb = (uint32_t)(b + 0.5f);
            if (rr > 255u) rr = 255u;
            if (gg > 255u) gg = 255u;
            if (bb > 255u) bb = 255u;
            dst[y * dst_w + x] = (rr << 16) | (gg << 8) | bb;
        }
    }

    *out_px = dst;
    return 1;
}

int bmp_decode_rgb32(const void *data, uint32_t size, uint32_t **out_px, int *out_w, int *out_h) {
    if (!data || size < 54 || !out_px || !out_w || !out_h) return 0;
    const uint8_t *p = (const uint8_t *)data;

    // BITMAPFILEHEADER (14 bytes)
    if (p[0] != 'B' || p[1] != 'M') return 0;
    uint32_t bfOffBits = rd_u32(p + 10);
    if (bfOffBits >= size) return 0;

    // DIB header
    const uint8_t *dib = p + 14;
    if ((uint32_t)(dib - p) + 4 > size) return 0;
    uint32_t dibSize = rd_u32(dib + 0);
    if (dibSize < 40) return 0;
    if (14u + dibSize > size) return 0;

    int32_t width = rd_s32(dib + 4);
    int32_t height = rd_s32(dib + 8);
    uint16_t planes = rd_u16(dib + 12);
    uint16_t bpp = rd_u16(dib + 14);
    uint32_t compression = rd_u32(dib + 16);
    uint32_t clrUsed = rd_u32(dib + 32);

    if (planes != 1) return 0;
    if (width <= 0) return 0;
    if (height == 0) return 0;
    if (compression != 0) return 0; // BI_RGB only
    if (!(bpp == 24 || bpp == 32 || bpp == 8)) return 0;

    int top_down = 0;
    if (height < 0) { top_down = 1; height = -height; }
    if (height <= 0) return 0;

    // Palette (for 8bpp)
    const uint8_t *palette = NULL;
    uint32_t palette_entries = 0;
    if (bpp == 8) {
        uint32_t colors = clrUsed ? clrUsed : 256u;
        uint32_t palette_bytes = colors * 4u;
        uint32_t palette_off = 14u + dibSize;
        if (palette_off + palette_bytes > size) return 0;
        palette = p + palette_off;
        palette_entries = colors;
    }

    uint32_t row_bytes = 0;
    if (bpp == 24) {
        // ((bitsPerPixel * width + 31) / 32) * 4
        row_bytes = (((uint32_t)width * 24u + 31u) / 32u) * 4u;
    } else if (bpp == 32) {
        row_bytes = (uint32_t)width * 4u;
    } else { // 8bpp
        row_bytes = (((uint32_t)width * 8u + 31u) / 32u) * 4u;
    }

    uint64_t needed = (uint64_t)row_bytes * (uint64_t)height;
    if ((uint64_t)bfOffBits + needed > (uint64_t)size) return 0;

    uint64_t out_bytes = (uint64_t)width * (uint64_t)height * 4ULL;
    uint32_t *out = (uint32_t *)kmem_alloc(out_bytes, 16);
    if (!out) return 0;

    const uint8_t *pix = p + bfOffBits;
    for (int y = 0; y < height; ++y) {
        int sy = top_down ? y : (height - 1 - y);
        const uint8_t *row = pix + (uint64_t)sy * (uint64_t)row_bytes;
        uint32_t *dst = out + (uint64_t)y * (uint64_t)width;

        if (bpp == 24) {
            for (int x = 0; x < width; ++x) {
                uint8_t b = row[x * 3 + 0];
                uint8_t g = row[x * 3 + 1];
                uint8_t r = row[x * 3 + 2];
                dst[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        } else if (bpp == 32) {
            for (int x = 0; x < width; ++x) {
                uint8_t b = row[x * 4 + 0];
                uint8_t g = row[x * 4 + 1];
                uint8_t r = row[x * 4 + 2];
                dst[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        } else { // 8bpp paletted
            for (int x = 0; x < width; ++x) {
                uint8_t idx = row[x];
                if (idx >= palette_entries) { dst[x] = 0; continue; }
                const uint8_t *e = palette + (uint32_t)idx * 4u;
                uint8_t b = e[0];
                uint8_t g = e[1];
                uint8_t r = e[2];
                dst[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
    }

    *out_px = out;
    *out_w = (int)width;
    *out_h = (int)height;
    return 1;
}

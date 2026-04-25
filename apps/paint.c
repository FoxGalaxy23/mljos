#include "sdk/mljos_api.h"
#include "sdk/mljos_app.h"

MLJOS_APP_DEFINE("Paint", MLJOS_APP_FLAG_TUI | MLJOS_APP_FLAG_GUI);

#define CANVAS_W 256
#define CANVAS_H 256

static uint32_t g_canvas[CANVAS_W * CANVAS_H];
static uint32_t g_palette[] = {
    0xFFFFFF, 0x000000, 0xFF0000, 0x00FF00, 
    0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF,
    0xFFA500, 0x808080, 0xA52A2A, 0x800080
};
#define PALETTE_COUNT (sizeof(g_palette) / sizeof(g_palette[0]))

static uint32_t g_current_color = 0xFFFFFF;
static int g_brush_size = 3;
static int g_is_eraser = 0;
static int g_is_fill = 0;

static void my_memset(void *p, int v, unsigned int n) {
    unsigned char *u = (unsigned char *)p;
    while (n--) *u++ = (unsigned char)v;
}

static void my_memcpy(void *dst, const void *src, unsigned int n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
}

static int my_strlen(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void my_strcpy(char *dst, const char *src) {
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

static int my_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static void clear_canvas(uint32_t color) {
    for (int i = 0; i < CANVAS_W * CANVAS_H; i++) g_canvas[i] = color;
}

static void draw_point(int x, int y, uint32_t color, int size) {
    int r = size / 2;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int px = x + dx;
            int py = y + dy;
            if (px >= 0 && px < CANVAS_W && py >= 0 && py < CANVAS_H) {
                g_canvas[py * CANVAS_W + px] = color;
            }
        }
    }
}

// Draw a line between two points to avoid gaps when moving mouse quickly
static void draw_line(int x0, int y0, int x1, int y1, uint32_t color, int size) {
    int dx = x1 - x0; if (dx < 0) dx = -dx;
    int dy = y1 - y0; if (dy < 0) dy = -dy;
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (1) {
        draw_point(x0, y0, color, size);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

static void flood_fill(int x, int y, uint32_t target_color, uint32_t replacement_color) {
    if (target_color == replacement_color) return;
    if (x < 0 || x >= CANVAS_W || y < 0 || y >= CANVAS_H) return;
    if (g_canvas[y * CANVAS_W + x] != target_color) return;

    static int stack_x[CANVAS_W * CANVAS_H];
    static int stack_y[CANVAS_W * CANVAS_H];
    int sp = 0;

    stack_x[sp] = x;
    stack_y[sp] = y;
    sp++;

    while (sp > 0) {
        sp--;
        int cx = stack_x[sp];
        int cy = stack_y[sp];

        if (g_canvas[cy * CANVAS_W + cx] == target_color) {
            g_canvas[cy * CANVAS_W + cx] = replacement_color;

            if (cx + 1 < CANVAS_W && sp < CANVAS_W * CANVAS_H) { stack_x[sp] = cx + 1; stack_y[sp] = cy; sp++; }
            if (cx - 1 >= 0 && sp < CANVAS_W * CANVAS_H) { stack_x[sp] = cx - 1; stack_y[sp] = cy; sp++; }
            if (cy + 1 < CANVAS_H && sp < CANVAS_W * CANVAS_H) { stack_x[sp] = cx; stack_y[sp] = cy + 1; sp++; }
            if (cy - 1 >= 0 && sp < CANVAS_W * CANVAS_H) { stack_x[sp] = cx; stack_y[sp] = cy - 1; sp++; }
        }
    }
}

// BMP Encoding (24bpp)
static int save_bmp(mljos_api_t *api, const char *path) {
    unsigned int row_size = CANVAS_W * 3;
    // Row size must be multiple of 4. CANVAS_W=256, 256*3=768 (multiple of 4), so no padding.
    unsigned int pixel_data_size = row_size * CANVAS_H;
    unsigned int total_size = 54 + pixel_data_size;

    // Allocate on stack might be risky if stack is small, but 200KB is usually fine for user apps in mljOS
    // However, let's use a smaller buffer or write in chunks if possible.
    // Actually, mljos_api_t usually has a write_file which takes a buffer.
    // Let's use a static buffer for the file data to be safe.
    static uint8_t file_buf[200000];
    if (total_size > sizeof(file_buf)) return 0;

    my_memset(file_buf, 0, 54);
    file_buf[0] = 'B'; file_buf[1] = 'M';
    *(uint32_t*)(file_buf + 2) = total_size;
    *(uint32_t*)(file_buf + 10) = 54; // offset
    *(uint32_t*)(file_buf + 14) = 40; // DIB header size
    *(int32_t*)(file_buf + 18) = CANVAS_W;
    *(int32_t*)(file_buf + 22) = CANVAS_H;
    *(uint16_t*)(file_buf + 26) = 1; // planes
    *(uint16_t*)(file_buf + 28) = 24; // bpp
    *(uint32_t*)(file_buf + 34) = pixel_data_size;

    uint8_t *pix_ptr = file_buf + 54;
    for (int y = 0; y < CANVAS_H; y++) {
        // BMP is bottom-to-top
        uint32_t *src_row = g_canvas + (CANVAS_H - 1 - y) * CANVAS_W;
        uint8_t *dst_row = pix_ptr + y * row_size;
        for (int x = 0; x < CANVAS_W; x++) {
            uint32_t c = src_row[x];
            dst_row[x * 3 + 0] = (uint8_t)(c & 0xFF);         // B
            dst_row[x * 3 + 1] = (uint8_t)((c >> 8) & 0xFF);  // G
            dst_row[x * 3 + 2] = (uint8_t)((c >> 16) & 0xFF); // R
        }
    }

    return api->write_file(path, (const char *)file_buf, total_size);
}

// BMP Decoding (Simple 24/32bpp)
static int load_bmp(mljos_api_t *api, const char *path) {
    static uint8_t file_buf[300000];
    unsigned int size = 0;
    if (!api->read_file(path, (char *)file_buf, sizeof(file_buf), &size)) return 0;
    if (size < 54 || file_buf[0] != 'B' || file_buf[1] != 'M') return 0;

    uint32_t offset = *(uint32_t*)(file_buf + 10);
    int32_t w = *(int32_t*)(file_buf + 18);
    int32_t h = *(int32_t*)(file_buf + 22);
    uint16_t bpp = *(uint16_t*)(file_buf + 28);

    if (w <= 0 || h == 0) return 0;
    int abs_h = h < 0 ? -h : h;
    int top_down = h < 0;

    // Scale or crop? For now, we only support CANVAS_W x CANVAS_H exactly for simplicity
    // or we just center/crop it.
    clear_canvas(0x000000);

    int copy_w = w < CANVAS_W ? w : CANVAS_W;
    int copy_h = abs_h < CANVAS_H ? abs_h : CANVAS_H;

    uint8_t *pix_ptr = file_buf + offset;
    unsigned int row_size = ((w * bpp + 31) / 32) * 4;

    for (int y = 0; y < copy_h; y++) {
        int sy = top_down ? y : (abs_h - 1 - y);
        uint8_t *src_row = pix_ptr + sy * row_size;
        uint32_t *dst_row = g_canvas + y * CANVAS_W;

        for (int x = 0; x < copy_w; x++) {
            if (bpp == 24) {
                uint8_t b = src_row[x * 3 + 0];
                uint8_t g = src_row[x * 3 + 1];
                uint8_t r = src_row[x * 3 + 2];
                dst_row[x] = (r << 16) | (g << 8) | b;
            } else if (bpp == 32) {
                uint8_t b = src_row[x * 4 + 0];
                uint8_t g = src_row[x * 4 + 1];
                uint8_t r = src_row[x * 4 + 2];
                dst_row[x] = (r << 16) | (g << 8) | b;
            }
        }
    }
    return 1;
}

MLJOS_APP_ENTRY void _start(mljos_api_t *api) {
    if (!api || !(api->launch_flags & MLJOS_LAUNCH_GUI) || !api->ui) {
        if (api) api->puts("Paint requires GUI mode.\n");
        return;
    }

    api->ui->begin_app("Paint");
    clear_canvas(0x000000);

    int dirty = 1;
    int last_mx = -1, last_my = -1;
    int mouse_down = 0;

    for (;;) {
        int sw = (int)api->ui->screen_w();
        int sh = (int)api->ui->screen_h();

        int sidebar_w = 60;
        int bottom_bar_h = 60;
        int canvas_x = sidebar_w + (sw - sidebar_w - CANVAS_W) / 2;
        int canvas_y = (sh - bottom_bar_h - CANVAS_H) / 2;
        if (canvas_x < sidebar_w) canvas_x = sidebar_w;
        if (canvas_y < 0) canvas_y = 0;

        if (dirty) {
            // Clear background
            api->ui->fill_rect(0, 0, sw, sh, 0x1E1E1E);

            // Sidebar
            api->ui->fill_rect(0, 0, sidebar_w, sh, 0x252526);
            
            // Tool: Brush
            api->ui->fill_rect(10, 10, 40, 40, (!g_is_eraser) ? 0x007ACC : 0x333333);
            api->ui->fill_rect(25, 20, 10, 20, 0xFFFFFF); // Brush icon
            
            // Tool: Eraser
            api->ui->fill_rect(10, 60, 40, 40, (g_is_eraser) ? 0x007ACC : 0x333333);
            api->ui->fill_rect(15, 75, 30, 15, 0xFFFFFF); // Eraser icon
            
            // Tool: Fill
            api->ui->fill_rect(10, 110, 40, 40, (g_is_fill) ? 0x007ACC : 0x333333);
            api->ui->draw_text("FILL", 15, 122, 0xFFFFFF);
            
            // Clear Tool
            api->ui->fill_rect(10, 160, 40, 40, 0x333333);
            api->ui->draw_text("CLR", 18, 172, 0xFFFFFF);

            // Save Tool
            api->ui->fill_rect(10, 210, 40, 40, 0x0E639C);
            api->ui->draw_text("SAVE", 15, 222, 0xFFFFFF);

            // Load Tool
            api->ui->fill_rect(10, 260, 40, 40, 0x0E639C);
            api->ui->draw_text("LOAD", 15, 272, 0xFFFFFF);

            // Size Selection
            api->ui->draw_text("SIZE", 15, 270, 0xCCCCCC);
            api->ui->fill_rect(10, 290, 40, 30, (g_brush_size == 1) ? 0x555555 : 0x333333);
            api->ui->draw_text("1", 27, 298, 0xFFFFFF);
            api->ui->fill_rect(10, 325, 40, 30, (g_brush_size == 3) ? 0x555555 : 0x333333);
            api->ui->draw_text("3", 27, 333, 0xFFFFFF);
            api->ui->fill_rect(10, 360, 40, 30, (g_brush_size == 5) ? 0x555555 : 0x333333);
            api->ui->draw_text("5", 27, 368, 0xFFFFFF);

            // Bottom bar (Palette)
            api->ui->fill_rect(sidebar_w, sh - bottom_bar_h, sw - sidebar_w, bottom_bar_h, 0x252526);
            int pal_x = sidebar_w + 20;
            for (int i = 0; i < PALETTE_COUNT; i++) {
                api->ui->fill_rect(pal_x, sh - bottom_bar_h + 15, 30, 30, g_palette[i]);
                if (g_palette[i] == g_current_color && !g_is_eraser) {
                    api->ui->fill_rect(pal_x, sh - bottom_bar_h + 48, 30, 4, 0x007ACC);
                }
                pal_x += 40;
            }

            // Draw Canvas Border
            api->ui->fill_rect(canvas_x - 2, canvas_y - 2, CANVAS_W + 4, CANVAS_H + 4, 0x000000);
            
            // Rendering the canvas can be expensive if we do 1x1 rects.
            // But mljOS UI is fast enough for 256x256? 65536 calls might be slow.
            // Let's see. In mljOS, window buffer is usually just memory writes.
            // Optimization: Only redraw what changed? Or draw canvas to a backbuffer.
            // Wait, there is no way to draw a bitmap directly in mljos_ui_api_t yet.
            // I'll draw the canvas pixel by pixel.
            for (int y = 0; y < CANVAS_H; y++) {
                for (int x = 0; x < CANVAS_W; x++) {
                    uint32_t c = g_canvas[y * CANVAS_W + x];
                    api->ui->fill_rect(canvas_x + x, canvas_y + y, 1, 1, c);
                }
            }

            api->ui->draw_text("Paint - mljOS", sidebar_w + 10, 10, 0xFFFFFF);

            dirty = 0;
        }

        mljos_ui_event_t ev;
        while (api->ui->poll_event(&ev)) {
            if (ev.type == MLJOS_UI_EVENT_EXPOSE) {
                dirty = 1;
            } else if (ev.type == MLJOS_UI_EVENT_KEY_DOWN) {
                if (ev.key == 27) { api->ui->end_app(); return; }
            } else if (ev.type == MLJOS_UI_EVENT_MOUSE_LEFT_DOWN) {
                mouse_down = 1;
                // Check tools
                if (ev.x >= 10 && ev.x <= 50) {
                    if (ev.y >= 10 && ev.y <= 50) { g_is_eraser = 0; g_is_fill = 0; dirty = 1; }
                    else if (ev.y >= 60 && ev.y <= 100) { g_is_eraser = 1; g_is_fill = 0; dirty = 1; }
                    else if (ev.y >= 110 && ev.y <= 150) { g_is_fill = 1; g_is_eraser = 0; dirty = 1; }
                    else if (ev.y >= 160 && ev.y <= 200) { clear_canvas(0x000000); dirty = 1; }
                    else if (ev.y >= 210 && ev.y <= 250) {
                        char name[64];
                        if (api->ui->prompt_input("Save", "Enter filename (.bmp):", name, sizeof(name))) {
                            save_bmp(api, name);
                        }
                        dirty = 1;
                    }
                    else if (ev.y >= 260 && ev.y <= 300) {
                        char name[64];
                        if (api->ui->prompt_input("Load", "Enter filename (.bmp):", name, sizeof(name))) {
                            load_bmp(api, name);
                        }
                        dirty = 1;
                    }
                    else if (ev.y >= 320 && ev.y <= 350) { g_brush_size = 1; dirty = 1; }
                    else if (ev.y >= 355 && ev.y <= 385) { g_brush_size = 3; dirty = 1; }
                    else if (ev.y >= 390 && ev.y <= 420) { g_brush_size = 5; dirty = 1; }
                }

                // Palette
                if (ev.y >= sh - bottom_bar_h + 15 && ev.y <= sh - bottom_bar_h + 45) {
                    int pal_x = sidebar_w + 20;
                    for (int i = 0; i < PALETTE_COUNT; i++) {
                        if (ev.x >= pal_x && ev.x <= pal_x + 30) {
                            g_current_color = g_palette[i];
                            g_is_eraser = 0;
                            g_is_fill = 0;
                            dirty = 1;
                            break;
                        }
                        pal_x += 40;
                    }
                }

                // Drawing start
                if (ev.x >= canvas_x && ev.x < canvas_x + CANVAS_W &&
                    ev.y >= canvas_y && ev.y < canvas_y + CANVAS_H) {
                    int cx = ev.x - canvas_x;
                    int cy = ev.y - canvas_y;
                    if (g_is_fill) {
                        flood_fill(cx, cy, g_canvas[cy * CANVAS_W + cx], g_current_color);
                        dirty = 1;
                    } else {
                        draw_point(cx, cy, g_is_eraser ? 0x000000 : g_current_color, g_brush_size);
                        api->ui->fill_rect(ev.x - g_brush_size/2, ev.y - g_brush_size/2, g_brush_size, g_brush_size, g_is_eraser ? 0x000000 : g_current_color);
                        last_mx = cx;
                        last_my = cy;
                    }
                } else {
                    last_mx = -1;
                    last_my = -1;
                }
            } else if (ev.type == MLJOS_UI_EVENT_MOUSE_LEFT_UP) {
                mouse_down = 0;
                last_mx = -1;
                last_my = -1;
            } else if (ev.type == MLJOS_UI_EVENT_MOUSE_MOVE) {
                if (mouse_down) {
                    if (ev.x >= canvas_x && ev.x < canvas_x + CANVAS_W &&
                        ev.y >= canvas_y && ev.y < canvas_y + CANVAS_H) {
                        int cx = ev.x - canvas_x;
                        int cy = ev.y - canvas_y;
                        uint32_t col = g_is_eraser ? 0x000000 : g_current_color;
                        if (last_mx != -1) {
                            draw_line(last_mx, last_my, cx, cy, col, g_brush_size);
                            // Visual feedback (crude line draw on screen)
                            // We don't have a screen line draw, so we just use many fill_rects
                            // Similar to draw_line but with screen coordinates
                            int scx0 = last_mx + canvas_x;
                            int scy0 = last_my + canvas_y;
                            int scx1 = cx + canvas_x;
                            int scy1 = cy + canvas_y;
                            
                            int dx = scx1 - scx0; if (dx < 0) dx = -dx;
                            int dy = scy1 - scy0; if (dy < 0) dy = -dy;
                            int sx = (scx0 < scx1) ? 1 : -1;
                            int sy = (scy0 < scy1) ? 1 : -1;
                            int err = dx - dy;
                            while (1) {
                                api->ui->fill_rect(scx0 - g_brush_size/2, scy0 - g_brush_size/2, g_brush_size, g_brush_size, col);
                                if (scx0 == scx1 && scy0 == scy1) break;
                                int e2 = 2 * err;
                                if (e2 > -dy) { err -= dy; scx0 += sx; }
                                if (e2 < dx) { err += dx; scy0 += sy; }
                            }
                        } else {
                            draw_point(cx, cy, col, g_brush_size);
                            api->ui->fill_rect(ev.x - g_brush_size/2, ev.y - g_brush_size/2, g_brush_size, g_brush_size, col);
                        }
                        last_mx = cx;
                        last_my = cy;
                    } else {
                        last_mx = -1;
                        last_my = -1;
                    }
                }
            }
        }
    }
}

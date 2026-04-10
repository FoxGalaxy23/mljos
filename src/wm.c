#include "wm.h"

#include "apps_registry.h"
#include "bmp.h"
#include "console.h"
#include "disk.h"
#include "font.h"
#include "fs.h"
#include "io.h"
#include "kmem.h"
#include "kstring.h"
#include "rtc.h"
#include "task.h"
#include "users.h"

#define WM_MAX_WINDOWS 16
#define WM_MAX_EVENTS 64
static const int ICON_DRAW_PX = 48;
static const uint32_t ICON_MAX_BYTES = 8u * 1024u * 1024u;

// Decoration metrics
static const int BORDER_PX = 2;
static const int TITLE_PX = 20;
static const int BTN_PX = 14;
static const int BTN_GAP_PX = 4;
static const int TASKBAR_PX = 24;

// Colors (0xRRGGBB)
static const uint32_t COL_DESKTOP = 0x202225;
static const uint32_t COL_TASKBAR = 0x2B2D31;
static const uint32_t COL_TASKBAR_BTN = 0x3A3D44;
static const uint32_t COL_TASKBAR_BTN_ACTIVE = 0x50545D;

static const uint32_t COL_WIN_BORDER = 0x0F1012;
static const uint32_t COL_WIN_INNER = 0x2A2C31;
static const uint32_t COL_TITLE_BG = 0x1E3A8A;
static const uint32_t COL_TITLE_BG_INACTIVE = 0x30343A;
static const uint32_t COL_TITLE_FG = 0xFFFFFF;

static const uint32_t COL_BTN_BG = 0x3A3D44;
static const uint32_t COL_BTN_FG = 0xFFFFFF;
static const uint32_t COL_BTN_CLOSE = 0xB91C1C;

typedef struct wm_event_queue {
    mljos_ui_event_t ev[WM_MAX_EVENTS];
    uint8_t head;
    uint8_t tail;
} wm_event_queue_t;

struct wm_window {
    uint8_t used;
    uint8_t minimized;
    uint8_t maximized;
    uint8_t close_requested;
    int id;

    int x, y, w, h;         // outer rect in pixels
    int prev_x, prev_y, prev_w, prev_h;

    int client_x, client_y, client_w, client_h;
    uint32_t *client_px;
    uint32_t client_pitch;
    int bb_w, bb_h;

    char title[48];

    wm_event_queue_t q;
    task_t *owner;
};

static wm_window_t g_windows[WM_MAX_WINDOWS];
static int g_zorder[WM_MAX_WINDOWS];
static int g_zcount = 0;
static int g_next_id = 1;

static wm_window_t *g_focused = NULL;
static wm_window_t *g_terminal_window = NULL;

static int g_dirty = 1;
static int g_start_open = 0;
static char g_launch_req[32];
static int g_has_launch_req = 0;
static int g_gui_enabled = 1;

// Mouse
static int g_mouse_x = 10;
static int g_mouse_y = 10;
static int g_mouse_left = 0;
static int g_mouse_left_prev = 0;
static uint8_t g_mouse_pkt[4];
static int g_mouse_pkt_i = 0;
static int g_mouse_pkt_bytes = 3;
static int g_mouse_moved = 0;

static void ps2_wait_input_empty(void) {
    while (inb(0x64) & 2) { }
}

static int ps2_wait_output_full_timeout(uint32_t spins) {
    while (spins--) {
        if (inb(0x64) & 1) return 1;
        __asm__ volatile ("nop");
    }
    return 0;
}

static int ps2_read_data_timeout(uint8_t *out_byte, uint32_t spins) {
    if (!out_byte) return 0;
    if (!ps2_wait_output_full_timeout(spins)) return 0;
    *out_byte = inb(0x60);
    return 1;
}

static void ps2_write_cmd(uint8_t cmd) {
    ps2_wait_input_empty();
    outb(0x64, cmd);
}

static void ps2_write_data(uint8_t data) {
    ps2_wait_input_empty();
    outb(0x60, data);
}

static void ps2_flush_output(void) {
    g_mouse_pkt_i = 0;
    for (int i = 0; i < 1024; ++i) {
        uint8_t st = inb(0x64);
        if (!(st & 1)) break;
        (void)inb(0x60);
    }
}

static void ps2_mouse_write(uint8_t cmd) {
    ps2_wait_input_empty();
    outb(0x64, 0xD4);
    ps2_wait_input_empty();
    outb(0x60, cmd);
}

static int ps2_mouse_wait_ack(uint32_t spins) {
    while (spins--) {
        uint8_t st = inb(0x64);
        if (st & 1) {
            uint8_t b = inb(0x60);
            if (st & 0x20) {
                if (b == 0xFA) return 1;
                if (b == 0xFE) return 0;
            }
        }
        __asm__ volatile ("nop");
    }
    return 0;
}

static int ps2_mouse_get_id(uint8_t *out_id) {
    if (!out_id) return 0;
    ps2_mouse_write(0xF2);
    if (!ps2_mouse_wait_ack(500000)) return 0;
    return ps2_read_data_timeout(out_id, 500000);
}

static void ps2_configure_controller(void) {
    uint8_t cmd_byte = 0;

    ps2_write_cmd(0xAD); // disable keyboard
    ps2_write_cmd(0xA7); // disable mouse

    for (int i = 0; i < 64; ++i) {
        uint8_t dummy;
        if (!ps2_read_data_timeout(&dummy, 2000)) break;
    }

    ps2_write_cmd(0x20);
    (void)ps2_read_data_timeout(&cmd_byte, 200000);

    // Enable IRQ1/IRQ12, enable clocks.
    cmd_byte |= (1u << 0);
    cmd_byte |= (1u << 1);
    cmd_byte &= (uint8_t)~(1u << 4);
    cmd_byte &= (uint8_t)~(1u << 5);

    ps2_write_cmd(0x60);
    ps2_write_data(cmd_byte);

    ps2_write_cmd(0xAE); // enable keyboard
    ps2_write_cmd(0xA8); // enable mouse
}

static void ps2_mouse_init(void) {
    ps2_configure_controller();
    ps2_flush_output();

    // Set defaults, then enable streaming.
    ps2_mouse_write(0xF6);
    (void)ps2_mouse_wait_ack(500000);
    ps2_mouse_write(0xF4);
    (void)ps2_mouse_wait_ack(500000);

    // Try to enable wheel (IntelliMouse) mode.
    ps2_mouse_write(0xF3);
    (void)ps2_mouse_wait_ack(500000);
    ps2_mouse_write(200);
    (void)ps2_mouse_wait_ack(500000);
    ps2_mouse_write(0xF3);
    (void)ps2_mouse_wait_ack(500000);
    ps2_mouse_write(100);
    (void)ps2_mouse_wait_ack(500000);
    ps2_mouse_write(0xF3);
    (void)ps2_mouse_wait_ack(500000);
    ps2_mouse_write(80);
    (void)ps2_mouse_wait_ack(500000);

    uint8_t mouse_id = 0;
    if (ps2_mouse_get_id(&mouse_id) && mouse_id == 3) {
        g_mouse_pkt_bytes = 4;
    } else {
        g_mouse_pkt_bytes = 3;
    }

    ps2_flush_output();
}

typedef enum {
    DRAG_NONE = 0,
    DRAG_MOVE = 1,
    DRAG_RESIZE = 2,
} drag_mode_t;

static drag_mode_t g_drag_mode = DRAG_NONE;
static wm_window_t *g_drag_win = NULL;
static int g_drag_off_x = 0;
static int g_drag_off_y = 0;
static int g_resize_edges = 0; // bitmask: 1 left,2 right,4 top,8 bottom

// Scrollbar drag (for console windows)
static int g_scroll_dragging = 0;
static wm_window_t *g_scroll_drag_win = NULL;
static int g_scroll_drag_offset = 0;

// Full-screen backbuffer to reduce flicker/tearing.
static uint32_t *g_backbuf = NULL;
static uint32_t g_back_pitch = 0;
static uint64_t g_backbuf_bytes = 0;

// Desktop wallpaper (scaled to screen, 0x00RRGGBB)
static uint32_t *g_wallpaper = NULL;
static int g_wallpaper_w = 0;
static int g_wallpaper_h = 0;

#define WM_ICON_CACHE_MAX 64
typedef struct wm_icon_entry {
    char name[32];
    uint8_t loaded;
    uint32_t *px_scaled; // ICON_DRAW_PX x ICON_DRAW_PX, 0x00RRGGBB
    int scaled_w;
    int scaled_h;
    wm_icon_scale_mode_t scaled_mode;
} wm_icon_entry_t;

static wm_icon_entry_t g_icon_cache[WM_ICON_CACHE_MAX];

static uint32_t screen_w(void) { return fb_root.width; }
static uint32_t screen_h(void) { return fb_root.height; }

static uint32_t *wm_get_app_icon_scaled(const char *app_name, int target_px);
static int load_icon_file_exact(const char *path, void **out_buf, uint32_t *out_size);
static int window_scrollbar_rect(wm_window_t *w, int *out_x, int *out_y, int *out_w, int *out_h);
static uint32_t rd_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static wm_icon_scale_mode_t g_icon_scale_mode = WM_ICON_SCALE_NEAREST;

static int rect_contains(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

static void fill_rect(uint32_t *dst, uint32_t pitch, int dst_w, int dst_h, int x, int y, int w, int h, uint32_t rgb) {
    if (!dst) return;
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > dst_w) w = dst_w - x;
    if (y + h > dst_h) h = dst_h - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = 0; yy < h; ++yy) {
        uint32_t *row = (uint32_t *)((uintptr_t)dst + (uintptr_t)(y + yy) * pitch);
        for (int xx = 0; xx < w; ++xx) row[x + xx] = rgb;
    }
}

static void draw_char(uint32_t *dst, uint32_t pitch, int dst_w, int dst_h, char ch, int x, int y, uint32_t rgb) {
    (void)dst_h;
    if (!dst) return;
    if (x < 0 || y < 0) return;
    if (x + 8 > dst_w) return;
    const uint8_t *glyph = font8x16[(uint8_t)ch];
    for (int i = 0; i < 16; ++i) {
        uint8_t bits = glyph[i];
        uint32_t *row = (uint32_t *)((uintptr_t)dst + (uintptr_t)(y + i) * pitch);
        for (int j = 0; j < 8; ++j) {
            if (bits & (1u << (7 - j))) row[x + j] = rgb;
        }
    }
}

static void draw_text(uint32_t *dst, uint32_t pitch, int dst_w, int dst_h, const char *s, int x, int y, uint32_t rgb) {
    (void)dst_h;
    if (!s) return;
    int xx = x;
    for (int i = 0; s[i]; ++i) {
        draw_char(dst, pitch, dst_w, dst_h, s[i], xx, y, rgb);
        xx += 8;
        if (xx + 8 > dst_w) break;
    }
}

static void blit_rgb32(uint32_t *dst, uint32_t pitch, int dst_w, int dst_h,
    int x, int y, const uint32_t *src, int src_w, int src_h) {
    if (!dst || !src || src_w <= 0 || src_h <= 0) return;

    int x0 = x;
    int y0 = y;
    int x1 = x + src_w;
    int y1 = y + src_h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > dst_w) x1 = dst_w;
    if (y1 > dst_h) y1 = dst_h;
    if (x1 <= x0 || y1 <= y0) return;

    for (int yy = y0; yy < y1; ++yy) {
        int sy = yy - y;
        const uint32_t *src_row = src + (uint64_t)sy * (uint64_t)src_w;
        uint32_t *dst_row = (uint32_t *)((uintptr_t)dst + (uintptr_t)yy * pitch);
        for (int xx = x0; xx < x1; ++xx) {
            int sx = xx - x;
            dst_row[xx] = src_row[sx];
        }
    }
}

static void win_recompute_client(wm_window_t *w) {
    if (!w) return;
    w->client_x = w->x + BORDER_PX;
    w->client_y = w->y + BORDER_PX + TITLE_PX;
    w->client_w = w->w - BORDER_PX * 2;
    w->client_h = w->h - BORDER_PX * 2 - TITLE_PX;
    if (w->client_w < 1) w->client_w = 1;
    if (w->client_h < 1) w->client_h = 1;
}

static void win_ensure_backbuffer(wm_window_t *w) {
    if (!w) return;
    if (w->client_w < 1 || w->client_h < 1) return;
    if (w->client_px && w->bb_w == w->client_w && w->bb_h == w->client_h) return;

    uint32_t *old_px = w->client_px;
    int old_w = w->bb_w;
    int old_h = w->bb_h;
    uint32_t old_pitch = w->client_pitch;

    uint64_t bytes = (uint64_t)w->client_w * (uint64_t)w->client_h * 4ULL;
    w->client_px = (uint32_t *)kmem_alloc(bytes, 16);
    w->client_pitch = (uint32_t)(w->client_w * 4);
    kmem_memset(w->client_px, 0, bytes);
    w->bb_w = w->client_w;
    w->bb_h = w->client_h;

    // Preserve as much of the previous client surface as possible.
    if (old_px && old_w > 0 && old_h > 0 && old_pitch) {
        int copy_w = old_w < w->client_w ? old_w : w->client_w;
        int copy_h = old_h < w->client_h ? old_h : w->client_h;
        for (int yy = 0; yy < copy_h; ++yy) {
            uint32_t *src_row = (uint32_t *)((uintptr_t)old_px + (uintptr_t)yy * old_pitch);
            uint32_t *dst_row = (uint32_t *)((uintptr_t)w->client_px + (uintptr_t)yy * w->client_pitch);
            for (int xx = 0; xx < copy_w; ++xx) dst_row[xx] = src_row[xx];
        }
    }
}

static void console_rebind_if_needed(wm_window_t *w) {
    if (!w) return;
    if (!w->client_px) return;
    if (!w->owner || !w->owner->console) return;
    uint32_t bind_w = (uint32_t)w->client_w;
    int sb = console_scrollbar_width_px();
    if (sb > 0 && bind_w > (uint32_t)sb + 8) bind_w -= (uint32_t)sb;
    console_bind_target(w->owner->console, w->client_px, bind_w, (uint32_t)w->client_h, w->client_pitch);
    console_redraw(w->owner->console);
    wm_mark_dirty();
}

static void z_rebuild(void) {
    g_zcount = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; ++i) {
        if (g_windows[i].used) g_zorder[g_zcount++] = i;
    }
}

static void z_focus_index(int idx) {
    if (idx < 0 || idx >= WM_MAX_WINDOWS) return;
    int pos = -1;
    for (int i = 0; i < g_zcount; ++i) if (g_zorder[i] == idx) { pos = i; break; }
    if (pos < 0) return;
    for (int i = pos; i < g_zcount - 1; ++i) g_zorder[i] = g_zorder[i + 1];
    g_zorder[g_zcount - 1] = idx;
}

static wm_window_t *win_at(int x, int y) {
    for (int i = g_zcount - 1; i >= 0; --i) {
        wm_window_t *w = &g_windows[g_zorder[i]];
        if (!w->used || w->minimized) continue;
        if (rect_contains(x, y, w->x, w->y, w->w, w->h)) return w;
    }
    return NULL;
}

static int q_push(wm_event_queue_t *q, const mljos_ui_event_t *ev) {
    uint8_t next = (uint8_t)((q->tail + 1u) % WM_MAX_EVENTS);
    if (next == q->head) return 0;
    q->ev[q->tail] = *ev;
    q->tail = next;
    return 1;
}

static int q_pop(wm_event_queue_t *q, mljos_ui_event_t *out) {
    if (q->head == q->tail) return 0;
    *out = q->ev[q->head];
    q->head = (uint8_t)((q->head + 1u) % WM_MAX_EVENTS);
    return 1;
}

static void win_post_key(wm_window_t *w, int key) {
    if (!w) return;
    mljos_ui_event_t ev;
    ev.type = MLJOS_UI_EVENT_KEY_DOWN;
    ev.x = 0;
    ev.y = 0;
    ev.key = key;
    (void)q_push(&w->q, &ev);
}

static void win_post_mouse(wm_window_t *w, uint32_t type, int x, int y) {
    if (!w) return;
    mljos_ui_event_t ev;
    ev.type = type;
    ev.x = x;
    ev.y = y;
    ev.key = 0;
    (void)q_push(&w->q, &ev);
}

static void win_post_expose(wm_window_t *w) {
    if (!w) return;
    mljos_ui_event_t ev;
    ev.type = MLJOS_UI_EVENT_EXPOSE;
    ev.x = 0;
    ev.y = 0;
    ev.key = 0;
    (void)q_push(&w->q, &ev);
}

static void draw_window_frame(uint32_t *dst, uint32_t pitch, int sw, int sh, wm_window_t *w) {
    (void)sh;
    if (!w || w->minimized) return;
    uint32_t title_bg = (w == g_focused) ? COL_TITLE_BG : COL_TITLE_BG_INACTIVE;
    fill_rect(dst, pitch, sw, (int)screen_h(), w->x, w->y, w->w, w->h, COL_WIN_BORDER);
    fill_rect(dst, pitch, sw, (int)screen_h(), w->x + 1, w->y + 1, w->w - 2, w->h - 2, COL_WIN_INNER);

    // Title bar
    fill_rect(dst, pitch, sw, (int)screen_h(), w->x + BORDER_PX, w->y + BORDER_PX, w->w - BORDER_PX * 2, TITLE_PX, title_bg);
    draw_text(dst, pitch, sw, (int)screen_h(), w->title, w->x + BORDER_PX + 6, w->y + BORDER_PX + 2, COL_TITLE_FG);

    // Buttons: [min][max][close]
    int bx3 = w->x + w->w - BORDER_PX - BTN_PX;
    int by = w->y + BORDER_PX + (TITLE_PX - BTN_PX) / 2;
    int bx2 = bx3 - BTN_PX - BTN_GAP_PX;
    int bx1 = bx2 - BTN_PX - BTN_GAP_PX;

    fill_rect(dst, pitch, sw, (int)screen_h(), bx1, by, BTN_PX, BTN_PX, COL_BTN_BG);
    // Minimize icon: small horizontal bar.
    fill_rect(dst, pitch, sw, (int)screen_h(), bx1 + 3, by + BTN_PX - 4, BTN_PX - 6, 2, COL_BTN_FG);

    fill_rect(dst, pitch, sw, (int)screen_h(), bx2, by, BTN_PX, BTN_PX, COL_BTN_BG);
    // Maximize / restore icon.
    if (!w->maximized) {
        // Single outline rectangle.
        fill_rect(dst, pitch, sw, (int)screen_h(), bx2 + 3, by + 3, BTN_PX - 6, 2, COL_BTN_FG);
        fill_rect(dst, pitch, sw, (int)screen_h(), bx2 + 3, by + BTN_PX - 5, BTN_PX - 6, 2, COL_BTN_FG);
        fill_rect(dst, pitch, sw, (int)screen_h(), bx2 + 3, by + 3, 2, BTN_PX - 6, COL_BTN_FG);
        fill_rect(dst, pitch, sw, (int)screen_h(), bx2 + BTN_PX - 5, by + 3, 2, BTN_PX - 6, COL_BTN_FG);
    } else {
        // Two overlapping outline rectangles.
        int ox = 2;
        int oy = 2;
        fill_rect(dst, pitch, sw, (int)screen_h(), bx2 + 4 + ox, by + 2 + oy, BTN_PX - 7, 2, COL_BTN_FG);
        fill_rect(dst, pitch, sw, (int)screen_h(), bx2 + 4 + ox, by + BTN_PX - 6 + oy, BTN_PX - 7, 2, COL_BTN_FG);
        fill_rect(dst, pitch, sw, (int)screen_h(), bx2 + 4 + ox, by + 2 + oy, 2, BTN_PX - 8, COL_BTN_FG);
        fill_rect(dst, pitch, sw, (int)screen_h(), bx2 + BTN_PX - 5 + ox, by + 2 + oy, 2, BTN_PX - 8, COL_BTN_FG);

        fill_rect(dst, pitch, sw, (int)screen_h(), bx2 + 3, by + 4, BTN_PX - 7, 2, COL_BTN_FG);
        fill_rect(dst, pitch, sw, (int)screen_h(), bx2 + 3, by + BTN_PX - 4, BTN_PX - 7, 2, COL_BTN_FG);
        fill_rect(dst, pitch, sw, (int)screen_h(), bx2 + 3, by + 4, 2, BTN_PX - 8, COL_BTN_FG);
        fill_rect(dst, pitch, sw, (int)screen_h(), bx2 + BTN_PX - 6, by + 4, 2, BTN_PX - 8, COL_BTN_FG);
    }

    fill_rect(dst, pitch, sw, (int)screen_h(), bx3, by, BTN_PX, BTN_PX, COL_BTN_CLOSE);
    draw_char(dst, pitch, sw, (int)screen_h(), 'X', bx3 + 4, by + 2, COL_BTN_FG);
}

static void blit_client(uint32_t *dst, uint32_t pitch, int sw, wm_window_t *w) {
    if (!w || w->minimized) return;
    if (!w->client_px) return;
    for (int yy = 0; yy < w->client_h; ++yy) {
        uint32_t *src_row = (uint32_t *)((uintptr_t)w->client_px + (uintptr_t)yy * w->client_pitch);
        uint32_t *dst_row = (uint32_t *)((uintptr_t)dst + (uintptr_t)(w->client_y + yy) * pitch);
        for (int xx = 0; xx < w->client_w; ++xx) dst_row[w->client_x + xx] = src_row[xx];
    }
}

static void draw_console_scrollbar(uint32_t *dst, uint32_t pitch, int sw, int sh, wm_window_t *w) {
    (void)sh;
    if (!w || !w->owner || !w->owner->console) return;
    int sb_x, sb_y, sb_w, sb_h;
    if (!window_scrollbar_rect(w, &sb_x, &sb_y, &sb_w, &sb_h)) return;

    int top = 0, visible = 0, total = 0;
    console_get_scroll_info(w->owner->console, &top, &visible, &total);
    if (visible <= 0 || total <= 0) return;

    uint32_t track_col = 0x1A1B1E;
    uint32_t thumb_col = 0x4B5563;
    uint32_t border_col = 0x0F1012;

    fill_rect(dst, pitch, sw, (int)screen_h(), sb_x, sb_y, sb_w, sb_h, track_col);
    fill_rect(dst, pitch, sw, (int)screen_h(), sb_x, sb_y, sb_w, 1, border_col);
    fill_rect(dst, pitch, sw, (int)screen_h(), sb_x, sb_y + sb_h - 1, sb_w, 1, border_col);

    int max_scroll = total - visible;
    if (max_scroll < 1) {
        fill_rect(dst, pitch, sw, (int)screen_h(), sb_x + 1, sb_y + 1, sb_w - 2, sb_h - 2, thumb_col);
        return;
    }

    int thumb_h = (sb_h * visible) / total;
    if (thumb_h < 16) thumb_h = 16;
    if (thumb_h > sb_h) thumb_h = sb_h;
    int thumb_y = sb_y + (int)((uint64_t)(sb_h - thumb_h) * (uint64_t)top / (uint64_t)max_scroll);
    fill_rect(dst, pitch, sw, (int)screen_h(), sb_x + 1, thumb_y, sb_w - 2, thumb_h, thumb_col);
}

static void draw_taskbar(uint32_t *dst, uint32_t pitch, int sw, int sh) {
    int y0 = sh - TASKBAR_PX;
    fill_rect(dst, pitch, sw, sh, 0, y0, sw, TASKBAR_PX, COL_TASKBAR);

    // Start button
    int start_w = 70;
    uint32_t start_col = g_start_open ? COL_TASKBAR_BTN_ACTIVE : COL_TASKBAR_BTN;
    fill_rect(dst, pitch, sw, sh, 6, y0 + 4, start_w, TASKBAR_PX - 8, start_col);
    draw_text(dst, pitch, sw, sh, "Start", 14, y0 + 6, 0xFFFFFF);

    // Window buttons
    int x = 6 + start_w + 10;
    for (int i = 0; i < g_zcount; ++i) {
        wm_window_t *w = &g_windows[g_zorder[i]];
        if (!w->used) continue;
        int bw = 120;
        if (x + bw > sw - 90) break;
        uint32_t col = (w == g_focused && !w->minimized) ? COL_TASKBAR_BTN_ACTIVE : COL_TASKBAR_BTN;
        fill_rect(dst, pitch, sw, sh, x, y0 + 4, bw, TASKBAR_PX - 8, col);
        draw_text(dst, pitch, sw, sh, w->title, x + 6, y0 + 6, 0xFFFFFF);
        x += bw + 6;
    }

    // Clock
    unsigned char hh, mm, ss;
    get_rtc_time(&hh, &mm, &ss);
    char buf[16];
    buf[0] = '0' + (hh / 10);
    buf[1] = '0' + (hh % 10);
    buf[2] = ':';
    buf[3] = '0' + (mm / 10);
    buf[4] = '0' + (mm % 10);
    buf[5] = '\0';
    draw_text(dst, pitch, sw, sh, buf, sw - 60, y0 + 6, 0xFFFFFF);
}

static void draw_start_menu(uint32_t *dst, uint32_t pitch, int sw, int sh) {
    if (!g_start_open) return;
    int menu_w = 220;
    int menu_h = 220;
    int x0 = 6;
    int y0 = sh - TASKBAR_PX - menu_h;
    fill_rect(dst, pitch, sw, sh, x0, y0, menu_w, menu_h, 0x1A1B1E);
    fill_rect(dst, pitch, sw, sh, x0, y0, menu_w, 2, COL_WIN_BORDER);
    fill_rect(dst, pitch, sw, sh, x0, y0, 2, menu_h, COL_WIN_BORDER);
    fill_rect(dst, pitch, sw, sh, x0 + menu_w - 2, y0, 2, menu_h, COL_WIN_BORDER);
    fill_rect(dst, pitch, sw, sh, x0, y0 + menu_h - 2, menu_w, 2, COL_WIN_BORDER);

    int pad = 10;
    int content_w = menu_w - pad * 2;
    int content_h = menu_h - pad * 2;
    int cell_h = ICON_DRAW_PX + 16 + 6;
    if (cell_h < ICON_DRAW_PX + 8) cell_h = ICON_DRAW_PX + 8;
    int cols = content_w / (ICON_DRAW_PX + 12);
    if (cols < 1) cols = 1;
    int cell_w = content_w / cols;
    if (cell_w < ICON_DRAW_PX) cell_w = ICON_DRAW_PX;
    int rows = content_h / cell_h;
    if (rows < 1) rows = 1;

    int y = y0 + pad;
    int count = 0;
    const mljos_app_descriptor_t *apps = apps_registry_list(&count);
    int col = 0;
    int row = 0;
    for (int i = 0; i < count; ++i) {
        if (!apps[i].supports_ui) continue;
        int cell_x = x0 + pad + col * cell_w;
        int cell_y = y0 + pad + row * cell_h;
        if (cell_y + cell_h > y0 + menu_h - pad) break;

        uint32_t *icon = wm_get_app_icon_scaled(apps[i].name, ICON_DRAW_PX);
        int icon_x = cell_x + (cell_w - ICON_DRAW_PX) / 2;
        int icon_y = cell_y;
        if (icon) blit_rgb32(dst, pitch, sw, sh, icon_x, icon_y, icon, ICON_DRAW_PX, ICON_DRAW_PX);

        const char *label = apps[i].title ? apps[i].title : apps[i].name;
        int max_chars = cell_w / 8;
        if (max_chars < 1) max_chars = 1;
        char tmp[32];
        int li = 0;
        while (label[li] && li < (int)sizeof(tmp) - 1 && li < max_chars) { tmp[li] = label[li]; li++; }
        tmp[li] = '\0';
        int text_px = li * 8;
        int text_x = cell_x + (cell_w - text_px) / 2;
        int text_y = cell_y + ICON_DRAW_PX + 2;
        draw_text(dst, pitch, sw, sh, tmp, text_x, text_y, 0xFFFFFF);

        col++;
        if (col >= cols) { col = 0; row++; }
        if (row >= rows) break;
    }
}

static void draw_cursor(uint32_t *dst, uint32_t pitch, int sw, int sh) {
    // Simple arrow cursor (no save/restore; relies on full redraw when dirty).
    static const uint8_t shape[16] = { 1,2,3,4,5,6,7,8,5,5,5,3,3,2,2,1 };
    int x = g_mouse_x;
    int y = g_mouse_y;
    for (int yy = 0; yy < 16; ++yy) {
        int w = shape[yy];
        if (y + yy < 0 || y + yy >= sh) continue;
        uint32_t *row = (uint32_t *)((uintptr_t)dst + (uintptr_t)(y + yy) * pitch);
        for (int xx = 0; xx < w; ++xx) {
            if (x + xx < 0 || x + xx >= sw) continue;
            uint32_t col = (xx == 0 || xx == w - 1 || yy == 0) ? 0x000000 : 0xFFFFFF;
            row[x + xx] = col;
        }
    }
}

void wm_mark_dirty(void) {
    g_dirty = 1;
}

static int load_file_anywhere(const char *path, void **out_buf, uint32_t *out_size, uint32_t maxlen) {
    if (!path || !out_buf || !out_size || maxlen == 0) return 0;

    char *buf = (char *)kmem_alloc((uint64_t)maxlen, 16);
    if (!buf) return 0;

    uint32_t size = 0;
    int ok = 0;

    if (users_system_is_installed()) {
        ok = disk_read_file(path, buf, (int)maxlen, &size);
        if (!ok) ok = fs_read_file(path, buf, (int)maxlen, &size);
    } else {
        ok = fs_read_file(path, buf, (int)maxlen, &size);
        if (!ok) ok = disk_read_file(path, buf, (int)maxlen, &size);
    }

    if (!ok || size == 0) return 0;
    *out_buf = buf;
    *out_size = size;
    return 1;
}

static void wm_try_load_wallpaper(void) {
    if (!screen_w() || !screen_h()) return;
    if (g_wallpaper) return;

    void *file = NULL;
    uint32_t file_size = 0;
    if (!load_file_anywhere("/wallpaper.bmp", &file, &file_size, 16u * 1024u * 1024u)) return;

    uint32_t *px = NULL;
    int w = 0;
    int h = 0;
    if (!bmp_decode_rgb32(file, file_size, &px, &w, &h)) return;
    if (w <= 0 || h <= 0) return;

    // Scale once to screen size (nearest neighbor).
    uint32_t *scaled = NULL;
    if (w == (int)screen_w() && h == (int)screen_h()) {
        scaled = px;
    } else {
        if (!bmp_scale_nearest_rgb32(px, w, h, &scaled, (int)screen_w(), (int)screen_h())) return;
    }

    g_wallpaper = scaled;
    g_wallpaper_w = (int)screen_w();
    g_wallpaper_h = (int)screen_h();
}

static wm_icon_entry_t *icon_cache_find_or_alloc(const char *name) {
    if (!name || !name[0]) return NULL;

    for (int i = 0; i < WM_ICON_CACHE_MAX; ++i) {
        if (g_icon_cache[i].loaded && g_icon_cache[i].name[0] && strcmp(g_icon_cache[i].name, name) == 0) return &g_icon_cache[i];
    }
    for (int i = 0; i < WM_ICON_CACHE_MAX; ++i) {
        if (!g_icon_cache[i].loaded && !g_icon_cache[i].name[0]) {
            strncpy(g_icon_cache[i].name, name, sizeof(g_icon_cache[i].name) - 1);
            g_icon_cache[i].name[sizeof(g_icon_cache[i].name) - 1] = '\0';
            return &g_icon_cache[i];
        }
    }
    return NULL;
}

static int build_icon_path(char *out, int out_size, const char *prefix, const char *name) {
    if (!out || out_size < 8 || !prefix || !name) return 0;
    int pos = 0;
    for (int i = 0; prefix[i] && pos < out_size - 1; ++i) out[pos++] = prefix[i];
    for (int i = 0; name[i] && pos < out_size - 1; ++i) out[pos++] = name[i];
    const char *suf = ".bmp";
    for (int i = 0; suf[i] && pos < out_size - 1; ++i) out[pos++] = suf[i];
    out[pos] = '\0';
    return pos > 0;
}

static int load_icon_file_exact(const char *path, void **out_buf, uint32_t *out_size) {
    if (!path || !out_buf || !out_size) return 0;
    *out_buf = NULL;
    *out_size = 0;

    uint8_t header[54];
    uint32_t got = 0;
    int ok = 0;

    if (users_system_is_installed()) {
        ok = disk_read_file_prefix(path, (char *)header, (int)sizeof(header), &got);
        if (!ok) ok = fs_read_file_prefix(path, (char *)header, (int)sizeof(header), &got);
    } else {
        ok = fs_read_file_prefix(path, (char *)header, (int)sizeof(header), &got);
        if (!ok) ok = disk_read_file_prefix(path, (char *)header, (int)sizeof(header), &got);
    }
    if (!ok || got < 54) return 0;
    if (header[0] != 'B' || header[1] != 'M') return 0;
    uint32_t file_size = rd_u32_le(header + 2);
    if (file_size < 54 || file_size > ICON_MAX_BYTES) return 0;

    char *buf = (char *)kmem_alloc((uint64_t)file_size, 16);
    if (!buf) return 0;

    if (users_system_is_installed()) {
        ok = disk_read_file(path, buf, (int)file_size, &got);
        if (!ok) ok = fs_read_file_prefix(path, buf, (int)file_size, &got);
    } else {
        ok = fs_read_file_prefix(path, buf, (int)file_size, &got);
        if (!ok) ok = disk_read_file(path, buf, (int)file_size, &got);
    }

    if (!ok || got != file_size) return 0;
    *out_buf = buf;
    *out_size = file_size;
    return 1;
}

static uint32_t *wm_get_app_icon_scaled(const char *app_name, int target_px) {
    wm_icon_entry_t *e = icon_cache_find_or_alloc(app_name);
    if (!e) return NULL;
    if (e->loaded && e->scaled_w == target_px && e->scaled_h == target_px && e->scaled_mode == g_icon_scale_mode) return e->px_scaled;

    // Mark as attempted for this size/mode to avoid reloading missing icons every frame.
    e->loaded = 1;
    e->px_scaled = NULL;
    e->scaled_w = target_px;
    e->scaled_h = target_px;
    e->scaled_mode = g_icon_scale_mode;

    const char *prefixes[] = {
        "/apps/icons/",
        "/apps/",
        "/icons/",
    };

    for (unsigned int i = 0; i < (unsigned int)(sizeof(prefixes) / sizeof(prefixes[0])); ++i) {
        char path[128];
        if (!build_icon_path(path, (int)sizeof(path), prefixes[i], app_name)) continue;

        void *file = NULL;
        uint32_t file_size = 0;
        if (!load_icon_file_exact(path, &file, &file_size)) continue;

        uint32_t *px = NULL;
        int w = 0;
        int h = 0;
        if (!bmp_decode_rgb32(file, file_size, &px, &w, &h)) continue;

        uint32_t *scaled = NULL;
        if (w == target_px && h == target_px) scaled = px;
        else {
            if (g_icon_scale_mode == WM_ICON_SCALE_BILINEAR) {
                if (!bmp_scale_bilinear_rgb32(px, w, h, &scaled, target_px, target_px)) continue;
            } else {
                if (!bmp_scale_nearest_rgb32(px, w, h, &scaled, target_px, target_px)) continue;
            }
        }

        e->px_scaled = scaled;
        e->scaled_w = target_px;
        e->scaled_h = target_px;
        return e->px_scaled;
    }

    return NULL;
}

void wm_init(void) {
    kmem_memset(g_windows, 0, sizeof(g_windows));
    kmem_memset(g_icon_cache, 0, sizeof(g_icon_cache));
    g_zcount = 0;
    g_next_id = 1;
    g_focused = NULL;
    g_terminal_window = NULL;
    g_start_open = 0;
    g_has_launch_req = 0;
    g_drag_mode = DRAG_NONE;
    g_drag_win = NULL;
    g_gui_enabled = 1;
    g_dirty = 1;
    z_rebuild();

    ps2_mouse_init();
    g_mouse_x = (int)(screen_w() / 2);
    g_mouse_y = (int)(screen_h() / 2);

    // Allocate a full-screen backbuffer (ARGB32) for compositing.
    if (fb_root.address && screen_w() && screen_h()) {
        uint64_t bytes = (uint64_t)screen_w() * (uint64_t)screen_h() * 4ULL;
        g_backbuf = (uint32_t *)kmem_alloc(bytes, 16);
        g_back_pitch = (uint32_t)(screen_w() * 4);
        g_backbuf_bytes = bytes;
        kmem_memset(g_backbuf, 0, bytes);
    }

}

void wm_on_resolution_change(void) {
    int sw = (int)screen_w();
    int sh = (int)screen_h();
    if (sw <= 0 || sh <= 0) return;

    // Reset wallpaper so it is reloaded and scaled to the new size.
    g_wallpaper = NULL;
    g_wallpaper_w = 0;
    g_wallpaper_h = 0;

    // Ensure backbuffer is large enough for the new size.
    uint64_t bytes = (uint64_t)sw * (uint64_t)sh * 4ULL;
    if (!g_backbuf || bytes > g_backbuf_bytes) {
        g_backbuf = (uint32_t *)kmem_alloc(bytes, 16);
        g_backbuf_bytes = bytes;
    }
    g_back_pitch = (uint32_t)(sw * 4);
    if (g_backbuf) kmem_memset(g_backbuf, 0, bytes);

    // Clamp mouse to new bounds.
    if (g_mouse_x < 0) g_mouse_x = 0;
    if (g_mouse_y < 0) g_mouse_y = 0;
    if (g_mouse_x >= sw) g_mouse_x = sw - 1;
    if (g_mouse_y >= sh) g_mouse_y = sh - 1;

    int work_h = sh - TASKBAR_PX;
    if (work_h < 1) work_h = 1;

    for (int i = 0; i < WM_MAX_WINDOWS; ++i) {
        wm_window_t *w = &g_windows[i];
        if (!w->used) continue;

        int max_w = sw;
        int max_h = work_h;
        if (max_w < 1) max_w = 1;
        if (max_h < 1) max_h = 1;

        if (w->maximized) {
            w->x = 6;
            w->y = 6;
            w->w = sw - 12;
            w->h = work_h - 12;
        }

        if (w->w > max_w) w->w = max_w;
        if (w->h > max_h) w->h = max_h;

        if (max_w >= 160 && w->w < 160) w->w = 160;
        if (max_h >= 120 && w->h < 120) w->h = 120;
        if (w->w < 1) w->w = 1;
        if (w->h < 1) w->h = 1;

        if (w->x + w->w > max_w) w->x = max_w - w->w;
        if (w->y + w->h > max_h) w->y = max_h - w->h;
        if (w->x < 0) w->x = 0;
        if (w->y < 0) w->y = 0;

        win_recompute_client(w);
        win_ensure_backbuffer(w);
        console_rebind_if_needed(w);
        win_post_expose(w);
    }

    wm_mark_dirty();
}

void wm_set_icon_scale_mode(wm_icon_scale_mode_t mode) {
    g_icon_scale_mode = mode;
    kmem_memset(g_icon_cache, 0, sizeof(g_icon_cache));
    wm_mark_dirty();
}

int wm_gui_enabled(void) {
    return g_gui_enabled ? 1 : 0;
}

void wm_set_gui_enabled(int enabled) {
    int v = enabled ? 1 : 0;
    if (g_gui_enabled == v) return;
    g_gui_enabled = v;
    if (!g_gui_enabled) g_start_open = 0;
    wm_mark_dirty();
}

wm_window_t *wm_window_create(const char *title, int client_w_px, int client_h_px) {
    int slot = -1;
    for (int i = 0; i < WM_MAX_WINDOWS; ++i) {
        if (!g_windows[i].used) { slot = i; break; }
    }
    if (slot < 0) return NULL;

    wm_window_t *w = &g_windows[slot];
    kmem_memset(w, 0, sizeof(*w));
    w->used = 1;
    w->id = g_next_id++;
    w->minimized = 0;
    w->maximized = 0;

    // Outer size from client size
    w->w = client_w_px + BORDER_PX * 2;
    w->h = client_h_px + BORDER_PX * 2 + TITLE_PX;
    if (w->w < 160) w->w = 160;
    if (w->h < 120) w->h = 120;

    // Cascade placement
    int sx = (int)screen_w();
    int sy = (int)screen_h() - TASKBAR_PX;
    int cx = 60 + (slot * 18);
    int cy = 50 + (slot * 18);
    if (cx + w->w > sx) cx = 10;
    if (cy + w->h > sy) cy = 10;
    w->x = cx;
    w->y = cy;

    w->title[0] = '\0';
    if (title) {
        int i = 0;
        while (title[i] && i < (int)sizeof(w->title) - 1) { w->title[i] = title[i]; i++; }
        w->title[i] = '\0';
    }

    win_recompute_client(w);
    win_ensure_backbuffer(w);
    win_post_expose(w);

    z_rebuild();
    wm_window_focus(w);
    wm_mark_dirty();
    return w;
}

void wm_window_destroy(wm_window_t *w) {
    if (!w) return;
    if (g_terminal_window == w) g_terminal_window = NULL;
    w->used = 0;
    if (g_focused == w) g_focused = NULL;
    z_rebuild();
    wm_mark_dirty();
}

void wm_window_set_owner(wm_window_t *w, struct task *owner) {
    if (!w) return;
    w->owner = (task_t *)owner;
}

void wm_window_set_title(wm_window_t *w, const char *title) {
    if (!w) return;
    w->title[0] = '\0';
    if (title) {
        int i = 0;
        while (title[i] && i < (int)sizeof(w->title) - 1) { w->title[i] = title[i]; i++; }
        w->title[i] = '\0';
    }
    wm_mark_dirty();
}

void wm_window_focus(wm_window_t *w) {
    if (!w) return;
    w->minimized = 0;
    g_focused = w;
    int idx = (int)(w - g_windows);
    z_focus_index(idx);
    wm_mark_dirty();
    win_post_expose(w);
}

wm_window_t *wm_terminal_window(void) {
    return g_terminal_window;
}

void wm_set_terminal_window(wm_window_t *w) {
    g_terminal_window = w;
}

int wm_window_client_w(const wm_window_t *w) { return w ? w->client_w : 0; }
int wm_window_client_h(const wm_window_t *w) { return w ? w->client_h : 0; }
uint32_t *wm_window_client_pixels(const wm_window_t *w) { return w ? w->client_px : NULL; }
uint32_t wm_window_client_pitch_bytes(const wm_window_t *w) { return w ? w->client_pitch : 0; }

int wm_window_poll_event(wm_window_t *w, mljos_ui_event_t *out) {
    if (!w || !out) return 0;
    return q_pop(&w->q, out);
}

void wm_window_post_expose(wm_window_t *w) {
    win_post_expose(w);
}

int wm_consume_launch_request(char *out_name, int out_size) {
    if (!out_name || out_size < 2) return 0;
    if (!g_has_launch_req) return 0;
    int i = 0;
    while (g_launch_req[i] && i < out_size - 1) { out_name[i] = g_launch_req[i]; i++; }
    out_name[i] = '\0';
    g_has_launch_req = 0;
    g_launch_req[0] = '\0';
    return 1;
}

static int text_len(const char *s) {
    int i = 0;
    while (s && s[i]) i++;
    return i;
}

int wm_prompt_input(const char *title, const char *prompt, char *out_buf, int max_len) {
    if (!title) title = "Input";
    if (!prompt) prompt = "Enter text:";
    
    // Create a real child window for the dialog
    wm_window_t *w = wm_window_create(title, 340, 130);
    if (!w) return 0;
    
    // Attach to the current task so it feels like the application's window
    task_t *owner = task_current();
    if (owner) wm_window_set_owner(w, owner);
    
    wm_window_focus(w);
    
    out_buf[0] = '\0';
    int done = 0;
    int status = 0;
    
    while (!done) {
        mljos_ui_event_t ev;
        while (wm_window_poll_event(w, &ev)) {
            if (ev.type == MLJOS_UI_EVENT_KEY_DOWN) {
                if (ev.key == '\n' || ev.key == '\r') {
                    status = 1;
                    done = 1;
                } else if (ev.key == 27) {
                    status = 0;
                    done = 1;
                } else if (ev.key == '\b') {
                    int l = text_len(out_buf);
                    if (l > 0) out_buf[l - 1] = '\0';
                } else if (ev.key >= 32 && ev.key <= 126) {
                    int l = text_len(out_buf);
                    if (l < max_len - 1) {
                        out_buf[l] = (char)ev.key;
                        out_buf[l + 1] = '\0';
                    }
                }
            }
        }
        
        if (w->close_requested) {
            status = 0;
            done = 1;
        }
        
        // Render the dialog content into the client canvas
        if (w->client_px) {
            fill_rect(w->client_px, w->client_pitch, w->client_w, w->client_h, 0, 0, w->client_w, w->client_h, 0x1E1E1E);
            draw_text(w->client_px, w->client_pitch, w->client_w, w->client_h, prompt, 15, 20, 0xCCCCCC);
            
            fill_rect(w->client_px, w->client_pitch, w->client_w, w->client_h, 15, 55, w->client_w - 30, 24, 0x0E0E0E);
            draw_text(w->client_px, w->client_pitch, w->client_w, w->client_h, out_buf, 20, 60, 0xFFFFFF);
            int cursor_x = 20 + text_len(out_buf) * 8;
            draw_text(w->client_px, w->client_pitch, w->client_w, w->client_h, "_", cursor_x, 60, 0xFFFFFF);
            
            draw_text(w->client_px, w->client_pitch, w->client_w, w->client_h, "[Enter] OK     [Esc] Cancel", 65, 100, 0x888888);
            
            win_post_expose(w);
            wm_mark_dirty();
        }
        
        task_yield();
    }
    
    wm_window_destroy(w);
    wm_mark_dirty();
    return status;
}

static void set_launch_req(const char *name) {
    if (!name || !name[0]) return;
    int i = 0;
    while (name[i] && i < (int)sizeof(g_launch_req) - 1) { g_launch_req[i] = name[i]; i++; }
    g_launch_req[i] = '\0';
    g_has_launch_req = 1;
}

// Keyboard translation (subset reused from old shell)
static const char scancode_map_normal[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',
    0, '*', 0, ' ', 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static const char scancode_map_shift[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?',
    0, '*', 0, ' ', 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static int g_kbd_shift = 0;
static int g_kbd_ctrl = 0;
static int g_kbd_alt = 0;
static int g_kbd_caps = 0;
static int g_kbd_extended = 0;

static void kbd_handle_mod(uint8_t sc) {
    if (sc == 0xE0) { g_kbd_extended = 1; return; }
    if (!g_kbd_extended) {
        if (sc == 0x2A || sc == 0x36) { g_kbd_shift = 1; return; }
        if (sc == 0xAA || sc == 0xB6) { g_kbd_shift = 0; return; }
        if (sc == 0x1D) { g_kbd_ctrl = 1; return; }
        if (sc == 0x9D) { g_kbd_ctrl = 0; return; }
        if (sc == 0x38) { g_kbd_alt = 1; return; }
        if (sc == 0xB8) { g_kbd_alt = 0; return; }
        if (sc == 0x3A) { g_kbd_caps = !g_kbd_caps; return; }
        return;
    }
    if (sc == 0x1D) { g_kbd_ctrl = 1; g_kbd_extended = 0; return; }
    if (sc == 0x9D) { g_kbd_ctrl = 0; g_kbd_extended = 0; return; }
    if (sc == 0x38) { g_kbd_alt = 1; g_kbd_extended = 0; return; }
    if (sc == 0xB8) { g_kbd_alt = 0; g_kbd_extended = 0; return; }
}

static int kbd_translate_arrow(uint8_t make, int extended) {
    if (!extended) return 0;
    if (make == 0x48) return 1000;
    if (make == 0x50) return 1001;
    if (make == 0x4B) return 1002;
    if (make == 0x4D) return 1003;
    return 0;
}

static int kbd_scancode_to_key(uint8_t sc) {
    kbd_handle_mod(sc);
    if (sc == 0xE0) return 0;

    int extended = g_kbd_extended;
    if (extended) g_kbd_extended = 0;

    if (sc & 0x80) return 0; // key-up ignored
    uint8_t make = sc & 0x7F;

    int arrow = kbd_translate_arrow(make, extended);
    if (arrow) return arrow;

    if (make >= 128) return 0;
    int is_letter = ((make >= 0x10 && make <= 0x19) || (make >= 0x1E && make <= 0x26) || (make >= 0x2C && make <= 0x32));
    int use_shift = g_kbd_shift;
    if (g_kbd_caps && is_letter) use_shift = !use_shift;
    char c = use_shift ? scancode_map_shift[make] : scancode_map_normal[make];
    if (!c) return 0;

    if (g_kbd_ctrl && is_letter) {
        char base = scancode_map_normal[make];
        if (base >= 'a' && base <= 'z') return base - 'a' + 1;
    }
    return (int)c;
}

static void handle_mouse_wheel(int delta);

static void mouse_push_byte(uint8_t b) {
    // First byte must have bit3 set; helps resync on packet loss.
    if (g_mouse_pkt_i == 0 && (b & 0x08) == 0) return;
    g_mouse_pkt[g_mouse_pkt_i++] = b;
    if (g_mouse_pkt_i < g_mouse_pkt_bytes) return;
    g_mouse_pkt_i = 0;

    uint8_t b0 = g_mouse_pkt[0];
    int dx = (int)((signed char)g_mouse_pkt[1]);
    int dy = (int)((signed char)g_mouse_pkt[2]);
    int wheel = 0;
    if (g_mouse_pkt_bytes == 4) wheel = (int)((signed char)g_mouse_pkt[3]);

    // Standard PS/2: dy is negative when moving up.
    (void)b0;
    g_mouse_x += dx;
    g_mouse_y -= dy;
    if (dx != 0 || dy != 0) g_mouse_moved = 1;

    int sw = (int)screen_w();
    int sh = (int)screen_h();
    if (g_mouse_x < 0) g_mouse_x = 0;
    if (g_mouse_y < 0) g_mouse_y = 0;
    if (g_mouse_x >= sw) g_mouse_x = sw - 1;
    if (g_mouse_y >= sh) g_mouse_y = sh - 1;

    int left = (b0 & 1) ? 1 : 0;
    g_mouse_left = left;
    wm_mark_dirty();

    if (wheel != 0) handle_mouse_wheel(wheel);
}

static int titlebar_contains(wm_window_t *w, int x, int y) {
    return rect_contains(x, y, w->x + BORDER_PX, w->y + BORDER_PX, w->w - BORDER_PX * 2, TITLE_PX);
}

static int client_contains(wm_window_t *w, int x, int y) {
    return rect_contains(x, y, w->client_x, w->client_y, w->client_w, w->client_h);
}

static int window_scrollbar_rect(wm_window_t *w, int *out_x, int *out_y, int *out_w, int *out_h) {
    if (!w || !w->owner || !w->owner->console) return 0;
    int sb = console_scrollbar_width_px();
    if (sb <= 0 || w->client_w <= sb) return 0;
    int x = w->client_x + w->client_w - sb;
    int y = w->client_y;
    int h = w->client_h;
    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
    if (out_w) *out_w = sb;
    if (out_h) *out_h = h;
    return 1;
}

static void win_toggle_maximize(wm_window_t *w) {
    if (!w) return;
    int sw = (int)screen_w();
    int sh = (int)screen_h() - TASKBAR_PX;
    if (!w->maximized) {
        w->prev_x = w->x; w->prev_y = w->y; w->prev_w = w->w; w->prev_h = w->h;
        w->x = 6;
        w->y = 6;
        w->w = sw - 12;
        w->h = sh - 12;
        w->maximized = 1;
    } else {
        w->x = w->prev_x; w->y = w->prev_y; w->w = w->prev_w; w->h = w->prev_h;
        w->maximized = 0;
    }
    win_recompute_client(w);
    win_ensure_backbuffer(w);
    console_rebind_if_needed(w);
    win_post_expose(w);
    wm_mark_dirty();
}

static void win_minimize(wm_window_t *w) {
    if (!w) return;
    w->minimized = 1;
    if (g_focused == w) g_focused = NULL;
    wm_mark_dirty();
}

static void win_restore(wm_window_t *w) {
    if (!w) return;
    w->minimized = 0;
    wm_window_focus(w);
    wm_mark_dirty();
}

static void handle_click_taskbar(int x, int y) {
    int sh = (int)screen_h();
    int y0 = sh - TASKBAR_PX;
    if (y < y0) return;

    int start_w = 70;
    if (rect_contains(x, y, 6, y0 + 4, start_w, TASKBAR_PX - 8)) {
        g_start_open = !g_start_open;
        if (g_start_open) apps_registry_refresh();
        wm_mark_dirty();
        return;
    }

    int bx = 6 + start_w + 10;
    for (int i = 0; i < g_zcount; ++i) {
        wm_window_t *w = &g_windows[g_zorder[i]];
        if (!w->used) continue;
        int bw = 120;
        if (rect_contains(x, y, bx, y0 + 4, bw, TASKBAR_PX - 8)) {
            if (w->minimized) win_restore(w);
            else if (w == g_focused) win_minimize(w);
            else wm_window_focus(w);
            return;
        }
        bx += bw + 6;
    }
}

static void handle_click_start_menu(int x, int y) {
    if (!g_start_open) return;
    int sw = (int)screen_w();
    int sh = (int)screen_h();
    int menu_w = 220;
    int menu_h = 220;
    int x0 = 6;
    int y0 = sh - TASKBAR_PX - menu_h;
    if (!rect_contains(x, y, x0, y0, menu_w, menu_h)) {
        g_start_open = 0;
        wm_mark_dirty();
        return;
    }

    int pad = 10;
    int content_w = menu_w - pad * 2;
    int content_h = menu_h - pad * 2;
    int cell_h = ICON_DRAW_PX + 16 + 6;
    if (cell_h < ICON_DRAW_PX + 8) cell_h = ICON_DRAW_PX + 8;
    int cols = content_w / (ICON_DRAW_PX + 12);
    if (cols < 1) cols = 1;
    int cell_w = content_w / cols;
    if (cell_w < ICON_DRAW_PX) cell_w = ICON_DRAW_PX;
    int rows = content_h / cell_h;
    if (rows < 1) rows = 1;

    int count = 0;
    const mljos_app_descriptor_t *apps = apps_registry_list(&count);
    int col = 0;
    int row = 0;
    for (int i = 0; i < count; ++i) {
        if (!apps[i].supports_ui) continue;
        int cell_x = x0 + pad + col * cell_w;
        int cell_y = y0 + pad + row * cell_h;
        if (cell_y + cell_h > y0 + menu_h - pad) break;
        if (rect_contains(x, y, cell_x, cell_y, cell_w, cell_h)) {
            g_start_open = 0;
            wm_mark_dirty();
            set_launch_req(apps[i].name);
            return;
        }
        col++;
        if (col >= cols) { col = 0; row++; }
        if (row >= rows) break;
    }
}

static void handle_console_scrollbar_mouse_down(wm_window_t *w, int x, int y) {
    if (!w || !w->owner || !w->owner->console) return;
    int sb_x, sb_y, sb_w, sb_h;
    if (!window_scrollbar_rect(w, &sb_x, &sb_y, &sb_w, &sb_h)) return;
    if (!rect_contains(x, y, sb_x, sb_y, sb_w, sb_h)) return;

    int top = 0, visible = 0, total = 0;
    console_get_scroll_info(w->owner->console, &top, &visible, &total);
    if (visible <= 0 || total <= visible) return;

    int max_scroll = total - visible;
    int thumb_h = (sb_h * visible) / total;
    if (thumb_h < 16) thumb_h = 16;
    if (thumb_h > sb_h) thumb_h = sb_h;
    int thumb_y = sb_y + (int)((uint64_t)(sb_h - thumb_h) * (uint64_t)top / (uint64_t)max_scroll);

    if (rect_contains(x, y, sb_x, thumb_y, sb_w, thumb_h)) {
        g_scroll_dragging = 1;
        g_scroll_drag_win = w;
        g_scroll_drag_offset = y - thumb_y;
        return;
    }

    if (y < thumb_y) console_scroll_lines(w->owner->console, -visible);
    else console_scroll_lines(w->owner->console, visible);
}

static void handle_mouse_down(void) {
    int x = g_mouse_x;
    int y = g_mouse_y;

    // Taskbar / start menu get first dibs
    handle_click_start_menu(x, y);
    handle_click_taskbar(x, y);

    // Window hit
    wm_window_t *w = win_at(x, y);
    if (!w) return;
    wm_window_focus(w);

    // Buttons
    int by = w->y + BORDER_PX + (TITLE_PX - BTN_PX) / 2;
    int bx3 = w->x + w->w - BORDER_PX - BTN_PX;
    int bx2 = bx3 - BTN_PX - BTN_GAP_PX;
    int bx1 = bx2 - BTN_PX - BTN_GAP_PX;

    if (rect_contains(x, y, bx3, by, BTN_PX, BTN_PX)) {
        // Close (request): kill owner task and hide window. Window is reaped after task exits.
        w->close_requested = 1;
        if (w->owner) task_kill(w->owner);
        w->minimized = 1;
        wm_mark_dirty();
        return;
    }
    if (rect_contains(x, y, bx2, by, BTN_PX, BTN_PX)) {
        win_toggle_maximize(w);
        return;
    }
    if (rect_contains(x, y, bx1, by, BTN_PX, BTN_PX)) {
        win_minimize(w);
        return;
    }

    // Console scrollbar (if present)
    if (w->owner && w->owner->console) {
        int sb_x, sb_y, sb_w, sb_h;
        if (window_scrollbar_rect(w, &sb_x, &sb_y, &sb_w, &sb_h) &&
            rect_contains(x, y, sb_x, sb_y, sb_w, sb_h)) {
            handle_console_scrollbar_mouse_down(w, x, y);
            return;
        }
    }

    // Resize edges (4px)
    const int r = 4;
    int edges = 0;
    if (rect_contains(x, y, w->x, w->y, r, w->h)) edges |= 1;
    if (rect_contains(x, y, w->x + w->w - r, w->y, r, w->h)) edges |= 2;
    if (rect_contains(x, y, w->x, w->y, w->w, r)) edges |= 4;
    if (rect_contains(x, y, w->x, w->y + w->h - r, w->w, r)) edges |= 8;

    if (edges) {
        g_drag_mode = DRAG_RESIZE;
        g_drag_win = w;
        g_resize_edges = edges;
        g_drag_off_x = x;
        g_drag_off_y = y;
        return;
    }

    if (titlebar_contains(w, x, y)) {
        g_drag_mode = DRAG_MOVE;
        g_drag_win = w;
        g_drag_off_x = x - w->x;
        g_drag_off_y = y - w->y;
        return;
    }

    if (client_contains(w, x, y)) {
        win_post_mouse(w, MLJOS_UI_EVENT_MOUSE_LEFT_DOWN, x - w->client_x, y - w->client_y);
    }
}

static void handle_mouse_up(void) {
    if (g_scroll_dragging) {
        g_scroll_dragging = 0;
        g_scroll_drag_win = NULL;
        g_scroll_drag_offset = 0;
        return;
    }
    wm_window_t *w = g_drag_win;
    g_drag_mode = DRAG_NONE;
    g_drag_win = NULL;
    g_resize_edges = 0;
    if (w && w->used && !w->minimized && client_contains(w, g_mouse_x, g_mouse_y)) {
        win_post_mouse(w, MLJOS_UI_EVENT_MOUSE_LEFT_UP, g_mouse_x - w->client_x, g_mouse_y - w->client_y);
    }
}

static void handle_mouse_move(void) {
    if (!g_mouse_moved && !g_scroll_dragging && g_drag_mode == DRAG_NONE) return;
    if (g_scroll_dragging && g_scroll_drag_win) {
        wm_window_t *w = g_scroll_drag_win;
        int sb_x, sb_y, sb_w, sb_h;
        if (!window_scrollbar_rect(w, &sb_x, &sb_y, &sb_w, &sb_h)) return;

        int top = 0, visible = 0, total = 0;
        console_get_scroll_info(w->owner->console, &top, &visible, &total);
        if (visible <= 0 || total <= visible) return;

        int max_scroll = total - visible;
        int thumb_h = (sb_h * visible) / total;
        if (thumb_h < 16) thumb_h = 16;
        if (thumb_h > sb_h) thumb_h = sb_h;
        int max_thumb_y = sb_y + sb_h - thumb_h;
        int new_thumb_y = g_mouse_y - g_scroll_drag_offset;
        if (new_thumb_y < sb_y) new_thumb_y = sb_y;
        if (new_thumb_y > max_thumb_y) new_thumb_y = max_thumb_y;
        int new_top = 0;
        if (sb_h > thumb_h) {
            new_top = (int)((uint64_t)(new_thumb_y - sb_y) * (uint64_t)max_scroll / (uint64_t)(sb_h - thumb_h));
        }
        console_scroll_to(w->owner->console, new_top);
        return;
    }

    if (g_drag_mode == DRAG_NONE || !g_drag_win) {
        wm_window_t *w = win_at(g_mouse_x, g_mouse_y);
        if (w && client_contains(w, g_mouse_x, g_mouse_y)) {
            win_post_mouse(w, MLJOS_UI_EVENT_MOUSE_MOVE, g_mouse_x - w->client_x, g_mouse_y - w->client_y);
        }
        return;
    }
    wm_window_t *w = g_drag_win;
    if (!w->used) { g_drag_mode = DRAG_NONE; g_drag_win = NULL; return; }

    int sw = (int)screen_w();
    int sh = (int)screen_h() - TASKBAR_PX;

    if (g_drag_mode == DRAG_MOVE) {
        int nx = g_mouse_x - g_drag_off_x;
        int ny = g_mouse_y - g_drag_off_y;
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx + w->w > sw) nx = sw - w->w;
        if (ny + w->h > sh) ny = sh - w->h;
        w->x = nx;
        w->y = ny;
        win_recompute_client(w);
        wm_mark_dirty();
        win_post_expose(w);
        return;
    }

    if (g_drag_mode == DRAG_RESIZE) {
        int dx = g_mouse_x - g_drag_off_x;
        int dy = g_mouse_y - g_drag_off_y;
        g_drag_off_x = g_mouse_x;
        g_drag_off_y = g_mouse_y;

        int nx = w->x;
        int ny = w->y;
        int nw = w->w;
        int nh = w->h;

        if (g_resize_edges & 1) { nx += dx; nw -= dx; }
        if (g_resize_edges & 2) { nw += dx; }
        if (g_resize_edges & 4) { ny += dy; nh -= dy; }
        if (g_resize_edges & 8) { nh += dy; }

        if (nw < 160) nw = 160;
        if (nh < 120) nh = 120;
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx + nw > sw) nw = sw - nx;
        if (ny + nh > sh) nh = sh - ny;
        if (nw < 160) nw = 160;
        if (nh < 120) nh = 120;

        w->x = nx; w->y = ny; w->w = nw; w->h = nh;
        win_recompute_client(w);
        win_ensure_backbuffer(w);
        console_rebind_if_needed(w);
        win_post_expose(w);
        wm_mark_dirty();
        return;
    }
}

static void handle_mouse_wheel(int delta) {
    wm_window_t *w = win_at(g_mouse_x, g_mouse_y);
    if (!w || w->minimized) return;
    if (w->owner && w->owner->console && client_contains(w, g_mouse_x, g_mouse_y)) {
        int lines = delta * 3;
        console_scroll_lines(w->owner->console, -lines);
        return;
    }
    if (client_contains(w, g_mouse_x, g_mouse_y)) {
        mljos_ui_event_t ev;
        ev.type = MLJOS_UI_EVENT_MOUSE_WHEEL;
        ev.x = g_mouse_x - w->client_x;
        ev.y = g_mouse_y - w->client_y;
        ev.key = delta;
        (void)q_push(&w->q, &ev);
    }
}

void wm_pump_input(void) {
    // Drain available bytes from controller.
    while (inb(0x64) & 1) {
        uint8_t st = inb(0x64);
        uint8_t data = inb(0x60);
        if (st & 0x20) {
            mouse_push_byte(data);
        } else {
            int key = kbd_scancode_to_key(data);
            if (g_kbd_alt && (data & 0x7F) == 0x0F && !(data & 0x80)) { // Alt+Tab
                if (g_zcount > 1) {
                    int next_idx = -1;
                    for (int i = g_zcount - 2; i >= 0; --i) {
                        if (g_windows[g_zorder[i]].used && !g_windows[g_zorder[i]].minimized) {
                            next_idx = g_zorder[i];
                            break;
                        }
                    }
                    if (next_idx >= 0) {
                        z_focus_index(next_idx);
                        g_focused = &g_windows[next_idx];
                        wm_mark_dirty();
                    }
                }
            } else if (g_kbd_alt && (data & 0x7F) == 0x3E && !(data & 0x80)) { // Alt+F4
                if (g_focused) {
                    g_focused->close_requested = 1;
                }
            } else if (key && g_focused) {
                win_post_key(g_focused, key);
            }
        }
    }

    // Mouse transitions
    if (g_mouse_left && !g_mouse_left_prev) handle_mouse_down();
    if (!g_mouse_left && g_mouse_left_prev) handle_mouse_up();
    handle_mouse_move();
    
    g_mouse_left_prev = g_mouse_left;
    g_mouse_moved = 0;
}

void wm_compose_if_dirty(void) {
    if (!g_dirty) return;
    if (!fb_root.address) return;
    if (!g_gui_enabled) {
        g_dirty = 0;
        return;
    }

    wm_try_load_wallpaper();

    uint32_t *dst = g_backbuf ? g_backbuf : fb_root.address;
    uint32_t pitch = g_backbuf ? g_back_pitch : fb_root.pitch;
    int sw = (int)screen_w();
    int sh = (int)screen_h();

    if (g_wallpaper && g_wallpaper_w == sw && g_wallpaper_h == sh) {
        for (int y = 0; y < sh; ++y) {
            uint32_t *src_row = g_wallpaper + (uint64_t)y * (uint64_t)sw;
            uint32_t *dst_row = (uint32_t *)((uintptr_t)dst + (uintptr_t)y * pitch);
            for (int x = 0; x < sw; ++x) dst_row[x] = src_row[x];
        }
    } else {
        fill_rect(dst, pitch, sw, sh, 0, 0, sw, sh, COL_DESKTOP);
    }

    // Windows bottom->top
    for (int i = 0; i < g_zcount; ++i) {
        wm_window_t *w = &g_windows[g_zorder[i]];
        if (!w->used || w->minimized) continue;
        draw_window_frame(dst, pitch, sw, sh, w);
        blit_client(dst, pitch, sw, w);
        draw_console_scrollbar(dst, pitch, sw, sh, w);
    }

    draw_taskbar(dst, pitch, sw, sh);
    draw_start_menu(dst, pitch, sw, sh);
    draw_cursor(dst, pitch, sw, sh);

    // Present: copy backbuffer to the real framebuffer in one pass.
    if (g_backbuf && fb_root.address) {
        for (int y = 0; y < sh; ++y) {
            uint32_t *src_row = (uint32_t *)((uintptr_t)g_backbuf + (uintptr_t)y * g_back_pitch);
            uint32_t *dst_row = (uint32_t *)((uintptr_t)fb_root.address + (uintptr_t)y * fb_root.pitch);
            for (int x = 0; x < sw; ++x) dst_row[x] = src_row[x];
        }
    }

    g_dirty = 0;
}

void wm_reap_closed_windows(void) {
    for (int i = 0; i < WM_MAX_WINDOWS; ++i) {
        wm_window_t *w = &g_windows[i];
        if (!w->used) continue;
        if (!w->close_requested) continue;
        if (w->owner && w->owner->state == TASK_DEAD) {
            wm_window_destroy(w);
        }
    }
}

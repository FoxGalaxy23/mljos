#include "wm.h"

#include "apps_registry.h"
#include "console.h"
#include "font.h"
#include "io.h"
#include "kmem.h"
#include "kstring.h"
#include "rtc.h"
#include "task.h"

#define WM_MAX_WINDOWS 16
#define WM_MAX_EVENTS 64

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

static int g_dirty = 1;
static int g_start_open = 0;
static char g_launch_req[32];
static int g_has_launch_req = 0;

// Mouse
static int g_mouse_x = 10;
static int g_mouse_y = 10;
static int g_mouse_left = 0;
static int g_mouse_left_prev = 0;
static uint8_t g_mouse_pkt[3];
static int g_mouse_pkt_i = 0;

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

// Full-screen backbuffer to reduce flicker/tearing.
static uint32_t *g_backbuf = NULL;
static uint32_t g_back_pitch = 0;

static uint32_t screen_w(void) { return fb_root.width; }
static uint32_t screen_h(void) { return fb_root.height; }

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
    console_bind_target(w->owner->console, w->client_px, (uint32_t)w->client_w, (uint32_t)w->client_h, w->client_pitch);
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

    int y = y0 + 10;
    int count = 0;
    const mljos_app_descriptor_t *apps = apps_registry_list(&count);
    for (int i = 0; i < count; ++i) {
        if (!apps[i].supports_ui) continue;
        draw_text(dst, pitch, sw, sh, apps[i].title ? apps[i].title : apps[i].name, x0 + 10, y, 0xFFFFFF);
        y += 22;
        if (y > y0 + menu_h - 24) break;
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

void wm_init(void) {
    kmem_memset(g_windows, 0, sizeof(g_windows));
    g_zcount = 0;
    g_next_id = 1;
    g_focused = NULL;
    g_start_open = 0;
    g_has_launch_req = 0;
    g_drag_mode = DRAG_NONE;
    g_drag_win = NULL;
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
        kmem_memset(g_backbuf, 0, bytes);
    }
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
static int g_kbd_caps = 0;
static int g_kbd_extended = 0;

static void kbd_handle_mod(uint8_t sc) {
    if (sc == 0xE0) { g_kbd_extended = 1; return; }
    if (!g_kbd_extended) {
        if (sc == 0x2A || sc == 0x36) { g_kbd_shift = 1; return; }
        if (sc == 0xAA || sc == 0xB6) { g_kbd_shift = 0; return; }
        if (sc == 0x1D) { g_kbd_ctrl = 1; return; }
        if (sc == 0x9D) { g_kbd_ctrl = 0; return; }
        if (sc == 0x3A) { g_kbd_caps = !g_kbd_caps; return; }
        return;
    }
    if (sc == 0x1D) { g_kbd_ctrl = 1; g_kbd_extended = 0; return; }
    if (sc == 0x9D) { g_kbd_ctrl = 0; g_kbd_extended = 0; return; }
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

static void mouse_push_byte(uint8_t b) {
    // First byte must have bit3 set; helps resync on packet loss.
    if (g_mouse_pkt_i == 0 && (b & 0x08) == 0) return;
    g_mouse_pkt[g_mouse_pkt_i++] = b;
    if (g_mouse_pkt_i < 3) return;
    g_mouse_pkt_i = 0;

    uint8_t b0 = g_mouse_pkt[0];
    int dx = (int)((signed char)g_mouse_pkt[1]);
    int dy = (int)((signed char)g_mouse_pkt[2]);

    // Standard PS/2: dy is negative when moving up.
    (void)b0;
    g_mouse_x += dx;
    g_mouse_y -= dy;

    int sw = (int)screen_w();
    int sh = (int)screen_h();
    if (g_mouse_x < 0) g_mouse_x = 0;
    if (g_mouse_y < 0) g_mouse_y = 0;
    if (g_mouse_x >= sw) g_mouse_x = sw - 1;
    if (g_mouse_y >= sh) g_mouse_y = sh - 1;

    int left = (b0 & 1) ? 1 : 0;
    g_mouse_left_prev = g_mouse_left;
    g_mouse_left = left;
    wm_mark_dirty();
}

static int titlebar_contains(wm_window_t *w, int x, int y) {
    return rect_contains(x, y, w->x + BORDER_PX, w->y + BORDER_PX, w->w - BORDER_PX * 2, TITLE_PX);
}

static int client_contains(wm_window_t *w, int x, int y) {
    return rect_contains(x, y, w->client_x, w->client_y, w->client_w, w->client_h);
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

    int yy = y0 + 10;
    int count = 0;
    const mljos_app_descriptor_t *apps = apps_registry_list(&count);
    for (int i = 0; i < count; ++i) {
        if (!apps[i].supports_ui) continue;
        if (rect_contains(x, y, x0 + 6, yy - 2, menu_w - 12, 18)) {
            g_start_open = 0;
            wm_mark_dirty();
            set_launch_req(apps[i].name);
            return;
        }
        yy += 22;
        if (yy > y0 + menu_h - 24) break;
    }
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
    wm_window_t *w = g_drag_win;
    g_drag_mode = DRAG_NONE;
    g_drag_win = NULL;
    g_resize_edges = 0;
    if (w && w->used && !w->minimized && client_contains(w, g_mouse_x, g_mouse_y)) {
        win_post_mouse(w, MLJOS_UI_EVENT_MOUSE_LEFT_UP, g_mouse_x - w->client_x, g_mouse_y - w->client_y);
    }
}

static void handle_mouse_move(void) {
    if (g_drag_mode == DRAG_NONE || !g_drag_win) return;
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

void wm_pump_input(void) {
    // Drain available bytes from controller.
    while (inb(0x64) & 1) {
        uint8_t st = inb(0x64);
        uint8_t data = inb(0x60);
        if (st & 0x20) {
            mouse_push_byte(data);
        } else {
            int key = kbd_scancode_to_key(data);
            if (key && g_focused) win_post_key(g_focused, key);
        }
    }

    // Mouse transitions
    if (g_mouse_left && !g_mouse_left_prev) handle_mouse_down();
    if (!g_mouse_left && g_mouse_left_prev) handle_mouse_up();
    handle_mouse_move();
}

void wm_compose_if_dirty(void) {
    if (!g_dirty) return;
    if (!fb_root.address) return;

    uint32_t *dst = g_backbuf ? g_backbuf : fb_root.address;
    uint32_t pitch = g_backbuf ? g_back_pitch : fb_root.pitch;
    int sw = (int)screen_w();
    int sh = (int)screen_h();

    fill_rect(dst, pitch, sw, sh, 0, 0, sw, sh, COL_DESKTOP);

    // Windows bottom->top
    for (int i = 0; i < g_zcount; ++i) {
        wm_window_t *w = &g_windows[g_zorder[i]];
        if (!w->used || w->minimized) continue;
        draw_window_frame(dst, pitch, sw, sh, w);
        blit_client(dst, pitch, sw, w);
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

#include "gui.h"
#include "console.h"
#include "rtc.h"
#include "font.h"
#include "kstring.h"
#include "io.h"
#include "apps_registry.h"
#include "shell.h"
#include "users.h"
#include "fs.h"

// Используем тот же моноширинный 8x16 шрифт.
static const int CHAR_WIDTH = 8;
static const int CHAR_HEIGHT = 16;

static int g_in_dos_mode = 0;
static int g_terminal_minimized = 0;
static int g_terminal_closed = 0;

// Геометрия окна в символах (только в GUI-режиме).
static int g_win_left = 0;
static int g_win_top = 0;
static int g_win_cols = 0;
static int g_win_rows = 0;

// Таскбар в символах (сейчас 1 строка как в демо-прототипе).
static const int g_taskbar_rows_chars = 1;

static uint8_t g_last_clock_sec = 255;

// Start menu state
static int g_start_menu_open = 0;

// Desktop icons and context menu
#define MAX_DESKTOP_ICONS 64
typedef struct {
    char name[32];
    int x, y, w, h;
    int is_dir;
} desktop_icon_t;

static desktop_icon_t g_desktop_icons[MAX_DESKTOP_ICONS];
static int g_desktop_icon_count = 0;

static int g_desktop_context_menu_open = 0;
static int g_desktop_context_menu_x = 0;
static int g_desktop_context_menu_y = 0;
static char g_desktop_path[128] = {0};

// Цвета (0xRRGGBB).
static const uint32_t DESKTOP_BG = 0x202020;
static const uint32_t TASKBAR_BG = 0x303030;
static const uint32_t TASKBAR_BTN_BG = 0x404040;
static const uint32_t TASKBAR_BTN_ACTIVE_BG = 0x606060;
static const uint32_t TITLE_BG = 0x000080;
static const uint32_t TITLE_FG = 0xFFFFFF;
static const uint32_t WINDOW_BG = 0x111111;

// Mouse state (PS/2 3-byte packets)
static int g_mouse_x_px = 0;
static int g_mouse_y_px = 0;
static int g_mouse_left = 0;
static int g_mouse_left_prev = 0;
static int g_mouse_right = 0;
static int g_mouse_right_prev = 0;
static int g_mouse_left_pressed_latch = 0;
static int g_mouse_left_released_latch = 0;
static int g_ui_expose_latch = 0;
static int g_dragging = 0;
static int g_drag_offset_x_px = 0;
static int g_drag_offset_y_px = 0;

static uint8_t g_mouse_packet[3];
static int g_mouse_packet_idx = 0;

// Simple software cursor: save/restore pixels under it.
#define CURSOR_W 10
#define CURSOR_H 16
static uint32_t g_cursor_saved[CURSOR_W * CURSOR_H];
static int g_cursor_saved_valid = 0;
static int g_cursor_saved_x = 0;
static int g_cursor_saved_y = 0;

static void gui_cursor_restore(void) {
    if (!fb_root.address) return;
    if (!g_cursor_saved_valid) return;

    int x0 = g_cursor_saved_x;
    int y0 = g_cursor_saved_y;

    if (x0 < 0 || y0 < 0 || x0 >= (int)fb_root.width || y0 >= (int)fb_root.height) {
        g_cursor_saved_valid = 0;
        return;
    }

    int w = CURSOR_W;
    int h = CURSOR_H;
    if (x0 + w > (int)fb_root.width) w = (int)fb_root.width - x0;
    if (y0 + h > (int)fb_root.height) h = (int)fb_root.height - y0;
    if (w <= 0 || h <= 0) {
        g_cursor_saved_valid = 0;
        return;
    }

    for (int y = 0; y < h; ++y) {
        uint32_t *row = (uint32_t*)((uintptr_t)fb_root.address + (y0 + y) * fb_root.pitch);
        for (int x = 0; x < w; ++x) {
            row[x0 + x] = g_cursor_saved[y * CURSOR_W + x];
        }
    }

    g_cursor_saved_valid = 0;
}

static void gui_cursor_draw_at(int x_px, int y_px) {
    if (!fb_root.address) return;
    if (g_in_dos_mode) return;

    // Clamp top-left to screen
    if (x_px < 0) x_px = 0;
    if (y_px < 0) y_px = 0;
    if (x_px >= (int)fb_root.width) x_px = (int)fb_root.width - 1;
    if (y_px >= (int)fb_root.height) y_px = (int)fb_root.height - 1;

    int w = CURSOR_W;
    int h = CURSOR_H;
    if (x_px + w > (int)fb_root.width) w = (int)fb_root.width - x_px;
    if (y_px + h > (int)fb_root.height) h = (int)fb_root.height - y_px;
    if (w <= 0 || h <= 0) return;

    // Save background under cursor.
    for (int y = 0; y < h; ++y) {
        uint32_t *row = (uint32_t*)((uintptr_t)fb_root.address + (y_px + y) * fb_root.pitch);
        for (int x = 0; x < w; ++x) {
            g_cursor_saved[y * CURSOR_W + x] = row[x_px + x];
        }
        for (int x = w; x < CURSOR_W; ++x) g_cursor_saved[y * CURSOR_W + x] = 0;
    }
    for (int y = h; y < CURSOR_H; ++y) {
        for (int x = 0; x < CURSOR_W; ++x) g_cursor_saved[y * CURSOR_W + x] = 0;
    }

    g_cursor_saved_x = x_px;
    g_cursor_saved_y = y_px;
    g_cursor_saved_valid = 1;

    // Draw a simple arrow cursor with black outline + white fill.
    // Shape defined as per-row width (in pixels).
    static const uint8_t shape[CURSOR_H] = {
        1, 2, 3, 4, 5, 6, 7, 8,
        5, 5, 5, 3, 3, 2, 2, 1
    };

    for (int y = 0; y < h; ++y) {
        int sw = shape[y];
        if (sw > w) sw = w;
        uint32_t *row = (uint32_t*)((uintptr_t)fb_root.address + (y_px + y) * fb_root.pitch);
        for (int x = 0; x < sw; ++x) {
            // Outline on edges, fill inside.
            uint32_t col = (x == 0 || x == sw - 1 || y == 0) ? 0x000000 : 0xFFFFFF;
            row[x_px + x] = col;
        }
        // Extra outline pixel to the right to improve visibility on white borders.
        if (sw < w) row[x_px + sw] = 0x000000;
    }
}

static uint32_t fb_cols_root(void) {
    return fb_root.width / CHAR_WIDTH;
}

static uint32_t fb_rows_root(void) {
    return fb_root.height / CHAR_HEIGHT;
}

static int rect_contains(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

static void gui_fill_rect(int x_px, int y_px, int w_px, int h_px, uint32_t color) {
    if (!fb_root.address) return;

    if (x_px < 0) { w_px += x_px; x_px = 0; }
    if (y_px < 0) { h_px += y_px; y_px = 0; }
    if (w_px <= 0 || h_px <= 0) return;
    if (x_px + w_px > (int)fb_root.width) w_px = (int)fb_root.width - x_px;
    if (y_px + h_px > (int)fb_root.height) h_px = (int)fb_root.height - y_px;

    for (int y = y_px; y < y_px + h_px; ++y) {
        uint32_t *row = (uint32_t*)((uintptr_t)fb_root.address + y * fb_root.pitch);
        for (int x = x_px; x < x_px + w_px; ++x) row[x] = color;
    }
}

static void gui_draw_text_transparent(const char *s, int x_px, int y_px, uint32_t color);
static void gui_draw_char_transparent(char ch, int x_px, int y_px, uint32_t color);
static void gui_draw_desktop(int redraw_console);

// Active app window (single app at a time for now).
static int g_app_active = 0;
static int g_app_minimized = 0;
static int g_app_closed = 1;
static int g_app_left = 0;
static int g_app_top = 0;
static int g_app_cols = 0;
static int g_app_rows = 0;
static char g_app_title[32];

// App client rect (pixels) used for UI API coordinate space.
static int g_app_client_x_px = 0;
static int g_app_client_y_px = 0;
static int g_app_client_w_px = 0;
static int g_app_client_h_px = 0;

static int g_app_dragging = 0;
static int g_app_drag_offset_x_px = 0;
static int g_app_drag_offset_y_px = 0;

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void gui_app_recompute_client_rect(void) {
    int outer_x_px = g_app_left * CHAR_WIDTH;
    int outer_y_px = g_app_top * CHAR_HEIGHT;
    int outer_w_px = g_app_cols * CHAR_WIDTH;
    int outer_h_px = g_app_rows * CHAR_HEIGHT;

    g_app_client_x_px = outer_x_px + CHAR_WIDTH;
    g_app_client_y_px = outer_y_px + CHAR_HEIGHT;
    g_app_client_w_px = outer_w_px - 2 * CHAR_WIDTH;
    g_app_client_h_px = outer_h_px - 2 * CHAR_HEIGHT;

    if (g_app_client_w_px < 1) g_app_client_w_px = 1;
    if (g_app_client_h_px < 1) g_app_client_h_px = 1;
}

static void gui_app_draw_frame(void) {
    if (!g_app_active || g_app_minimized || g_app_closed) return;

    int outer_x_px = g_app_left * CHAR_WIDTH;
    int outer_y_px = g_app_top * CHAR_HEIGHT;
    int outer_w_px = g_app_cols * CHAR_WIDTH;
    int outer_h_px = g_app_rows * CHAR_HEIGHT;

    gui_fill_rect(outer_x_px, outer_y_px, outer_w_px, outer_h_px, WINDOW_BG);
    gui_fill_rect(outer_x_px, outer_y_px, outer_w_px, CHAR_HEIGHT, TITLE_BG);
    gui_fill_rect(outer_x_px, outer_y_px, CHAR_WIDTH, outer_h_px, 0xFFFFFF);
    gui_fill_rect(outer_x_px + outer_w_px - CHAR_WIDTH, outer_y_px, CHAR_WIDTH, outer_h_px, 0xFFFFFF);
    gui_fill_rect(outer_x_px, outer_y_px + outer_h_px - CHAR_HEIGHT, outer_w_px, CHAR_HEIGHT, 0xFFFFFF);

    int title_text_x_px = outer_x_px + CHAR_WIDTH + 2;
    gui_draw_text_transparent(g_app_title[0] ? g_app_title : "App", title_text_x_px, outer_y_px, TITLE_FG);

    // Title buttons.
    int btn_w_px = 2 * CHAR_WIDTH;
    int btn_right_x = outer_x_px + outer_w_px - 2 * btn_w_px - 1;
    gui_fill_rect(btn_right_x, outer_y_px, btn_w_px, CHAR_HEIGHT, TASKBAR_BTN_BG);
    gui_fill_rect(btn_right_x + btn_w_px + 1, outer_y_px, btn_w_px, CHAR_HEIGHT, TASKBAR_BTN_BG);
    gui_draw_char_transparent('-', btn_right_x + CHAR_WIDTH / 2, outer_y_px + 4, TITLE_FG);
    gui_draw_char_transparent('X', btn_right_x + btn_w_px + 1 + CHAR_WIDTH / 2, outer_y_px + 4, TITLE_FG);
}

static int gui_app_outer_contains(int x, int y) {
    if (!g_app_active || g_app_minimized || g_app_closed) return 0;
    int ox = g_app_left * CHAR_WIDTH;
    int oy = g_app_top * CHAR_HEIGHT;
    int ow = g_app_cols * CHAR_WIDTH;
    int oh = g_app_rows * CHAR_HEIGHT;
    return rect_contains(x, y, ox, oy, ow, oh);
}

static int gui_app_client_contains(int x, int y) {
    if (!g_app_active || g_app_minimized || g_app_closed) return 0;
    return rect_contains(x, y, g_app_client_x_px, g_app_client_y_px, g_app_client_w_px, g_app_client_h_px);
}

static void gui_app_close(void) {
    g_app_active = 0;
    g_app_minimized = 0;
    g_app_closed = 1;
    g_app_dragging = 0;
    gui_draw_desktop(1);
}

static void gui_app_minimize(void) {
    g_app_minimized = 1;
    gui_draw_desktop(1);
}

static void gui_app_show(void) {
    g_app_minimized = 0;
    g_app_closed = 0;
    g_app_active = 1;
    gui_draw_desktop(1);
    gui_app_draw_frame();
    gui_app_recompute_client_rect();
    gui_fill_rect(g_app_client_x_px, g_app_client_y_px, g_app_client_w_px, g_app_client_h_px, 0x000000);
    g_ui_expose_latch = 1;
    // Cursor on top
    g_cursor_saved_valid = 0;
    gui_cursor_draw_at(g_mouse_x_px, g_mouse_y_px);
}

uint32_t gui_ui_screen_w(void) { return (uint32_t)g_app_client_w_px; }
uint32_t gui_ui_screen_h(void) { return (uint32_t)g_app_client_h_px; }

static void gui_ui_fill_rect_clipped(int x, int y, int w, int h, uint32_t rgb) {
    // Clip to client area, coordinates are client-relative.
    if (w <= 0 || h <= 0) return;
    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_app_client_w_px) x1 = g_app_client_w_px;
    if (y1 > g_app_client_h_px) y1 = g_app_client_h_px;
    if (x1 <= x0 || y1 <= y0) return;
    gui_fill_rect(g_app_client_x_px + x0, g_app_client_y_px + y0, x1 - x0, y1 - y0, rgb);
}

void gui_ui_fill_rect(int x, int y, int w, int h, uint32_t rgb) {
    if (!g_app_active || g_app_minimized || g_app_closed) return;
    gui_ui_fill_rect_clipped(x, y, w, h, rgb);
}

void gui_ui_draw_text(const char *s, int x, int y, uint32_t rgb) {
    if (!g_app_active || g_app_minimized || g_app_closed) return;
    // Basic clip: skip if baseline is outside.
    if (y < -CHAR_HEIGHT || y >= g_app_client_h_px) return;
    if (!s) return;
    gui_draw_text_transparent(s, g_app_client_x_px + x, g_app_client_y_px + y, rgb);
}

void gui_ui_begin_app(const char *title) {
    if (g_in_dos_mode) g_in_dos_mode = 0;
    g_start_menu_open = 0;
    g_terminal_minimized = 1;
    g_terminal_closed = 0;

    // Init app window geometry similar to terminal.
    int total_cols = (int)fb_cols_root();
    int total_rows = (int)fb_rows_root() - g_taskbar_rows_chars;
    int desired_cols = total_cols > 70 ? 70 : (total_cols > 20 ? total_cols - 4 : total_cols);
    int desired_rows = total_rows > 28 ? 28 : (total_rows > 10 ? total_rows - 4 : total_rows);
    if (desired_cols < 20) desired_cols = total_cols;
    if (desired_rows < 8) desired_rows = total_rows;
    if (desired_cols < 6) desired_cols = 6;
    if (desired_rows < 6) desired_rows = 6;

    g_app_cols = desired_cols;
    g_app_rows = desired_rows;
    g_app_left = total_cols > g_app_cols ? (total_cols - g_app_cols) / 2 : 0;
    g_app_top = total_rows > g_app_rows ? 1 : 0;

    // Title
    g_app_title[0] = '\0';
    if (title) {
        int i = 0;
        while (title[i] && i < (int)sizeof(g_app_title) - 1) { g_app_title[i] = title[i]; i++; }
        g_app_title[i] = '\0';
    }

    g_app_active = 1;
    g_app_minimized = 0;
    g_app_closed = 0;
    g_app_dragging = 0;

    gui_draw_desktop(1);
    gui_app_recompute_client_rect();
    gui_app_draw_frame();
    gui_fill_rect(g_app_client_x_px, g_app_client_y_px, g_app_client_w_px, g_app_client_h_px, 0x000000);
    g_ui_expose_latch = 1;
}

void gui_ui_end_app(void) {
    gui_app_close();
    g_terminal_minimized = 0;
    g_terminal_closed = 0;
    gui_draw_desktop(1);
}

int gui_ui_mouse_x(void) { return g_mouse_x_px - g_app_client_x_px; }
int gui_ui_mouse_y(void) { return g_mouse_y_px - g_app_client_y_px; }
int gui_ui_consume_left_pressed(void) { int v = g_mouse_left_pressed_latch; g_mouse_left_pressed_latch = 0; return v; }
int gui_ui_consume_left_released(void) { int v = g_mouse_left_released_latch; g_mouse_left_released_latch = 0; return v; }
int gui_ui_consume_expose(void) { int v = g_ui_expose_latch; g_ui_expose_latch = 0; return v; }

void gui_force_redraw(void) {
    if (g_in_dos_mode) return;
    gui_draw_desktop(1);
    if (g_app_active && !g_app_minimized && !g_app_closed) {
        gui_app_recompute_client_rect();
        gui_app_draw_frame();
        g_ui_expose_latch = 1;
        g_cursor_saved_valid = 0;
        gui_cursor_draw_at(g_mouse_x_px, g_mouse_y_px);
    }
}

static void gui_draw_char_transparent(char ch, int x_px, int y_px, uint32_t color) {
    if (!fb_root.address) return;
    if ((unsigned char)ch >= 256) return;

    const uint8_t *glyph = font8x16[(uint8_t)ch];
    for (int i = 0; i < CHAR_HEIGHT; ++i) {
        uint8_t bits = glyph[i];
        uint32_t *pixel_row = (uint32_t*)((uintptr_t)fb_root.address + (y_px + i) * fb_root.pitch + x_px * 4);
        for (int j = 0; j < CHAR_WIDTH; ++j) {
            if (bits & (1 << (7 - j))) pixel_row[j] = color;
        }
    }
}

static void gui_draw_text_transparent(const char *s, int x_px, int y_px, uint32_t color) {
    if (!s) return;
    int x = x_px;
    while (*s) {
        gui_draw_char_transparent(*s++, x, y_px, color);
        x += CHAR_WIDTH;
    }
}

static void gui_apply_console_viewport(int redraw_console) {
    if (g_in_dos_mode) {
        console_set_viewport(0, 0, fb_root.width, fb_root.height);
        console_set_visible(NULL, 1);
        if (redraw_console) console_redraw(NULL);
        return;
    }

    // Интерьер окна (без рамки и title bar).
    int inner_left_chars = g_win_left + 1;
    int inner_top_chars = g_win_top + 1;
    int inner_cols = g_win_cols - 2;
    int inner_rows = g_win_rows - 2;

    int inner_left_px = inner_left_chars * CHAR_WIDTH;
    int inner_top_px = inner_top_chars * CHAR_HEIGHT;
    int inner_w_px = inner_cols * CHAR_WIDTH;
    int inner_h_px = inner_rows * CHAR_HEIGHT;

    console_set_viewport((uint32_t)inner_left_px, (uint32_t)inner_top_px, (uint32_t)inner_w_px, (uint32_t)inner_h_px);

    int should_show = !g_terminal_minimized && !g_terminal_closed;
    console_set_visible(NULL, should_show);
    if (redraw_console && should_show) console_redraw(NULL);
}

static void gui_draw_window_frame(void) {
    // Внешняя рамка окна в пикселях.
    int outer_x_px = g_win_left * CHAR_WIDTH;
    int outer_y_px = g_win_top * CHAR_HEIGHT;
    int outer_w_px = g_win_cols * CHAR_WIDTH;
    int outer_h_px = g_win_rows * CHAR_HEIGHT;

    // Фон окна.
    gui_fill_rect(outer_x_px, outer_y_px, outer_w_px, outer_h_px, WINDOW_BG);
    // Title bar.
    gui_fill_rect(outer_x_px, outer_y_px, outer_w_px, CHAR_HEIGHT, TITLE_BG);
    // Левые/правые бордеры (толщина 1 символ).
    gui_fill_rect(outer_x_px, outer_y_px, CHAR_WIDTH, outer_h_px, 0xFFFFFF);
    gui_fill_rect(outer_x_px + outer_w_px - CHAR_WIDTH, outer_y_px, CHAR_WIDTH, outer_h_px, 0xFFFFFF);
    // Нижний бордер.
    gui_fill_rect(outer_x_px, outer_y_px + outer_h_px - CHAR_HEIGHT, outer_w_px, CHAR_HEIGHT, 0xFFFFFF);

    // Название.
    int title_text_x_px = outer_x_px + CHAR_WIDTH + 2;
    int title_text_y_px = outer_y_px + 0;
    gui_draw_text_transparent("Terminal", title_text_x_px, title_text_y_px, TITLE_FG);

    // Заглушки кнопок (не интерактивны).
    int btn_w_px = 2 * CHAR_WIDTH;
    int btn_h_px = CHAR_HEIGHT;
    int btn_y = outer_y_px;
    int btn_right_x = outer_x_px + outer_w_px - 2 * btn_w_px - 1;
    gui_fill_rect(btn_right_x, btn_y, btn_w_px, btn_h_px, TASKBAR_BTN_BG);
    gui_fill_rect(btn_right_x + btn_w_px + 1, btn_y, btn_w_px, btn_h_px, TASKBAR_BTN_BG);
    gui_draw_char_transparent('-', btn_right_x + CHAR_WIDTH / 2, btn_y + 4, TITLE_FG);
    gui_draw_char_transparent('X', btn_right_x + btn_w_px + 1 + CHAR_WIDTH / 2, btn_y + 4, TITLE_FG);
}

static void gui_draw_taskbar(void) {
    int taskbar_h_px = g_taskbar_rows_chars * CHAR_HEIGHT;
    int taskbar_y_px = (int)fb_root.height - taskbar_h_px;

    gui_fill_rect(0, taskbar_y_px, (int)fb_root.width, taskbar_h_px, TASKBAR_BG);

    // Start button.
    int start_btn_cols = 6;
    int start_w_px = start_btn_cols * CHAR_WIDTH;
    gui_fill_rect(0, taskbar_y_px, start_w_px, taskbar_h_px, g_start_menu_open ? TASKBAR_BTN_ACTIVE_BG : TASKBAR_BTN_BG);
    gui_draw_text_transparent("Pusk", CHAR_WIDTH, taskbar_y_px, TITLE_FG);

    // "Running windows": Terminal + active app.
    int btn_x_px = start_w_px + CHAR_WIDTH;
    int btn_w_px = 10 * CHAR_WIDTH;

    // Terminal button
    {
        uint32_t btn_bg = (!g_terminal_minimized && !g_terminal_closed) ? TASKBAR_BTN_ACTIVE_BG : TASKBAR_BTN_BG;
        gui_fill_rect(btn_x_px, taskbar_y_px + 2, btn_w_px, taskbar_h_px - 4, btn_bg);
        gui_draw_text_transparent("Terminal", btn_x_px + CHAR_WIDTH / 2, taskbar_y_px + 1, TITLE_FG);
        btn_x_px += btn_w_px + CHAR_WIDTH;
    }

    // App button (if any)
    if (g_app_active && !g_app_closed) {
        uint32_t btn_bg = (!g_app_minimized) ? TASKBAR_BTN_ACTIVE_BG : TASKBAR_BTN_BG;
        gui_fill_rect(btn_x_px, taskbar_y_px + 2, btn_w_px, taskbar_h_px - 4, btn_bg);
        gui_draw_text_transparent(g_app_title[0] ? g_app_title : "App", btn_x_px + CHAR_WIDTH / 2, taskbar_y_px + 1, TITLE_FG);
    }

    // Clock.
    uint8_t hh = 0, mm = 0, ss = 0;
    get_rtc_time(&hh, &mm, &ss);
    char clock[6];
    clock[0] = '0' + (hh / 10) % 10;
    clock[1] = '0' + (hh % 10);
    clock[2] = ':';
    clock[3] = '0' + (mm / 10) % 10;
    clock[4] = '0' + (mm % 10);
    clock[5] = '\0';

    int clock_len = 5; // HH:MM
    int clock_w_px = clock_len * CHAR_WIDTH;
    int clock_x_px = (int)fb_root.width - clock_w_px - CHAR_WIDTH;
    gui_draw_text_transparent(clock, clock_x_px, taskbar_y_px, TITLE_FG);
}

static void gui_draw_start_menu(void) {
    if (!g_start_menu_open) return;

    int taskbar_h_px = g_taskbar_rows_chars * CHAR_HEIGHT;
    int taskbar_y_px = (int)fb_root.height - taskbar_h_px;

    int item_h_px = CHAR_HEIGHT;
    int menu_w_cols = 22;
    int menu_w_px = menu_w_cols * CHAR_WIDTH;

    int count = 0;
    const mljos_app_descriptor_t *apps = apps_registry_list(&count);
    int gui_count = 0;
    for (int i = 0; i < count; ++i) if (apps[i].supports_ui) gui_count++;
    if (gui_count < 1) gui_count = 1;

    int menu_h_px = (gui_count + 1) * item_h_px; // + header row
    int menu_x_px = 0;
    int menu_y_px = taskbar_y_px - menu_h_px;
    if (menu_y_px < 0) menu_y_px = 0;

    gui_fill_rect(menu_x_px, menu_y_px, menu_w_px, menu_h_px, 0x101010);
    gui_fill_rect(menu_x_px, menu_y_px, menu_w_px, item_h_px, 0x202020);
    gui_draw_text_transparent("Applications", menu_x_px + CHAR_WIDTH, menu_y_px, TITLE_FG);

    int row = 0;
    for (int i = 0; i < count; ++i) {
        if (!apps[i].supports_ui) continue;
        int y = menu_y_px + (row + 1) * item_h_px;
        const char *label = apps[i].title ? apps[i].title : apps[i].name;
        gui_draw_text_transparent(label, menu_x_px + CHAR_WIDTH, y, TITLE_FG);
        row++;
    }
}

static void gui_draw_desktop(int redraw_console);

static void gui_terminal_show(void) {
    g_terminal_minimized = 0;
    g_terminal_closed = 0;
    gui_draw_desktop(1);
}

static void gui_terminal_minimize(void) {
    g_terminal_minimized = 1;
    g_terminal_closed = 0;
    gui_draw_desktop(1);
}

static void gui_terminal_close(void) {
    g_terminal_minimized = 0;
    g_terminal_closed = 1;
    gui_draw_desktop(1);
}

static void gui_terminal_taskbar_click(void) {
    if (g_terminal_minimized || g_terminal_closed) gui_terminal_show();
    else gui_terminal_minimize();
}

static void gui_window_title_click_or_drag(void) {
    if (g_terminal_minimized || g_terminal_closed) return;

    // Title bar / drag area.
    int outer_x_px = g_win_left * CHAR_WIDTH;
    int outer_y_px = g_win_top * CHAR_HEIGHT;
    int outer_w_px = g_win_cols * CHAR_WIDTH;
    int outer_h_px = g_win_rows * CHAR_HEIGHT;
    int title_h_px = CHAR_HEIGHT;

    // Buttons in title bar (same geometry as draw_window_frame()).
    int btn_w_px = 2 * CHAR_WIDTH;
    int btn_y = outer_y_px;
    int btn_right_x = outer_x_px + outer_w_px - 2 * btn_w_px - 1;
    int btn_min_x = btn_right_x;
    int btn_x_x = btn_right_x + btn_w_px + 1;

    // 1) Check minimize button.
    if (rect_contains(g_mouse_x_px, g_mouse_y_px, btn_min_x, btn_y, btn_w_px, CHAR_HEIGHT)) {
        gui_terminal_minimize();
        return;
    }
    // 2) Check close button.
    if (rect_contains(g_mouse_x_px, g_mouse_y_px, btn_x_x, btn_y, btn_w_px, CHAR_HEIGHT)) {
        gui_terminal_close();
        return;
    }

    // 3) Start dragging if click is inside title bar.
    if (rect_contains(g_mouse_x_px, g_mouse_y_px, outer_x_px, outer_y_px, outer_w_px, title_h_px)) {
        g_dragging = 1;
        g_drag_offset_x_px = g_mouse_x_px - outer_x_px;
        g_drag_offset_y_px = g_mouse_y_px - outer_y_px;
    }
}

static void gui_drag_update(void) {
    if (!g_dragging) return;
    if (g_mouse_left == 0) return;
    if (g_terminal_minimized || g_terminal_closed) {
        g_dragging = 0;
        return;
    }

    int total_cols = (int)fb_cols_root();
    int total_rows_avail = (int)fb_rows_root() - g_taskbar_rows_chars;

    int outer_x_px_new = g_mouse_x_px - g_drag_offset_x_px;
    int outer_y_px_new = g_mouse_y_px - g_drag_offset_y_px;

    int new_left_chars = outer_x_px_new / CHAR_WIDTH;
    int new_top_chars = outer_y_px_new / CHAR_HEIGHT;

    int max_left = total_cols - g_win_cols;
    int max_top = total_rows_avail - g_win_rows;

    if (max_left < 0) max_left = 0;
    if (max_top < 0) max_top = 0;

    if (new_left_chars < 0) new_left_chars = 0;
    if (new_top_chars < 0) new_top_chars = 0;
    if (new_left_chars > max_left) new_left_chars = max_left;
    if (new_top_chars > max_top) new_top_chars = max_top;

    if (new_left_chars != g_win_left || new_top_chars != g_win_top) {
        g_win_left = new_left_chars;
        g_win_top = new_top_chars;
        gui_draw_desktop(1);
    }
}

static void gui_refresh_desktop_icons(void) {
    g_desktop_icon_count = 0;
    const user_account_t *u = users_current();
    if (!u) return;

    int i = 0;
    while (u->home[i] && i < sizeof(g_desktop_path) - 10) {
        g_desktop_path[i] = u->home[i];
        i++;
    }
    const char *d = "/desktop";
    int j = 0;
    while (d[j]) {
        g_desktop_path[i++] = d[j++];
    }
    g_desktop_path[i] = '\0';

    os_api.mkdir(g_desktop_path);

    static int desktop_script_created = 0;
    if (!desktop_script_created) {
        desktop_script_created = 1;
        char script_path[256];
        int k = 0;
        while (g_desktop_path[k] && k < 200) { script_path[k] = g_desktop_path[k]; k++; }
        const char *s_ext = "/Files.scri";
        int m = 0;
        while (s_ext[m]) { script_path[k++] = s_ext[m++]; }
        script_path[k] = '\0';
        
        char dummy_buf[2];
        unsigned int out_sz = 0;
        if (!os_api.read_file(script_path, dummy_buf, 1, &out_sz)) {
            const char *content = "open files\n";
            int c_len = 0;
            while(content[c_len]) c_len++;
            os_api.write_file(script_path, content, c_len);
        }
    }

    char buf[4096];
    if (!os_api.list_dir(g_desktop_path, buf, sizeof(buf))) return;

    int x = 20, y = 20;
    int item_h = 48;
    int item_w = 64;

    char *p = buf;
    while (*p && g_desktop_icon_count < MAX_DESKTOP_ICONS) {
        int is_dot = (p[0] == '.' && p[1] == '\0');
        int is_dotdot = (p[0] == '.' && p[1] == '.' && p[2] == '\0');
        if (!is_dot && !is_dotdot) {
            desktop_icon_t *icon = &g_desktop_icons[g_desktop_icon_count];
            int n = 0;
            while (p[n] && n < 31) { icon->name[n] = p[n]; n++; }
            icon->name[n] = '\0';
            
            icon->x = x;
            icon->y = y;
            icon->w = item_w;
            icon->h = item_h;
            
            char test_path[256];
            int k = 0;
            while (g_desktop_path[k] && k < 250) { test_path[k] = g_desktop_path[k]; k++; }
            test_path[k++] = '/';
            n = 0;
            while (icon->name[n] && k < 255) { test_path[k++] = icon->name[n++]; }
            test_path[k] = '\0';
            char dummy[2];
            icon->is_dir = os_api.list_dir(test_path, dummy, sizeof(dummy));

            g_desktop_icon_count++;
            y += item_h + 20;
            if (y + item_h > (int)fb_root.height - 40) {
                y = 20;
                x += item_w + 20;
            }
        }
        while (*p) p++;
        p++;
    }
}

static void gui_draw_desktop_icons_ui(void) {
    for (int i = 0; i < g_desktop_icon_count; i++) {
        desktop_icon_t *icon = &g_desktop_icons[i];
        uint32_t col = icon->is_dir ? 0xD4A373 : 0xEDEDED;
        gui_fill_rect(icon->x + 16, icon->y, 32, 32, col);
        
        int name_len = 0;
        while (icon->name[name_len]) name_len++;
        int text_x = icon->x + 32 - (name_len * CHAR_WIDTH) / 2;
        if (text_x < icon->x) text_x = icon->x;
        gui_draw_text_transparent(icon->name, text_x, icon->y + 36, 0xFFFFFF);
    }
}

static void gui_draw_desktop_context_menu(void) {
    if (!g_desktop_context_menu_open) return;
    int w = 140, h = 64;
    gui_fill_rect(g_desktop_context_menu_x, g_desktop_context_menu_y, w, h, 0x101010);
    gui_fill_rect(g_desktop_context_menu_x, g_desktop_context_menu_y, w, 32, 0x202020);
    gui_draw_text_transparent("New Folder", g_desktop_context_menu_x + 8, g_desktop_context_menu_y + 8, 0xFFFFFF);
    gui_fill_rect(g_desktop_context_menu_x, g_desktop_context_menu_y + 32, w, 32, 0x202020);
    gui_draw_text_transparent("New File", g_desktop_context_menu_x + 8, g_desktop_context_menu_y + 40, 0xFFFFFF);
}


static void gui_draw_desktop(int redraw_console) {
    // Полная очистка — проще для прототипа.
    gui_fill_rect(0, 0, (int)fb_root.width, (int)fb_root.height, DESKTOP_BG);

    if (!g_in_dos_mode) {
        gui_refresh_desktop_icons();
        gui_draw_desktop_icons_ui();
        gui_draw_taskbar();
        gui_draw_start_menu();
        if (g_app_active && !g_app_minimized && !g_app_closed) gui_app_draw_frame();
        if (!g_terminal_minimized && !g_terminal_closed) gui_draw_window_frame();
        gui_apply_console_viewport(redraw_console);
        gui_draw_desktop_context_menu();
        // Re-draw cursor on top of UI.
        g_cursor_saved_valid = 0;
        gui_cursor_draw_at(g_mouse_x_px, g_mouse_y_px);
    } else {
        gui_apply_console_viewport(1);
    }
}

static void ps2_wait_input_empty(void) {
    while (inb(0x64) & 2) {}
}

static void ps2_wait_output_full(void) {
    while (!(inb(0x64) & 1)) {}
}

static int ps2_wait_input_empty_timeout(uint32_t spins) {
    while (spins--) {
        if ((inb(0x64) & 2) == 0) return 1;
        __asm__ volatile ("nop");
    }
    return 0;
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

static void ps2_flush_aux_output(void) {
    // Проматываем возможные старые AUX байты, чтобы не собрать "битый" пакет.
    g_mouse_packet_idx = 0;
    for (int i = 0; i < 10000; ++i) {
        uint8_t st = inb(0x64);
        if (!(st & 1)) break;
        if (st & 0x20) {
            (void)inb(0x60);
        } else {
            (void)inb(0x60);
        }
    }
}

static void ps2_mouse_write(uint8_t cmd) {
    ps2_wait_input_empty();
    outb(0x64, 0xD4);
    ps2_wait_input_empty();
    outb(0x60, cmd);
}

static int ps2_mouse_wait_ack(uint32_t spins) {
    // Ждём ответа от мыши (AUX). В норме ACK=0xFA, но после RESET может прийти 0xAA.
    while (spins--) {
        uint8_t st = inb(0x64);
        if (st & 1) {
            uint8_t b = inb(0x60);
            if (st & 0x20) {
                if (b == 0xFA) return 1; // ACK
                if (b == 0xFE) return 0; // RESEND
                // 0xAA self-test passed (после reset) — не считаем ACK, продолжаем ждать.
            }
        }
        __asm__ volatile ("nop");
    }
    return 0;
}

static void ps2_configure_controller(void) {
    // Базовая настройка i8042: включаем IRQ1/IRQ12 и AUX, чистим буферы.
    // Последовательность мягкая (без full self-test), чтобы не ломать VM.
    uint8_t cmd_byte = 0;

    // Disable ports
    ps2_write_cmd(0xAD); // disable keyboard
    ps2_write_cmd(0xA7); // disable mouse (AUX)

    // Flush any pending output
    for (int i = 0; i < 64; ++i) {
        uint8_t dummy;
        if (!ps2_read_data_timeout(&dummy, 2000)) break;
    }

    // Read command byte
    ps2_write_cmd(0x20);
    (void)ps2_read_data_timeout(&cmd_byte, 200000);

    // Bits (typical):
    // 0: IRQ1 enable, 1: IRQ12 enable, 4: disable keyboard clock (1=disable), 5: disable mouse clock (1=disable)
    // We want IRQs enabled and clocks enabled.
    cmd_byte |= (1u << 0);  // IRQ1 enable
    cmd_byte |= (1u << 1);  // IRQ12 enable
    cmd_byte &= (uint8_t)~(1u << 4); // enable keyboard clock
    cmd_byte &= (uint8_t)~(1u << 5); // enable mouse clock

    // Write command byte
    ps2_write_cmd(0x60);
    ps2_write_data(cmd_byte);

    // Enable ports
    ps2_write_cmd(0xAE); // enable keyboard
    ps2_write_cmd(0xA8); // enable mouse (AUX)
}

static void ps2_mouse_init(void) {
    ps2_configure_controller();

    // Try to get mouse into a known state.
    // Set defaults (0xF6) then enable streaming (0xF4), with ACK checks.
    ps2_flush_aux_output();

    ps2_mouse_write(0xF6);
    (void)ps2_mouse_wait_ack(500000);

    ps2_mouse_write(0xF4);
    (void)ps2_mouse_wait_ack(500000);

    ps2_flush_aux_output();

    g_mouse_x_px = (int)(fb_root.width / 2);
    g_mouse_y_px = (int)((fb_root.height - g_taskbar_rows_chars * CHAR_HEIGHT) / 2);
    g_mouse_left = 0;
    g_mouse_left_prev = 0;
    g_dragging = 0;
    g_mouse_packet_idx = 0;
}

void gui_init(void) {
    g_in_dos_mode = 0;
    g_terminal_minimized = 0;
    g_terminal_closed = 0;

    uint32_t total_cols = fb_cols_root();
    uint32_t total_rows = fb_rows_root();

    uint32_t avail_rows = total_rows - g_taskbar_rows_chars;

    // Пытаемся сделать окно максимально похожим на Windows 95: не на весь экран, но большое.
    uint32_t desired_cols = total_cols > 90 ? 90 : (total_cols > 20 ? total_cols - 4 : total_cols);
    uint32_t desired_rows = avail_rows > 32 ? 32 : (avail_rows > 10 ? avail_rows - 4 : avail_rows);

    if (desired_cols < 20) desired_cols = total_cols;
    if (desired_rows < 8) desired_rows = avail_rows;

    if (desired_cols < 4) desired_cols = 4;
    if (desired_rows < 4) desired_rows = 4;

    g_win_cols = (int)desired_cols;
    g_win_rows = (int)desired_rows;

    int max_left = (int)total_cols - g_win_cols;
    int max_top = (int)avail_rows - g_win_rows;
    g_win_left = max_left > 0 ? max_left / 2 : 0;
    g_win_top = max_top > 0 ? 1 : 0;

    // Приводим к минимуму 1свободное место сверху/снизу для рамки.
    if (g_win_left < 1 && total_cols > g_win_cols) g_win_left = 1;
    if (g_win_top < 1 && avail_rows > (uint32_t)g_win_rows) g_win_top = 1;

    // Устанавливаем viewport под окно, но не рисуем буфер (shell сам сделает clear_screen).
    gui_apply_console_viewport(0);
    gui_draw_desktop(0);

    ps2_mouse_init();
}

void gui_mouse_push_byte(uint8_t byte) {
    if (g_in_dos_mode) return;
    // Собираем 3-байтный пакет.
    if (g_mouse_packet_idx == 0) {
        // Первый байт: обычно bit3=1.
        if ((byte & 0x08) == 0) return;
        g_mouse_packet[0] = byte;
        g_mouse_packet_idx = 1;
        return;
    }
    if (g_mouse_packet_idx == 1) {
        g_mouse_packet[1] = byte;
        g_mouse_packet_idx = 2;
        return;
    }
    if (g_mouse_packet_idx == 2) {
        g_mouse_packet[2] = byte;
        g_mouse_packet_idx = 0;

        // Remove previous cursor before applying movement.
        gui_cursor_restore();

        uint8_t b0 = g_mouse_packet[0];
        uint8_t dx_mag = g_mouse_packet[1];
        uint8_t dy_mag = g_mouse_packet[2];

        int left = (b0 & 0x01) ? 1 : 0;
        int right = (b0 & 0x02) ? 1 : 0;

        // PS/2 motion bytes are signed (two's complement). Sign bits in b0 mirror this.
        int dx = (int)(signed char)dx_mag;
        int dy = (int)(signed char)dy_mag;

        // Overflow bits: при наличии, clamp грубо.
        if (b0 & 0x40) dx = (dx < 0) ? -255 : 255;
        if (b0 & 0x80) dy = (dy < 0) ? -255 : 255;

        // Применяем к координатам экрана (y направлен вниз).
        g_mouse_x_px += dx;
        g_mouse_y_px -= dy;

        if (g_mouse_x_px < 0) g_mouse_x_px = 0;
        if (g_mouse_x_px >= (int)fb_root.width) g_mouse_x_px = (int)fb_root.width - 1;

        if (g_mouse_y_px < 0) g_mouse_y_px = 0;
        if (g_mouse_y_px >= (int)fb_root.height) g_mouse_y_px = (int)fb_root.height - 1;

        g_mouse_left_prev = g_mouse_left;
        g_mouse_left = left;
        g_mouse_right_prev = g_mouse_right;
        g_mouse_right = right;

        int left_pressed = (g_mouse_left == 1 && g_mouse_left_prev == 0);
        int left_released = (g_mouse_left == 0 && g_mouse_left_prev == 1);
        int right_pressed = (g_mouse_right == 1 && g_mouse_right_prev == 0);
        int right_released = (g_mouse_right == 0 && g_mouse_right_prev == 1);
        // Latches are for app client-area clicks only (set below after hit-testing).
        if (left_pressed) g_mouse_left_pressed_latch = 0;
        if (left_released) g_mouse_left_released_latch = 0;

        if (right_pressed) {
            // Check if right clicked on desktop
            int taskbar_h_px = g_taskbar_rows_chars * CHAR_HEIGHT;
            int taskbar_y_px = (int)fb_root.height - taskbar_h_px;
            if (g_mouse_y_px < taskbar_y_px) {
                // simple bounds check to ensure it's not on a window
                int on_window = 0;
                if (g_app_active && !g_app_minimized && !g_app_closed && gui_app_outer_contains(g_mouse_x_px, g_mouse_y_px)) on_window = 1;
                if (!g_terminal_minimized && !g_terminal_closed) {
                    int outer_x_px = g_win_left * CHAR_WIDTH;
                    int outer_y_px = g_win_top * CHAR_HEIGHT;
                    int outer_w_px = g_win_cols * CHAR_WIDTH;
                    int outer_h_px = g_win_rows * CHAR_HEIGHT;
                    if (rect_contains(g_mouse_x_px, g_mouse_y_px, outer_x_px, outer_y_px, outer_w_px, outer_h_px)) on_window = 1;
                }
                if (!on_window) {
                    g_desktop_context_menu_open = 1;
                    g_desktop_context_menu_x = g_mouse_x_px;
                    g_desktop_context_menu_y = g_mouse_y_px;
                    gui_draw_desktop(1);
                    return;
                }
            }
        }

        if (left_pressed) {
            // Taskbar hit-test.
            int taskbar_h_px = g_taskbar_rows_chars * CHAR_HEIGHT;
            int taskbar_y_px = (int)fb_root.height - taskbar_h_px;

            int start_btn_cols = 6;
            int start_w_px = start_btn_cols * CHAR_WIDTH;
            if (g_mouse_y_px >= taskbar_y_px) {
                // Start: open/close start menu
                if (rect_contains(g_mouse_x_px, g_mouse_y_px, 0, taskbar_y_px, start_w_px, taskbar_h_px)) {
                    g_start_menu_open = !g_start_menu_open;
                    gui_draw_desktop(1);
                    return;
                }

                int btn_x_px = start_w_px + CHAR_WIDTH;
                int btn_w_px = 10 * CHAR_WIDTH;
                int btn_y_top = taskbar_y_px + 2;
                int btn_h_px = taskbar_h_px - 4;
                if (rect_contains(g_mouse_x_px, g_mouse_y_px, btn_x_px, btn_y_top, btn_w_px, btn_h_px)) {
                    gui_terminal_taskbar_click();
                    return;
                }

                // App task button (if any)
                if (g_app_active && !g_app_closed) {
                    int app_btn_x = btn_x_px + btn_w_px + CHAR_WIDTH;
                    if (rect_contains(g_mouse_x_px, g_mouse_y_px, app_btn_x, btn_y_top, btn_w_px, btn_h_px)) {
                        if (g_app_minimized) gui_app_show();
                        else gui_app_minimize();
                        return;
                    }
                }

                return;
            }

            // Start menu click (above taskbar).
            if (g_start_menu_open) {
                int item_h_px = CHAR_HEIGHT;
                int menu_w_px = 22 * CHAR_WIDTH;

                int count = 0;
                const mljos_app_descriptor_t *apps = apps_registry_list(&count);
                int gui_count = 0;
                for (int i = 0; i < count; ++i) if (apps[i].supports_ui) gui_count++;
                if (gui_count < 1) gui_count = 1;

                int menu_h_px = (gui_count + 1) * item_h_px;
                int menu_x_px = 0;
                int menu_y_px = taskbar_y_px - menu_h_px;
                if (menu_y_px < 0) menu_y_px = 0;

                if (rect_contains(g_mouse_x_px, g_mouse_y_px, menu_x_px, menu_y_px, menu_w_px, menu_h_px)) {
                    int rel_y = g_mouse_y_px - menu_y_px;
                    int row = (rel_y / item_h_px) - 1; // -1 for header row
                    if (row >= 0) {
                        int idx = 0;
                        for (int i = 0; i < count; ++i) {
                            if (!apps[i].supports_ui) continue;
                            if (idx == row) {
                                g_start_menu_open = 0;
                                gui_draw_desktop(1);
                                shell_set_launch_flags(MLJOS_LAUNCH_GUI);
                                shell_exec_app_command(apps[i].name);
                                shell_set_launch_flags(0);
                                gui_draw_desktop(1);
                                return;
                            }
                            idx++;
                        }
                    }
                    g_start_menu_open = 0;
                    gui_draw_desktop(1);
                    return;
                }

                // Click outside closes.
                g_start_menu_open = 0;
                gui_draw_desktop(1);
                return;
            }

            // Title bar click/drag.
            // 1) App window interactions (top-most)
            if (g_app_active && !g_app_minimized && !g_app_closed && gui_app_outer_contains(g_mouse_x_px, g_mouse_y_px)) {
                int outer_x_px = g_app_left * CHAR_WIDTH;
                int outer_y_px = g_app_top * CHAR_HEIGHT;
                int outer_w_px = g_app_cols * CHAR_WIDTH;
                int btn_w_px = 2 * CHAR_WIDTH;
                int btn_right_x = outer_x_px + outer_w_px - 2 * btn_w_px - 1;
                int btn_min_x = btn_right_x;
                int btn_x_x = btn_right_x + btn_w_px + 1;

                if (rect_contains(g_mouse_x_px, g_mouse_y_px, btn_min_x, outer_y_px, btn_w_px, CHAR_HEIGHT)) {
                    gui_app_minimize();
                    return;
                }
                if (rect_contains(g_mouse_x_px, g_mouse_y_px, btn_x_x, outer_y_px, btn_w_px, CHAR_HEIGHT)) {
                    gui_app_close();
                    return;
                }

                // Drag by title bar
                if (rect_contains(g_mouse_x_px, g_mouse_y_px, outer_x_px, outer_y_px, outer_w_px, CHAR_HEIGHT)) {
                    g_app_dragging = 1;
                    g_app_drag_offset_x_px = g_mouse_x_px - outer_x_px;
                    g_app_drag_offset_y_px = g_mouse_y_px - outer_y_px;
                    return;
                }

                // Client click -> latch for app
                if (gui_app_client_contains(g_mouse_x_px, g_mouse_y_px)) {
                    g_mouse_left_pressed_latch = 1;
                    return;
                }

                // Click inside app frame but not client: ignore.
                return;
            }

            // 2) Terminal window interactions
            gui_window_title_click_or_drag();

            // Desktop Context Menu Click
            if (g_desktop_context_menu_open) {
                int menu_w = 140;
                int menu_h = 64;
                if (rect_contains(g_mouse_x_px, g_mouse_y_px, g_desktop_context_menu_x, g_desktop_context_menu_y, menu_w, menu_h)) {
                    if (g_mouse_y_px < g_desktop_context_menu_y + 32) {
                        os_api.launch_app("files");
                    } else {
                        os_api.launch_app("edit");
                    }
                    g_desktop_context_menu_open = 0;
                    gui_draw_desktop(1);
                    return;
                } else {
                    g_desktop_context_menu_open = 0;
                    gui_draw_desktop(1);
                }
            }

            // Desktop Icons Click
            for (int i = 0; i < g_desktop_icon_count; i++) {
                desktop_icon_t *icon = &g_desktop_icons[i];
                if (rect_contains(g_mouse_x_px, g_mouse_y_px, icon->x, icon->y, icon->w, icon->h)) {
                    char full_path[256];
                    int k = 0;
                    while (g_desktop_path[k] && k < 250) { full_path[k] = g_desktop_path[k]; k++; }
                    full_path[k++] = '/';
                    int n = 0;
                    while (icon->name[n] && k < 255) { full_path[k++] = icon->name[n++]; }
                    full_path[k] = '\0';

                    if (icon->is_dir) {
                        shell_set_launch_flags(MLJOS_LAUNCH_GUI);
                        os_api.launch_app_args("files", full_path);
                        shell_set_launch_flags(0);
                    } else {
                        int len = 0; while(icon->name[len]) len++;
                        if (len >= 4 && icon->name[len-4] == '.' && icon->name[len-3] == 'a' && icon->name[len-2] == 'p' && icon->name[len-1] == 'p') {
                            shell_set_launch_flags(MLJOS_LAUNCH_GUI);
                            os_api.launch_app(full_path);
                            shell_set_launch_flags(0);
                        } else {
                            shell_set_launch_flags(MLJOS_LAUNCH_GUI);
                            os_api.launch_app_args("edit", full_path);
                            shell_set_launch_flags(0);
                        }
                    }
                    g_desktop_context_menu_open = 0;
                    gui_draw_desktop(1);
                    return;
                }
            }
        }

        if (g_dragging) {
            if (left_released) g_dragging = 0;
            else gui_drag_update();
        }

        if (g_app_dragging) {
            if (left_released) {
                g_app_dragging = 0;
            } else if (g_mouse_left) {
                int total_cols = (int)fb_cols_root();
                int total_rows_avail = (int)fb_rows_root() - g_taskbar_rows_chars;

                int outer_x_px_new = g_mouse_x_px - g_app_drag_offset_x_px;
                int outer_y_px_new = g_mouse_y_px - g_app_drag_offset_y_px;

                int new_left_chars = outer_x_px_new / CHAR_WIDTH;
                int new_top_chars = outer_y_px_new / CHAR_HEIGHT;

                int max_left = total_cols - g_app_cols;
                int max_top = total_rows_avail - g_app_rows;
                if (max_left < 0) max_left = 0;
                if (max_top < 0) max_top = 0;
                new_left_chars = clampi(new_left_chars, 0, max_left);
                new_top_chars = clampi(new_top_chars, 0, max_top);

                if (new_left_chars != g_app_left || new_top_chars != g_app_top) {
                    g_app_left = new_left_chars;
                    g_app_top = new_top_chars;
                    gui_draw_desktop(1);
                    gui_app_recompute_client_rect();
                    gui_app_draw_frame();
                    g_ui_expose_latch = 1;
                }
            }
        }

        // Draw cursor if no full redraw happened.
        if (!g_in_dos_mode) gui_cursor_draw_at(g_mouse_x_px, g_mouse_y_px);
    }
}

void gui_tick(void) {
    if (g_in_dos_mode) return;

    uint8_t hh = 0, mm = 0, ss = 0;
    get_rtc_time(&hh, &mm, &ss);
    if (ss == g_last_clock_sec) return;
    g_last_clock_sec = ss;

    // Перерисовываем только таскбар (рамка окна не трогаем).
    int taskbar_h_px = g_taskbar_rows_chars * CHAR_HEIGHT;
    int taskbar_y_px = (int)fb_root.height - taskbar_h_px;
    gui_fill_rect(0, taskbar_y_px, (int)fb_root.width, taskbar_h_px, TASKBAR_BG);
    gui_draw_taskbar();
}

void gui_toggle_dos_mode(void) {
    // Таскбар и рамка скрываются, консоль разворачивается на весь экран.
    g_in_dos_mode = !g_in_dos_mode;
    gui_draw_desktop(1);
}

void gui_toggle_minimize_terminal(void) {
    if (g_in_dos_mode) return;
    g_terminal_minimized = !g_terminal_minimized;
    gui_draw_desktop(1);
}

void gui_restore_terminal(void) {
    if (g_in_dos_mode) return;
    g_terminal_minimized = 0;
    g_terminal_closed = 0;
    gui_draw_desktop(1);
}

void gui_move_terminal(int dx_chars, int dy_chars) {
    if (g_in_dos_mode) return;
    if (g_terminal_minimized || g_terminal_closed) return;

    int total_cols = (int)fb_cols_root();
    int total_rows = (int)fb_rows_root() - g_taskbar_rows_chars;

    int new_left = g_win_left + dx_chars;
    int new_top = g_win_top + dy_chars;

    int max_left = total_cols - g_win_cols;
    int max_top = total_rows - g_win_rows;

    if (new_left < 0) new_left = 0;
    if (new_left > max_left) new_left = max_left;
    if (new_top < 0) new_top = 0;
    if (new_top > max_top) new_top = max_top;

    g_win_left = new_left;
    g_win_top = new_top;

    gui_draw_desktop(1);
}

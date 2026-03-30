#include "ui.h"

#include "font.h"
#include "task.h"
#include "wm.h"

static uint32_t ui_screen_w_impl(void) {
    task_t *t = task_current();
    if (!t || !t->window) return 0;
    return (uint32_t)wm_window_client_w(t->window);
}

static uint32_t ui_screen_h_impl(void) {
    task_t *t = task_current();
    if (!t || !t->window) return 0;
    return (uint32_t)wm_window_client_h(t->window);
}

static void ui_fill_rect_impl(int x, int y, int w, int h, uint32_t rgb) {
    task_t *t = task_current();
    if (!t || !t->window) return;
    uint32_t *px = wm_window_client_pixels(t->window);
    int cw = wm_window_client_w(t->window);
    int ch = wm_window_client_h(t->window);
    uint32_t pitch = wm_window_client_pitch_bytes(t->window);
    if (!px || cw <= 0 || ch <= 0) return;

    // Clip
    if (w <= 0 || h <= 0) return;
    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > cw) x1 = cw;
    if (y1 > ch) y1 = ch;
    if (x1 <= x0 || y1 <= y0) return;

    for (int yy = y0; yy < y1; ++yy) {
        uint32_t *row = (uint32_t *)((uintptr_t)px + (uintptr_t)yy * pitch);
        for (int xx = x0; xx < x1; ++xx) row[xx] = rgb;
    }
    wm_mark_dirty();
}

static void ui_draw_text_impl(const char *s, int x, int y, uint32_t rgb) {
    task_t *t = task_current();
    if (!t || !t->window) return;
    uint32_t *px = wm_window_client_pixels(t->window);
    int cw = wm_window_client_w(t->window);
    int ch = wm_window_client_h(t->window);
    uint32_t pitch = wm_window_client_pitch_bytes(t->window);
    if (!px || cw <= 0 || ch <= 0 || !s) return;
    if (y < -16 || y >= ch) return;

    int xx = x;
    for (int i = 0; s[i]; ++i) {
        char chh = s[i];
        if (xx + 8 > cw) break;
        if (xx >= 0 && y >= 0 && y + 16 <= ch) {
            const uint8_t *glyph = font8x16[(uint8_t)chh];
            for (int gy = 0; gy < 16; ++gy) {
                uint8_t bits = glyph[gy];
                uint32_t *row = (uint32_t *)((uintptr_t)px + (uintptr_t)(y + gy) * pitch);
                for (int gx = 0; gx < 8; ++gx) {
                    if (bits & (1u << (7 - gx))) row[xx + gx] = rgb;
                }
            }
        }
        xx += 8;
    }
    wm_mark_dirty();
}

static void ui_draw_text_scale_impl(const char *s, int x, int y, uint32_t rgb, int scale) {
    if (scale <= 1) {
        ui_draw_text_impl(s, x, y, rgb);
        return;
    }

    task_t *t = task_current();
    if (!t || !t->window) return;
    uint32_t *px = wm_window_client_pixels(t->window);
    int cw = wm_window_client_w(t->window);
    int ch = wm_window_client_h(t->window);
    uint32_t pitch = wm_window_client_pitch_bytes(t->window);
    if (!px || cw <= 0 || ch <= 0 || !s) return;

    int xx = x;
    for (int i = 0; s[i]; ++i) {
        char chh = s[i];
        const uint8_t *glyph = font8x16[(uint8_t)chh];
        for (int gy = 0; gy < 16; ++gy) {
            uint8_t bits = glyph[gy];
            int py0 = y + gy * scale;
            int py1 = py0 + scale;
            if (py1 <= 0 || py0 >= ch) continue;
            for (int gx = 0; gx < 8; ++gx) {
                if (!(bits & (1u << (7 - gx)))) continue;
                int px0 = xx + gx * scale;
                int px1 = px0 + scale;
                if (px1 <= 0 || px0 >= cw) continue;
                int sy0 = py0 < 0 ? 0 : py0;
                int sy1 = py1 > ch ? ch : py1;
                int sx0 = px0 < 0 ? 0 : px0;
                int sx1 = px1 > cw ? cw : px1;
                for (int yy = sy0; yy < sy1; ++yy) {
                    uint32_t *row = (uint32_t *)((uintptr_t)px + (uintptr_t)yy * pitch);
                    for (int xx2 = sx0; xx2 < sx1; ++xx2) row[xx2] = rgb;
                }
            }
        }
        xx += 8 * scale;
        if (xx >= cw) break;
    }
    wm_mark_dirty();
}

static void ui_begin_app_impl(const char *title) {
    task_t *t = task_current();
    if (t && t->window && title) wm_window_set_title(t->window, title);
}

static void ui_end_app_impl(void) {
    task_t *t = task_current();
    if (t && t->window) wm_window_destroy(t->window);
    task_exit();
}

static int ui_poll_event_impl(mljos_ui_event_t *out_event) {
    task_t *t = task_current();
    if (!t || !t->window || !out_event) return 0;
    if (t->killed) task_exit();

    if (wm_window_poll_event(t->window, out_event)) return 1;
    // No events: yield to WM/kernel.
    task_yield();
    return 0;
}

static mljos_ui_api_t g_ui_api = {
    .screen_w = ui_screen_w_impl,
    .screen_h = ui_screen_h_impl,
    .fill_rect = ui_fill_rect_impl,
    .draw_text = ui_draw_text_impl,
    .draw_text_scale = ui_draw_text_scale_impl,
    .begin_app = ui_begin_app_impl,
    .end_app = ui_end_app_impl,
    .poll_event = ui_poll_event_impl,
};

mljos_ui_api_t *ui_api(void) {
    return &g_ui_api;
}

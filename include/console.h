#ifndef CONSOLE_H
#define CONSOLE_H

#include "common.h"

typedef struct console console_t;

#define COLOR_DEFAULT 0xFFFFFF
#define COLOR_PROMPT  0x00FF00
#define COLOR_ERROR   0xFF0000
#define COLOR_ALERT   0xFFFF00

struct framebuffer {
    uint32_t *address;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
};

// Root framebuffer info (полн. экран).
extern struct framebuffer fb_root;

// Per-task console state is selected automatically using task_current()->console.
// These pointer helpers allow legacy code to keep using `fb`, `COLOR`, `cursor_row`, `cursor_col`.
struct framebuffer *console_fb_ptr(void);
uint32_t *console_color_ptr(void);
int *console_cursor_row_ptr(void);
int *console_cursor_col_ptr(void);

#define fb (*console_fb_ptr())
#define COLOR (*console_color_ptr())
#define cursor_row (*console_cursor_row_ptr())
#define cursor_col (*console_cursor_col_ptr())

#define VGA_COLS (fb.width / 8)
#define VGA_ROWS (fb.height / 16)

void console_init(uint32_t *addr, uint32_t w, uint32_t h, uint32_t p);
console_t *console_create(void);
// Updates the logical screen size (in pixels) within the physical framebuffer.
// Returns 1 on success, 0 if the requested size is not supported.
int console_set_screen_size(uint32_t w, uint32_t h);
void console_get_screen_size(uint32_t *w, uint32_t *h);
void console_get_max_screen_size(uint32_t *w, uint32_t *h);
// Binds console rendering to an arbitrary framebuffer (e.g. a window backbuffer).
// This does NOT change `fb_root` (physical screen); only the console target changes.
void console_bind_target(console_t *c, uint32_t *addr, uint32_t w, uint32_t h, uint32_t pitch);
// Re-binds console rendering back to the physical screen (`fb_root`).
void console_bind_screen(void);
// Настраивает активный viewport консоли (в пикселях относительно fb_root).
// Параметры размера будут округлены вниз до кратности CHAR_WIDTH/CHAR_HEIGHT.
void console_set_viewport(uint32_t origin_x_px, uint32_t origin_y_px, uint32_t w_px, uint32_t h_px);
// Управляет тем, будет ли консоль рисовать пиксели в framebuffer.
// Буфер (текст) при этом продолжает обновляться, чтобы содержимое можно было перерисовать.
void console_set_visible(console_t *c, int visible);
int console_is_visible(console_t *c);
// Полная перерисовка текста из буфера в текущий viewport.
void console_redraw(console_t *c);
void scroll_if_needed(void);
// Scroll view by a number of text lines (positive = down, negative = up).
void console_scroll_lines(console_t *c, int delta);
// Scroll view to an absolute top row (0..max).
void console_scroll_to(console_t *c, int top);
// Scrollbar width in pixels (for UI layout).
int console_scrollbar_width_px(void);
// Returns current scroll info: top row, visible rows, total rows.
void console_get_scroll_info(console_t *c, int *out_top, int *out_visible, int *out_total);
int console_get_view_cols(console_t *c);
int console_get_view_rows(console_t *c);
uint8_t console_get_cell_char(console_t *c, int row, int col);
uint32_t console_get_cell_color(console_t *c, int row, int col);
int console_copy_range(console_t *c, int start_row, int start_col, int end_row, int end_col, char *out, int maxlen);
void putchar_at(char ch, int row, int col);
void putchar(char ch);
void puts(const char *s);
void clear_screen(void);
void update_cursor(void); // Software cursor wrapper

#endif

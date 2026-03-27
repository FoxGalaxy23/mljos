#ifndef CONSOLE_H
#define CONSOLE_H

#include "common.h"

#define COLOR_DEFAULT 0xFFFFFF
#define COLOR_PROMPT  0x00FF00
#define COLOR_ERROR   0xFF0000
#define COLOR_ALERT   0xFFFF00

#define VGA_COLS (fb.width / 8)
#define VGA_ROWS (fb.height / 16)

extern uint32_t COLOR;
extern int cursor_row;
extern int cursor_col;

struct framebuffer {
    uint32_t *address;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
};

extern struct framebuffer fb;
// Root framebuffer info (полн. экран). `fb` ниже — активный viewport консоли.
extern struct framebuffer fb_root;

void console_init(uint32_t *addr, uint32_t w, uint32_t h, uint32_t p);
// Binds console rendering to an arbitrary framebuffer (e.g. a window backbuffer).
// This does NOT change `fb_root` (physical screen); only the console target changes.
void console_bind_target(uint32_t *addr, uint32_t w, uint32_t h, uint32_t pitch);
// Re-binds console rendering back to the physical screen (`fb_root`).
void console_bind_screen(void);
// Настраивает активный viewport консоли (в пикселях относительно fb_root).
// Параметры размера будут округлены вниз до кратности CHAR_WIDTH/CHAR_HEIGHT.
void console_set_viewport(uint32_t origin_x_px, uint32_t origin_y_px, uint32_t w_px, uint32_t h_px);
// Управляет тем, будет ли консоль рисовать пиксели в framebuffer.
// Буфер (текст) при этом продолжает обновляться, чтобы содержимое можно было перерисовать.
void console_set_visible(int visible);
int console_is_visible(void);
// Полная перерисовка текста из буфера в текущий viewport.
void console_redraw(void);
void scroll_if_needed(void);
void putchar_at(char ch, int row, int col);
void putchar(char ch);
void puts(const char *s);
void clear_screen(void);
void update_cursor(void); // Software cursor wrapper

#endif

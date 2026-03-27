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

void console_init(uint32_t *addr, uint32_t w, uint32_t h, uint32_t p);
void scroll_if_needed(void);
void putchar_at(char ch, int row, int col);
void putchar(char ch);
void puts(const char *s);
void clear_screen(void);
void update_cursor(void); // Software cursor wrapper

#endif

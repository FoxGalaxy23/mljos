#ifndef CONSOLE_H
#define CONSOLE_H

#include "common.h"

#define COLOR_DEFAULT 0x0F
#define COLOR_PROMPT  0x0A
#define COLOR_ERROR   0x0C
#define COLOR_ALERT   0x0E

extern uint8_t COLOR;
extern const int VGA_COLS;
extern const int VGA_ROWS;
extern int cursor_row;
extern int cursor_col;

void scroll_if_needed(void);
void putchar_at(char ch, int row, int col);
void putchar(char ch);
void puts(const char *s);
void clear_screen(void);
void update_cursor(void);

#endif

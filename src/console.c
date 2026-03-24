#include "console.h"
#include "io.h"

volatile uint8_t *vga_buffer = (volatile uint8_t *)0xB8000;
const int VGA_COLS = 80;
const int VGA_ROWS = 25;
uint8_t COLOR = COLOR_DEFAULT;
int cursor_row = 0;
int cursor_col = 0;

static void *kmemmove(void *dst, const void *src, unsigned int n) {
    unsigned char *d = (unsigned char*)dst;
    const unsigned char *s = (const unsigned char*)src;
    if (d == s) return dst;
    if (d < s) {
        for (unsigned int i = 0; i < n; ++i) d[i] = s[i];
    } else {
        for (unsigned int i = n; i-- > 0;) d[i] = s[i];
    }
    return dst;
}

void update_cursor(void) {
    uint16_t pos = (uint16_t)(cursor_row * VGA_COLS + cursor_col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

void scroll_if_needed(void) {
    if (cursor_row < VGA_ROWS) return;

    const int row_bytes = VGA_COLS * 2;
    kmemmove((void*)vga_buffer, (const void*)(vga_buffer + row_bytes), (VGA_ROWS - 1) * row_bytes);

    for (int c = 0; c < VGA_COLS; ++c) {
        int offset = ((VGA_ROWS - 1) * VGA_COLS + c) * 2;
        vga_buffer[offset] = ' ';
        vga_buffer[offset + 1] = COLOR_DEFAULT;
    }

    cursor_row = VGA_ROWS - 1;
    cursor_col = 0;
    update_cursor();
}

void putchar_at(char ch, int row, int col) {
    if (row < 0 || row >= VGA_ROWS || col < 0 || col >= VGA_COLS) return;
    vga_buffer[(row * VGA_COLS + col) * 2] = (uint8_t)ch;
    vga_buffer[(row * VGA_COLS + col) * 2 + 1] = COLOR;
}

void putchar(char ch) {
    if (ch == '\n') {
        cursor_col = 0;
        cursor_row++;
        scroll_if_needed();
        update_cursor();
        return;
    }

    if (ch == '\r') {
        cursor_col = 0;
        update_cursor();
        return;
    }

    if (ch == '\t') {
        int t = 4 - (cursor_col % 4);
        while (t--) putchar(' ');
        return;
    }

    putchar_at(ch, cursor_row, cursor_col);
    cursor_col++;
    if (cursor_col >= VGA_COLS) {
        cursor_col = 0;
        cursor_row++;
        scroll_if_needed();
    }
    update_cursor();
}

void puts(const char *s) {
    for (int i = 0; s[i]; ++i) putchar(s[i]);
}

void clear_screen(void) {
    for (int i = 0; i < VGA_COLS * VGA_ROWS; ++i) {
        vga_buffer[i * 2] = ' ';
        vga_buffer[i * 2 + 1] = COLOR_DEFAULT;
    }
    cursor_row = 0;
    cursor_col = 0;
    COLOR = COLOR_DEFAULT;
    update_cursor();
}

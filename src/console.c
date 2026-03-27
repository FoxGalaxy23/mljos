#include "console.h"
#include "font.h"

struct framebuffer fb;
uint32_t COLOR = COLOR_DEFAULT;
int cursor_row = 0;
int cursor_col = 0;

static const int CHAR_WIDTH = 8;
static const int CHAR_HEIGHT = 16;

static void *kmemcpy(void *dst, const void *src, unsigned int n) {
    unsigned char *d = (unsigned char*)dst;
    const unsigned char *s = (const unsigned char*)src;
    for (unsigned int i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}

static void *kmemset(void *s, int c, unsigned int n) {
    unsigned char *p = (unsigned char*)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

void console_init(uint32_t *addr, uint32_t w, uint32_t h, uint32_t p) {
    fb.address = addr;
    fb.width = w;
    fb.height = h;
    fb.pitch = p;
    clear_screen();
}

void clear_screen(void) {
    if (!fb.address) return;
    for (uint32_t y = 0; y < fb.height; ++y) {
        uint32_t *row = (uint32_t*)((uintptr_t)fb.address + y * fb.pitch);
        for (uint32_t x = 0; x < fb.width; ++x) {
            row[x] = 0; // Black
        }
    }
    cursor_row = 0;
    cursor_col = 0;
}

void scroll_if_needed(void) {
    uint32_t max_rows = fb.height / CHAR_HEIGHT;
    if (cursor_row < max_rows) return;

    // Scroll up by one character height
    uint32_t row_size = CHAR_HEIGHT * fb.pitch;
    uint32_t total_size = fb.height * fb.pitch;
    
    kmemcpy(fb.address, (void*)((uintptr_t)fb.address + row_size), total_size - row_size);
    
    // Clear last line
    kmemset((void*)((uintptr_t)fb.address + total_size - row_size), 0, row_size);

    cursor_row = max_rows - 1;
}

void draw_char(char ch, int x, int y, uint32_t color) {
    if (!fb.address) return;
    const uint8_t *glyph = font8x16[(uint8_t)ch];
    for (int i = 0; i < CHAR_HEIGHT; ++i) {
        uint8_t bits = glyph[i];
        uint32_t *pixel = (uint32_t*)((uintptr_t)fb.address + (y + i) * fb.pitch + x * 4);
        for (int j = 0; j < CHAR_WIDTH; ++j) {
            if (bits & (1 << (7 - j))) {
                pixel[j] = color;
            } else {
                pixel[j] = 0; // Transparent/Black
            }
        }
    }
}

void putchar_at(char ch, int row, int col) {
    draw_char(ch, col * CHAR_WIDTH, row * CHAR_HEIGHT, COLOR);
}

void putchar(char ch) {
    if (!fb.address) return;
    uint32_t max_cols = fb.width / CHAR_WIDTH;
    uint32_t max_rows = fb.height / CHAR_HEIGHT;
    if (max_cols == 0 || max_rows == 0) return;

    if (ch == '\n') {
        cursor_col = 0;
        cursor_row++;
        scroll_if_needed();
        return;
    }
    if (ch == '\r') {
        cursor_col = 0;
        return;
    }
    if (ch == '\t') {
        for (int i = 0; i < 4; ++i) putchar(' ');
        return;
    }

    putchar_at(ch, cursor_row, cursor_col);
    cursor_col++;
    if (cursor_col >= max_cols) {
        cursor_col = 0;
        cursor_row++;
        scroll_if_needed();
    }
}

void puts(const char *s) {
    while (*s) putchar(*s++);
}

void update_cursor(void) {
    // Software cursor not yet implemented, for now just a stub.
}

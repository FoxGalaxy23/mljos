#include "console.h"
#include "font.h"
#include "wm.h"

struct framebuffer fb_root;
struct framebuffer fb;
uint32_t COLOR = COLOR_DEFAULT;
int cursor_row = 0;
int cursor_col = 0;

// Active console render target root (may be different from physical screen fb_root).
static struct framebuffer g_target_root;

static const int CHAR_WIDTH = 8;
static const int CHAR_HEIGHT = 16;

// Max buffer size in character cells (подбирается под типичные framebuffer в QEMU).
#define CONSOLE_MAX_COLS 256
#define CONSOLE_MAX_ROWS 80
static uint8_t g_cell_ch[CONSOLE_MAX_ROWS][CONSOLE_MAX_COLS];
static uint32_t g_cell_color[CONSOLE_MAX_ROWS][CONSOLE_MAX_COLS];
static int g_visible = 1;
static uint32_t g_rows = 0;
static uint32_t g_cols = 0;

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

static uint32_t clamp_cells_cols(uint32_t cols) {
    return cols > CONSOLE_MAX_COLS ? CONSOLE_MAX_COLS : cols;
}

static uint32_t clamp_cells_rows(uint32_t rows) {
    return rows > CONSOLE_MAX_ROWS ? CONSOLE_MAX_ROWS : rows;
}

static void buffer_clear_all_cells(uint32_t rows, uint32_t cols) {
    // Обнуляем только активную область.
    for (uint32_t r = 0; r < rows; ++r) {
        for (uint32_t c = 0; c < cols; ++c) {
            g_cell_ch[r][c] = ' ';
            g_cell_color[r][c] = 0;
        }
    }
}

static void buffer_resize_preserve(uint32_t new_rows, uint32_t new_cols) {
    uint32_t old_rows = g_rows;
    uint32_t old_cols = g_cols;

    // Сохраняем старое содержимое в пересечении, а новые ячейки делаем пустыми.
    for (uint32_t r = 0; r < new_rows; ++r) {
        for (uint32_t c = 0; c < new_cols; ++c) {
            if (r >= old_rows || c >= old_cols) {
                g_cell_ch[r][c] = ' ';
                g_cell_color[r][c] = 0;
            }
        }
    }

    g_rows = new_rows;
    g_cols = new_cols;
}

static void draw_char(char ch, int x, int y, uint32_t color) {
    if (!fb.address) return;
    const uint8_t *glyph = font8x16[(uint8_t)ch];
    for (int i = 0; i < CHAR_HEIGHT; ++i) {
        uint8_t bits = glyph[i];
        uint32_t *pixel = (uint32_t*)((uintptr_t)fb.address + (y + i) * fb.pitch + x * 4);
        for (int j = 0; j < CHAR_WIDTH; ++j) {
            if (bits & (1 << (7 - j))) {
                pixel[j] = color;
            } else {
                pixel[j] = 0; // Черный фон внутри ячейки (упрощение).
            }
        }
    }
}

void console_init(uint32_t *addr, uint32_t w, uint32_t h, uint32_t p) {
    fb_root.address = addr;
    fb_root.pitch = p;

    // Округляем к границам символов.
    uint32_t cols = clamp_cells_cols(w / CHAR_WIDTH);
    uint32_t rows = clamp_cells_rows(h / CHAR_HEIGHT);
    fb_root.width = cols * CHAR_WIDTH;
    fb_root.height = rows * CHAR_HEIGHT;

    // В начале viewport == весь экран.
    g_target_root = fb_root;
    fb = g_target_root;
    g_visible = 1;
    g_cols = cols;
    g_rows = rows;

    buffer_clear_all_cells(g_rows, g_cols);
    clear_screen();
}

void console_bind_target(uint32_t *addr, uint32_t w, uint32_t h, uint32_t pitch) {
    if (!addr || w == 0 || h == 0 || pitch == 0) return;

    // Round down to character grid.
    uint32_t cols = clamp_cells_cols(w / CHAR_WIDTH);
    uint32_t rows = clamp_cells_rows(h / CHAR_HEIGHT);
    if (cols == 0) cols = 1;
    if (rows == 0) rows = 1;

    g_target_root.address = addr;
    g_target_root.pitch = pitch;
    g_target_root.width = cols * CHAR_WIDTH;
    g_target_root.height = rows * CHAR_HEIGHT;

    fb = g_target_root;
    buffer_resize_preserve(rows, cols);

    if ((uint32_t)cursor_row >= g_rows) cursor_row = (int)g_rows - 1;
    if ((uint32_t)cursor_col >= g_cols) cursor_col = (int)g_cols - 1;
}

void console_bind_screen(void) {
    g_target_root = fb_root;
    fb = g_target_root;
    buffer_resize_preserve(clamp_cells_rows(fb.height / CHAR_HEIGHT), clamp_cells_cols(fb.width / CHAR_WIDTH));
    if ((uint32_t)cursor_row >= g_rows) cursor_row = (int)g_rows - 1;
    if ((uint32_t)cursor_col >= g_cols) cursor_col = (int)g_cols - 1;
}

void clear_screen(void) {
    if (!g_rows || !g_cols) return;

    buffer_clear_all_cells(g_rows, g_cols);

    cursor_row = 0;
    cursor_col = 0;

    if (!g_visible || !fb.address) return;

    for (uint32_t y = 0; y < fb.height; ++y) {
        uint32_t *row = (uint32_t*)((uintptr_t)fb.address + y * fb.pitch);
        for (uint32_t x = 0; x < fb.width; ++x) row[x] = 0;
    }
    wm_mark_dirty();
}

void scroll_if_needed(void) {
    uint32_t max_rows = g_rows;
    if (max_rows == 0) return;
    if ((uint32_t)cursor_row < max_rows) return;

    // Scroll up by one character height
    uint32_t row_size = CHAR_HEIGHT * fb.pitch;
    uint32_t total_size = fb.height * fb.pitch;

    // 1) Сдвиг буфера (всегда, чтобы контент корректно перерисовывался).
    for (uint32_t r = 1; r < max_rows; ++r) {
        for (uint32_t c = 0; c < g_cols; ++c) {
            g_cell_ch[r - 1][c] = g_cell_ch[r][c];
            g_cell_color[r - 1][c] = g_cell_color[r][c];
        }
    }
    // Очистка последней строки.
    for (uint32_t c = 0; c < g_cols; ++c) {
        g_cell_ch[max_rows - 1][c] = ' ';
        g_cell_color[max_rows - 1][c] = 0;
    }

    // 2) Сдвиг пикселей только если консоль видима.
    if (g_visible && fb.address) {
        kmemcpy(fb.address, (void*)((uintptr_t)fb.address + row_size), total_size - row_size);
        kmemset((void*)((uintptr_t)fb.address + total_size - row_size), 0, row_size);
    }

    cursor_row = (int)max_rows - 1;
    wm_mark_dirty();
}

void putchar_at(char ch, int row, int col) {
    if (row < 0 || col < 0) return;
    if ((uint32_t)row >= g_rows || (uint32_t)col >= g_cols) return;

    g_cell_ch[row][col] = (ch == 0 ? ' ' : ch);
    g_cell_color[row][col] = COLOR;

    if (!g_visible) return;
    draw_char(ch == 0 ? ' ' : ch, col * CHAR_WIDTH, row * CHAR_HEIGHT, COLOR);
    wm_mark_dirty();
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

void console_set_viewport(uint32_t origin_x_px, uint32_t origin_y_px, uint32_t w_px, uint32_t h_px) {
    if (!g_target_root.address) return;

    // Приводим к границе символа.
    origin_x_px = (origin_x_px / CHAR_WIDTH) * CHAR_WIDTH;
    origin_y_px = (origin_y_px / CHAR_HEIGHT) * CHAR_HEIGHT;

    uint32_t new_cols = clamp_cells_cols(w_px / CHAR_WIDTH);
    uint32_t new_rows = clamp_cells_rows(h_px / CHAR_HEIGHT);

    // Если viewport обнулился — пусть будет хотя бы 1x1.
    if (new_cols == 0) new_cols = 1;
    if (new_rows == 0) new_rows = 1;

    uint32_t old_rows = g_rows;
    uint32_t old_cols = g_cols;

    fb.address = (uint32_t*)((uintptr_t)g_target_root.address + origin_y_px * g_target_root.pitch + origin_x_px * 4);
    fb.width = new_cols * CHAR_WIDTH;
    fb.height = new_rows * CHAR_HEIGHT;
    fb.pitch = g_target_root.pitch;

    buffer_resize_preserve(new_rows, new_cols);

    // Приводим курсор в допустимые границы.
    if ((uint32_t)cursor_row >= g_rows) cursor_row = (int)g_rows - 1;
    if ((uint32_t)cursor_col >= g_cols) cursor_col = (int)g_cols - 1;
    if (g_rows == 0) cursor_row = 0;
    if (g_cols == 0) cursor_col = 0;

    // Если размеры не менялись — буфер уже на месте, пиксели GUI перерисует сам.
    (void)old_rows;
    (void)old_cols;
}

void console_set_visible(int visible) {
    g_visible = visible ? 1 : 0;
}

int console_is_visible(void) {
    return g_visible;
}

void console_redraw(void) {
    if (!g_rows || !g_cols) return;
    if (!fb.address) return;
    if (!g_visible) return;

    // Черный фон в viewport.
    for (uint32_t y = 0; y < fb.height; ++y) {
        uint32_t *row = (uint32_t*)((uintptr_t)fb.address + y * fb.pitch);
        for (uint32_t x = 0; x < fb.width; ++x) row[x] = 0;
    }

    for (uint32_t r = 0; r < g_rows; ++r) {
        for (uint32_t c = 0; c < g_cols; ++c) {
            uint8_t ch = g_cell_ch[r][c];
            if (ch == 0 || ch == ' ') continue;
            draw_char((char)ch, c * CHAR_WIDTH, r * CHAR_HEIGHT, g_cell_color[r][c]);
        }
    }
    wm_mark_dirty();
}

void update_cursor(void) {
    // Software cursor not yet implemented, for now just a stub.
}

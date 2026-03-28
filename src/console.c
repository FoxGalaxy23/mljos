#include "console.h"
#include "font.h"
#include "kmem.h"
#include "task.h"
#include "wm.h"

struct framebuffer fb_root;

// Active console buffer size in character cells (подбирается под типичные framebuffer в QEMU).
#define CONSOLE_MAX_COLS 256
#define CONSOLE_MAX_ROWS 80

typedef struct console {
    struct framebuffer target_root; // current render target (full size, char-aligned)
    struct framebuffer viewport;    // active viewport within target_root

    uint32_t color;
    int cursor_r;
    int cursor_c;
    int visible;

    uint32_t rows;
    uint32_t cols;
    uint8_t cell_ch[CONSOLE_MAX_ROWS][CONSOLE_MAX_COLS];
    uint32_t cell_color[CONSOLE_MAX_ROWS][CONSOLE_MAX_COLS];
} console_t;

static console_t g_kernel_console;

static const int CHAR_WIDTH = 8;
static const int CHAR_HEIGHT = 16;

static console_t *console_current_impl(void) {
    task_t *t = task_current();
    if (t && t->console) return t->console;
    return &g_kernel_console;
}

struct framebuffer *console_fb_ptr(void) { return &console_current_impl()->viewport; }
uint32_t *console_color_ptr(void) { return &console_current_impl()->color; }
int *console_cursor_row_ptr(void) { return &console_current_impl()->cursor_r; }
int *console_cursor_col_ptr(void) { return &console_current_impl()->cursor_c; }

static uint32_t clamp_cells_cols(uint32_t cols) {
    return cols > CONSOLE_MAX_COLS ? CONSOLE_MAX_COLS : cols;
}

static uint32_t clamp_cells_rows(uint32_t rows) {
    return rows > CONSOLE_MAX_ROWS ? CONSOLE_MAX_ROWS : rows;
}

static void buffer_clear_all_cells(console_t *c, uint32_t rows, uint32_t cols) {
    if (!c) return;
    for (uint32_t r = 0; r < rows; ++r) {
        for (uint32_t cc = 0; cc < cols; ++cc) {
            c->cell_ch[r][cc] = ' ';
            c->cell_color[r][cc] = 0;
        }
    }
}

static void buffer_resize_preserve(console_t *c, uint32_t new_rows, uint32_t new_cols) {
    if (!c) return;
    uint32_t old_rows = c->rows;
    uint32_t old_cols = c->cols;

    for (uint32_t r = 0; r < new_rows; ++r) {
        for (uint32_t cc = 0; cc < new_cols; ++cc) {
            if (r >= old_rows || cc >= old_cols) {
                c->cell_ch[r][cc] = ' ';
                c->cell_color[r][cc] = 0;
            }
        }
    }

    c->rows = new_rows;
    c->cols = new_cols;
}

static void draw_char(console_t *c, char ch, int x, int y, uint32_t color) {
    if (!c) return;
    if (!c->viewport.address) return;
    const uint8_t *glyph = font8x16[(uint8_t)ch];
    for (int i = 0; i < CHAR_HEIGHT; ++i) {
        uint8_t bits = glyph[i];
        uint32_t *pixel = (uint32_t *)((uintptr_t)c->viewport.address + (uintptr_t)(y + i) * c->viewport.pitch + (uintptr_t)x * 4);
        for (int j = 0; j < CHAR_WIDTH; ++j) {
            if (bits & (1u << (7 - j))) pixel[j] = color;
            else pixel[j] = 0; // Черный фон внутри ячейки (упрощение).
        }
    }
}

console_t *console_create(void) {
    console_t *c = (console_t *)kmem_alloc(sizeof(console_t), 16);
    if (!c) return NULL;
    kmem_memset(c, 0, sizeof(*c));
    c->color = COLOR_DEFAULT;
    c->cursor_r = 0;
    c->cursor_c = 0;
    c->visible = 1;
    c->rows = 0;
    c->cols = 0;
    return c;
}

void console_init(uint32_t *addr, uint32_t w, uint32_t h, uint32_t p) {
    fb_root.address = addr;
    fb_root.pitch = p;

    // Округляем к границам символов.
    uint32_t cols = clamp_cells_cols(w / (uint32_t)CHAR_WIDTH);
    uint32_t rows = clamp_cells_rows(h / (uint32_t)CHAR_HEIGHT);
    fb_root.width = cols * (uint32_t)CHAR_WIDTH;
    fb_root.height = rows * (uint32_t)CHAR_HEIGHT;

    kmem_memset(&g_kernel_console, 0, sizeof(g_kernel_console));
    g_kernel_console.target_root = fb_root;
    g_kernel_console.viewport = g_kernel_console.target_root;
    g_kernel_console.color = COLOR_DEFAULT;
    g_kernel_console.cursor_r = 0;
    g_kernel_console.cursor_c = 0;
    g_kernel_console.visible = 1;
    g_kernel_console.rows = rows;
    g_kernel_console.cols = cols;

    buffer_clear_all_cells(&g_kernel_console, g_kernel_console.rows, g_kernel_console.cols);
    clear_screen();
}

void console_bind_target(console_t *c, uint32_t *addr, uint32_t w, uint32_t h, uint32_t pitch) {
    if (!c) c = console_current_impl();
    if (!addr || w == 0 || h == 0 || pitch == 0) return;

    // Round down to character grid.
    uint32_t cols = clamp_cells_cols(w / (uint32_t)CHAR_WIDTH);
    uint32_t rows = clamp_cells_rows(h / (uint32_t)CHAR_HEIGHT);
    if (cols == 0) cols = 1;
    if (rows == 0) rows = 1;

    c->target_root.address = addr;
    c->target_root.pitch = pitch;
    c->target_root.width = cols * (uint32_t)CHAR_WIDTH;
    c->target_root.height = rows * (uint32_t)CHAR_HEIGHT;

    c->viewport = c->target_root;
    buffer_resize_preserve(c, rows, cols);

    if ((uint32_t)c->cursor_r >= c->rows) c->cursor_r = (int)c->rows - 1;
    if ((uint32_t)c->cursor_c >= c->cols) c->cursor_c = (int)c->cols - 1;
}

void console_bind_screen(void) {
    console_t *c = console_current_impl();
    c->target_root = fb_root;
    c->viewport = c->target_root;
    buffer_resize_preserve(c,
        clamp_cells_rows(c->viewport.height / (uint32_t)CHAR_HEIGHT),
        clamp_cells_cols(c->viewport.width / (uint32_t)CHAR_WIDTH));
    if ((uint32_t)c->cursor_r >= c->rows) c->cursor_r = (int)c->rows - 1;
    if ((uint32_t)c->cursor_c >= c->cols) c->cursor_c = (int)c->cols - 1;
}

void clear_screen(void) {
    console_t *c = console_current_impl();
    if (!c->rows || !c->cols) return;

    buffer_clear_all_cells(c, c->rows, c->cols);

    c->cursor_r = 0;
    c->cursor_c = 0;

    if (!c->visible || !c->viewport.address) return;

    for (uint32_t y = 0; y < c->viewport.height; ++y) {
        uint32_t *row = (uint32_t *)((uintptr_t)c->viewport.address + (uintptr_t)y * c->viewport.pitch);
        for (uint32_t x = 0; x < c->viewport.width; ++x) row[x] = 0;
    }
    wm_mark_dirty();
}

void scroll_if_needed(void) {
    console_t *c = console_current_impl();
    uint32_t max_rows = c->rows;
    if (max_rows == 0) return;
    if ((uint32_t)c->cursor_r < max_rows) return;

    // Scroll up by one character height
    uint32_t row_size = (uint32_t)CHAR_HEIGHT * c->viewport.pitch;
    uint32_t total_size = c->viewport.height * c->viewport.pitch;

    // 1) Сдвиг буфера (всегда, чтобы контент корректно перерисовывался).
    for (uint32_t r = 1; r < max_rows; ++r) {
        for (uint32_t cc = 0; cc < c->cols; ++cc) {
            c->cell_ch[r - 1][cc] = c->cell_ch[r][cc];
            c->cell_color[r - 1][cc] = c->cell_color[r][cc];
        }
    }
    // Очистка последней строки.
    for (uint32_t cc = 0; cc < c->cols; ++cc) {
        c->cell_ch[max_rows - 1][cc] = ' ';
        c->cell_color[max_rows - 1][cc] = 0;
    }

    // 2) Сдвиг пикселей только если консоль видима.
    if (c->visible && c->viewport.address) {
        kmem_memcpy(c->viewport.address, (void *)((uintptr_t)c->viewport.address + row_size), total_size - row_size);
        kmem_memset((void *)((uintptr_t)c->viewport.address + total_size - row_size), 0, row_size);
    }

    c->cursor_r = (int)max_rows - 1;
    wm_mark_dirty();
}

void putchar_at(char ch, int row, int col) {
    console_t *c = console_current_impl();
    if (row < 0 || col < 0) return;
    if ((uint32_t)row >= c->rows || (uint32_t)col >= c->cols) return;

    c->cell_ch[row][col] = (ch == 0 ? ' ' : (uint8_t)ch);
    c->cell_color[row][col] = c->color;

    if (!c->visible) return;
    draw_char(c, ch == 0 ? ' ' : ch, col * CHAR_WIDTH, row * CHAR_HEIGHT, c->color);
    wm_mark_dirty();
}

void putchar(char ch) {
    console_t *c = console_current_impl();
    if (!c->viewport.address) return;
    uint32_t max_cols = c->viewport.width / (uint32_t)CHAR_WIDTH;
    uint32_t max_rows = c->viewport.height / (uint32_t)CHAR_HEIGHT;
    if (max_cols == 0 || max_rows == 0) return;

    if (ch == '\n') {
        c->cursor_c = 0;
        c->cursor_r++;
        scroll_if_needed();
        return;
    }
    if (ch == '\r') {
        c->cursor_c = 0;
        return;
    }
    if (ch == '\t') {
        for (int i = 0; i < 4; ++i) putchar(' ');
        return;
    }

    putchar_at(ch, c->cursor_r, c->cursor_c);
    c->cursor_c++;
    if ((uint32_t)c->cursor_c >= max_cols) {
        c->cursor_c = 0;
        c->cursor_r++;
        scroll_if_needed();
    }
}

void puts(const char *s) {
    if (!s) return;
    while (*s) putchar(*s++);
}

void console_set_viewport(uint32_t origin_x_px, uint32_t origin_y_px, uint32_t w_px, uint32_t h_px) {
    console_t *c = console_current_impl();
    if (!c->target_root.address) return;

    // Приводим к границе символа.
    origin_x_px = (origin_x_px / (uint32_t)CHAR_WIDTH) * (uint32_t)CHAR_WIDTH;
    origin_y_px = (origin_y_px / (uint32_t)CHAR_HEIGHT) * (uint32_t)CHAR_HEIGHT;

    uint32_t new_cols = clamp_cells_cols(w_px / (uint32_t)CHAR_WIDTH);
    uint32_t new_rows = clamp_cells_rows(h_px / (uint32_t)CHAR_HEIGHT);
    if (new_cols == 0) new_cols = 1;
    if (new_rows == 0) new_rows = 1;

    c->viewport.address = (uint32_t *)((uintptr_t)c->target_root.address + (uintptr_t)origin_y_px * c->target_root.pitch + (uintptr_t)origin_x_px * 4);
    c->viewport.width = new_cols * (uint32_t)CHAR_WIDTH;
    c->viewport.height = new_rows * (uint32_t)CHAR_HEIGHT;
    c->viewport.pitch = c->target_root.pitch;

    buffer_resize_preserve(c, new_rows, new_cols);

    if ((uint32_t)c->cursor_r >= c->rows) c->cursor_r = (int)c->rows - 1;
    if ((uint32_t)c->cursor_c >= c->cols) c->cursor_c = (int)c->cols - 1;
    if (c->rows == 0) c->cursor_r = 0;
    if (c->cols == 0) c->cursor_c = 0;
}

void console_set_visible(console_t *c, int visible) {
    if (!c) c = console_current_impl();
    c->visible = visible ? 1 : 0;
}

int console_is_visible(console_t *c) {
    if (!c) c = console_current_impl();
    return c->visible ? 1 : 0;
}

void console_redraw(console_t *c) {
    if (!c) c = console_current_impl();
    if (!c->rows || !c->cols) return;
    if (!c->viewport.address) return;
    if (!c->visible) return;

    // Черный фон в viewport.
    for (uint32_t y = 0; y < c->viewport.height; ++y) {
        uint32_t *row = (uint32_t *)((uintptr_t)c->viewport.address + (uintptr_t)y * c->viewport.pitch);
        for (uint32_t x = 0; x < c->viewport.width; ++x) row[x] = 0;
    }

    for (uint32_t r = 0; r < c->rows; ++r) {
        for (uint32_t cc = 0; cc < c->cols; ++cc) {
            uint8_t ch = c->cell_ch[r][cc];
            if (ch == 0 || ch == ' ') continue;
            draw_char(c, (char)ch, (int)cc * CHAR_WIDTH, (int)r * CHAR_HEIGHT, c->cell_color[r][cc]);
        }
    }
    wm_mark_dirty();
}

void update_cursor(void) {
    // Software cursor not yet implemented, for now just a stub.
}

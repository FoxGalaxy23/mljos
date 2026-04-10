#include "console.h"
#include "font.h"
#include "kmem.h"
#include "task.h"
#include "wm.h"

struct framebuffer fb_root;
static uint32_t g_fb_root_max_w = 0;
static uint32_t g_fb_root_max_h = 0;

// Active console buffer size in character cells (подбирается под типичные framebuffer в QEMU).
#define CONSOLE_MAX_COLS 256
#define CONSOLE_MAX_ROWS 256
static const int CONSOLE_SCROLLBAR_W_PX = 10;

typedef struct console {
    struct framebuffer target_root; // current render target (full size, char-aligned)
    struct framebuffer viewport;    // active viewport within target_root

    uint32_t color;
    int cursor_r;
    int cursor_c;
    int visible;

    uint32_t view_rows;
    uint32_t view_cols;
    uint32_t buf_rows;
    uint32_t buf_cols;
    uint32_t content_rows;
    int view_top;
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

static uint32_t console_total_rows(console_t *c) {
    if (!c) return 0;
    uint32_t total = c->content_rows;
    if (total < c->view_rows) total = c->view_rows;
    if (total > c->buf_rows) total = c->buf_rows;
    return total;
}

static uint32_t console_max_view_top(console_t *c) {
    if (!c || c->view_rows == 0 || c->buf_rows == 0) return 0;
    uint32_t total = console_total_rows(c);
    if (total <= c->view_rows) return 0;
    uint32_t max = total - c->view_rows;
    if (max > c->buf_rows - c->view_rows) max = c->buf_rows - c->view_rows;
    return max;
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

static void buffer_resize_cols(console_t *c, uint32_t new_cols) {
    if (!c) return;
    if (new_cols > CONSOLE_MAX_COLS) new_cols = CONSOLE_MAX_COLS;
    uint32_t old_cols = c->buf_cols;
    if (new_cols > old_cols) {
        for (uint32_t r = 0; r < c->buf_rows; ++r) {
            for (uint32_t cc = old_cols; cc < new_cols; ++cc) {
                c->cell_ch[r][cc] = ' ';
                c->cell_color[r][cc] = 0;
            }
        }
    }
    c->buf_cols = new_cols;
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
    c->view_rows = 0;
    c->view_cols = 0;
    c->buf_rows = CONSOLE_MAX_ROWS;
    c->buf_cols = 0;
    c->content_rows = 0;
    c->view_top = 0;
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
    g_fb_root_max_w = fb_root.width;
    g_fb_root_max_h = fb_root.height;

    kmem_memset(&g_kernel_console, 0, sizeof(g_kernel_console));
    g_kernel_console.target_root = fb_root;
    g_kernel_console.viewport = g_kernel_console.target_root;
    g_kernel_console.color = COLOR_DEFAULT;
    g_kernel_console.cursor_r = 0;
    g_kernel_console.cursor_c = 0;
    g_kernel_console.visible = 1;
    g_kernel_console.view_rows = rows;
    g_kernel_console.view_cols = cols;
    g_kernel_console.buf_rows = CONSOLE_MAX_ROWS;
    g_kernel_console.buf_cols = cols;
    g_kernel_console.content_rows = 0;
    g_kernel_console.view_top = 0;

    buffer_clear_all_cells(&g_kernel_console, g_kernel_console.buf_rows, g_kernel_console.buf_cols);
    clear_screen();
}

int console_set_screen_size(uint32_t w, uint32_t h) {
    if (!fb_root.address || w == 0 || h == 0) return 0;
    if (g_fb_root_max_w == 0 || g_fb_root_max_h == 0) {
        g_fb_root_max_w = fb_root.width;
        g_fb_root_max_h = fb_root.height;
    }

    uint32_t max_cols = clamp_cells_cols(g_fb_root_max_w / (uint32_t)CHAR_WIDTH);
    uint32_t max_rows = clamp_cells_rows(g_fb_root_max_h / (uint32_t)CHAR_HEIGHT);
    if (max_cols == 0 || max_rows == 0) return 0;

    uint32_t cols = clamp_cells_cols(w / (uint32_t)CHAR_WIDTH);
    uint32_t rows = clamp_cells_rows(h / (uint32_t)CHAR_HEIGHT);
    if (cols == 0 || rows == 0) return 0;
    if (cols > max_cols || rows > max_rows) return 0;

    fb_root.width = cols * (uint32_t)CHAR_WIDTH;
    fb_root.height = rows * (uint32_t)CHAR_HEIGHT;

    console_t *kc = &g_kernel_console;
    if (kc->target_root.address == fb_root.address && kc->target_root.pitch == fb_root.pitch) {
        int was_at_bottom = (kc->view_top >= (int)console_max_view_top(kc));
        kc->target_root = fb_root;
        kc->viewport = kc->target_root;
        kc->view_rows = rows;
        kc->view_cols = cols;
        buffer_resize_cols(kc, cols);
        if ((uint32_t)kc->cursor_r >= kc->buf_rows) kc->cursor_r = (int)kc->buf_rows - 1;
        if ((uint32_t)kc->cursor_c >= kc->buf_cols) kc->cursor_c = (int)kc->buf_cols - 1;
        if (kc->buf_rows == 0) kc->cursor_r = 0;
        if (kc->buf_cols == 0) kc->cursor_c = 0;
        if (was_at_bottom) kc->view_top = (int)console_max_view_top(kc);
        if (kc->view_top < 0) kc->view_top = 0;
        {
            uint32_t max_top = console_max_view_top(kc);
            if (kc->view_top > (int)max_top) kc->view_top = (int)max_top;
        }
        if (kc->visible) console_redraw(kc);
    }

    console_t *cur = console_current_impl();
    if (cur && cur != kc && cur->target_root.address == fb_root.address && cur->target_root.pitch == fb_root.pitch) {
        int was_at_bottom = (cur->view_top >= (int)console_max_view_top(cur));
        cur->target_root = fb_root;
        cur->viewport = cur->target_root;
        cur->view_rows = rows;
        cur->view_cols = cols;
        buffer_resize_cols(cur, cols);
        if ((uint32_t)cur->cursor_r >= cur->buf_rows) cur->cursor_r = (int)cur->buf_rows - 1;
        if ((uint32_t)cur->cursor_c >= cur->buf_cols) cur->cursor_c = (int)cur->buf_cols - 1;
        if (cur->buf_rows == 0) cur->cursor_r = 0;
        if (cur->buf_cols == 0) cur->cursor_c = 0;
        if (was_at_bottom) cur->view_top = (int)console_max_view_top(cur);
        if (cur->view_top < 0) cur->view_top = 0;
        {
            uint32_t max_top = console_max_view_top(cur);
            if (cur->view_top > (int)max_top) cur->view_top = (int)max_top;
        }
        if (cur->visible) console_redraw(cur);
    }

    return 1;
}

void console_get_screen_size(uint32_t *w, uint32_t *h) {
    if (w) *w = fb_root.width;
    if (h) *h = fb_root.height;
}

void console_get_max_screen_size(uint32_t *w, uint32_t *h) {
    if (g_fb_root_max_w == 0 || g_fb_root_max_h == 0) {
        g_fb_root_max_w = fb_root.width;
        g_fb_root_max_h = fb_root.height;
    }
    if (w) *w = g_fb_root_max_w;
    if (h) *h = g_fb_root_max_h;
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
    {
        int was_at_bottom = (c->view_top >= (int)console_max_view_top(c));
        c->view_rows = rows;
        c->view_cols = cols;
        buffer_resize_cols(c, cols);
        if ((uint32_t)c->cursor_r >= c->buf_rows) c->cursor_r = (int)c->buf_rows - 1;
        if ((uint32_t)c->cursor_c >= c->buf_cols) c->cursor_c = (int)c->buf_cols - 1;
        if (c->buf_rows == 0) c->cursor_r = 0;
        if (c->buf_cols == 0) c->cursor_c = 0;
        if (was_at_bottom) c->view_top = (int)console_max_view_top(c);
        if (c->view_top < 0) c->view_top = 0;
        {
            uint32_t max_top = console_max_view_top(c);
            if (c->view_top > (int)max_top) c->view_top = (int)max_top;
        }
        {
            uint32_t max_top = console_max_view_top(c);
            if (c->view_top > (int)max_top) c->view_top = (int)max_top;
        }
        {
            uint32_t max_top = console_max_view_top(c);
            if (c->view_top > (int)max_top) c->view_top = (int)max_top;
        }
    }
}

void console_bind_screen(void) {
    console_t *c = console_current_impl();
    c->target_root = fb_root;
    c->viewport = c->target_root;
    {
        uint32_t rows = clamp_cells_rows(c->viewport.height / (uint32_t)CHAR_HEIGHT);
        uint32_t cols = clamp_cells_cols(c->viewport.width / (uint32_t)CHAR_WIDTH);
        int was_at_bottom = (c->view_top >= (int)console_max_view_top(c));
        c->view_rows = rows;
        c->view_cols = cols;
        buffer_resize_cols(c, cols);
        if ((uint32_t)c->cursor_r >= c->buf_rows) c->cursor_r = (int)c->buf_rows - 1;
        if ((uint32_t)c->cursor_c >= c->buf_cols) c->cursor_c = (int)c->buf_cols - 1;
        if (c->buf_rows == 0) c->cursor_r = 0;
        if (c->buf_cols == 0) c->cursor_c = 0;
        if (was_at_bottom) c->view_top = (int)console_max_view_top(c);
        if (c->view_top < 0) c->view_top = 0;
    }
}

void clear_screen(void) {
    console_t *c = console_current_impl();
    if (!c->buf_rows || !c->buf_cols) return;

    buffer_clear_all_cells(c, c->buf_rows, c->buf_cols);

    c->cursor_r = 0;
    c->cursor_c = 0;
    c->content_rows = 0;
    c->view_top = 0;

    if (!c->visible || !c->viewport.address) return;

    for (uint32_t y = 0; y < c->viewport.height; ++y) {
        uint32_t *row = (uint32_t *)((uintptr_t)c->viewport.address + (uintptr_t)y * c->viewport.pitch);
        for (uint32_t x = 0; x < c->viewport.width; ++x) row[x] = 0;
    }
    wm_mark_dirty();
}

void scroll_if_needed(void) {
    console_t *c = console_current_impl();
    uint32_t max_rows = c->buf_rows;
    if (max_rows == 0) return;
    if ((uint32_t)c->cursor_r < max_rows) return;

    int was_at_bottom = (c->view_top >= (int)console_max_view_top(c));

    // Shift buffer up by one line.
    for (uint32_t r = 1; r < max_rows; ++r) {
        for (uint32_t cc = 0; cc < c->buf_cols; ++cc) {
            c->cell_ch[r - 1][cc] = c->cell_ch[r][cc];
            c->cell_color[r - 1][cc] = c->cell_color[r][cc];
        }
    }
    for (uint32_t cc = 0; cc < c->buf_cols; ++cc) {
        c->cell_ch[max_rows - 1][cc] = ' ';
        c->cell_color[max_rows - 1][cc] = 0;
    }

    c->cursor_r = (int)max_rows - 1;
    c->content_rows = max_rows;

    if (was_at_bottom) {
        c->view_top = (int)console_max_view_top(c);
    } else if (c->view_top > 0) {
        c->view_top -= 1;
    }

    if (c->visible) console_redraw(c);
}

void putchar_at(char ch, int row, int col) {
    console_t *c = console_current_impl();
    if (row < 0 || col < 0) return;
    if ((uint32_t)row >= c->buf_rows || (uint32_t)col >= c->buf_cols) return;

    c->cell_ch[row][col] = (ch == 0 ? ' ' : (uint8_t)ch);
    c->cell_color[row][col] = c->color;
    if ((uint32_t)(row + 1) > c->content_rows) c->content_rows = (uint32_t)(row + 1);

    if (!c->visible) return;
    if (row < c->view_top || row >= c->view_top + (int)c->view_rows) return;
    int draw_row = row - c->view_top;
    draw_char(c, ch == 0 ? ' ' : ch, col * CHAR_WIDTH, draw_row * CHAR_HEIGHT, c->color);
    wm_mark_dirty();
}

void putchar(char ch) {
    console_t *c = console_current_impl();
    if (!c->viewport.address) return;
    uint32_t max_cols = c->buf_cols;
    if (max_cols == 0 || c->buf_rows == 0) return;
    int was_at_bottom = (c->view_top >= (int)console_max_view_top(c));

    if (ch == '\n') {
        c->cursor_c = 0;
        c->cursor_r++;
        if ((uint32_t)(c->cursor_r + 1) > c->content_rows) c->content_rows = (uint32_t)(c->cursor_r + 1);
        scroll_if_needed();
        if (was_at_bottom) {
            int old_top = c->view_top;
            c->view_top = (int)console_max_view_top(c);
            if (old_top != c->view_top && c->visible) console_redraw(c);
        }
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
        if ((uint32_t)(c->cursor_r + 1) > c->content_rows) c->content_rows = (uint32_t)(c->cursor_r + 1);
        scroll_if_needed();
        if (was_at_bottom) {
            int old_top = c->view_top;
            c->view_top = (int)console_max_view_top(c);
            if (old_top != c->view_top && c->visible) console_redraw(c);
        }
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

    {
        int was_at_bottom = (c->view_top >= (int)console_max_view_top(c));
        c->view_rows = new_rows;
        c->view_cols = new_cols;
        buffer_resize_cols(c, new_cols);
        if ((uint32_t)c->cursor_r >= c->buf_rows) c->cursor_r = (int)c->buf_rows - 1;
        if ((uint32_t)c->cursor_c >= c->buf_cols) c->cursor_c = (int)c->buf_cols - 1;
        if (c->buf_rows == 0) c->cursor_r = 0;
        if (c->buf_cols == 0) c->cursor_c = 0;
        if (was_at_bottom) c->view_top = (int)console_max_view_top(c);
        if (c->view_top < 0) c->view_top = 0;
    }
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
    if (!c->view_rows || !c->view_cols) return;
    if (!c->viewport.address) return;
    if (!c->visible) return;

    // Черный фон в viewport.
    for (uint32_t y = 0; y < c->viewport.height; ++y) {
        uint32_t *row = (uint32_t *)((uintptr_t)c->viewport.address + (uintptr_t)y * c->viewport.pitch);
        for (uint32_t x = 0; x < c->viewport.width; ++x) row[x] = 0;
    }

    uint32_t start = (uint32_t)c->view_top;
    uint32_t end = start + c->view_rows;
    if (end > c->buf_rows) end = c->buf_rows;
    uint32_t draw_r = 0;
    for (uint32_t r = start; r < end; ++r, ++draw_r) {
        for (uint32_t cc = 0; cc < c->view_cols; ++cc) {
            uint8_t ch = c->cell_ch[r][cc];
            if (ch == 0 || ch == ' ') continue;
            draw_char(c, (char)ch, (int)cc * CHAR_WIDTH, (int)draw_r * CHAR_HEIGHT, c->cell_color[r][cc]);
        }
    }
    wm_mark_dirty();
}

void console_scroll_lines(console_t *c, int delta) {
    if (!c) c = console_current_impl();
    if (!c || c->view_rows == 0) return;
    uint32_t max_top = console_max_view_top(c);
    if (max_top == 0) {
        c->view_top = 0;
        return;
    }
    int new_top = c->view_top + delta;
    if (new_top < 0) new_top = 0;
    if (new_top > (int)max_top) new_top = (int)max_top;
    if (new_top == c->view_top) return;
    c->view_top = new_top;
    if (c->visible) console_redraw(c);
}

void console_scroll_to(console_t *c, int top) {
    if (!c) c = console_current_impl();
    if (!c || c->view_rows == 0) return;
    uint32_t max_top = console_max_view_top(c);
    if (top < 0) top = 0;
    if ((uint32_t)top > max_top) top = (int)max_top;
    if (top == c->view_top) return;
    c->view_top = top;
    if (c->visible) console_redraw(c);
}

int console_scrollbar_width_px(void) {
    return CONSOLE_SCROLLBAR_W_PX;
}

void console_get_scroll_info(console_t *c, int *out_top, int *out_visible, int *out_total) {
    if (!c) c = console_current_impl();
    if (!c) return;
    int top = c->view_top;
    int visible = (int)c->view_rows;
    int total = (int)console_total_rows(c);
    if (total < visible) total = visible;
    if (top < 0) top = 0;
    if (top > total - visible) top = total - visible;
    if (total < 0) total = 0;
    if (out_top) *out_top = top;
    if (out_visible) *out_visible = visible;
    if (out_total) *out_total = total;
}

int console_get_view_cols(console_t *c) {
    if (!c) c = console_current_impl();
    if (!c) return 0;
    return (int)c->view_cols;
}

int console_get_view_rows(console_t *c) {
    if (!c) c = console_current_impl();
    if (!c) return 0;
    return (int)c->view_rows;
}

uint8_t console_get_cell_char(console_t *c, int row, int col) {
    if (!c) c = console_current_impl();
    if (!c) return ' ';
    if (row < 0 || col < 0) return ' ';
    if ((uint32_t)row >= c->buf_rows || (uint32_t)col >= c->buf_cols) return ' ';
    return c->cell_ch[row][col];
}

uint32_t console_get_cell_color(console_t *c, int row, int col) {
    if (!c) c = console_current_impl();
    if (!c) return COLOR_DEFAULT;
    if (row < 0 || col < 0) return COLOR_DEFAULT;
    if ((uint32_t)row >= c->buf_rows || (uint32_t)col >= c->buf_cols) return COLOR_DEFAULT;
    return c->cell_color[row][col] ? c->cell_color[row][col] : COLOR_DEFAULT;
}

int console_copy_range(console_t *c, int start_row, int start_col, int end_row, int end_col, char *out, int maxlen) {
    int top = 0;
    int left = 0;
    int bottom = 0;
    int right = 0;
    int pos = 0;
    if (!c) c = console_current_impl();
    if (!c || !out || maxlen <= 0) return 0;
    out[0] = '\0';

    if (start_row < end_row || (start_row == end_row && start_col <= end_col)) {
        top = start_row;
        left = start_col;
        bottom = end_row;
        right = end_col;
    } else {
        top = end_row;
        left = end_col;
        bottom = start_row;
        right = start_col;
    }

    if (top < 0) top = 0;
    if (bottom < 0) return 0;
    if (left < 0) left = 0;
    if (right < 0) right = 0;
    if ((uint32_t)top >= c->buf_rows) return 0;
    if ((uint32_t)bottom >= c->buf_rows) bottom = (int)c->buf_rows - 1;

    for (int row = top; row <= bottom && pos < maxlen - 1; ++row) {
        int row_start = (row == top) ? left : 0;
        int row_end = (row == bottom) ? right : (int)c->buf_cols - 1;
        int last_non_space = -1;
        if (row_start < 0) row_start = 0;
        if (row_end >= (int)c->buf_cols) row_end = (int)c->buf_cols - 1;
        if (row_start > row_end) continue;

        for (int col = row_start; col <= row_end; ++col) {
            uint8_t ch = c->cell_ch[row][col];
            if (ch != 0 && ch != ' ') last_non_space = col;
        }

        if (last_non_space >= row_start) {
            for (int col = row_start; col <= last_non_space && pos < maxlen - 1; ++col) {
                uint8_t ch = c->cell_ch[row][col];
                out[pos++] = (char)(ch ? ch : ' ');
            }
        }

        if (row != bottom && pos < maxlen - 1) out[pos++] = '\n';
    }

    out[pos] = '\0';
    return pos;
}

void update_cursor(void) {
    // Software cursor not yet implemented, for now just a stub.
}


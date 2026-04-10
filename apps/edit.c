#include "sdk/mljos_api.h"
#include "sdk/mljos_app.h"

MLJOS_APP_DEFINE("Editor", MLJOS_APP_FLAG_TUI | MLJOS_APP_FLAG_GUI);

#define MAX_LINES 128
#define MAX_COLS 256
#define MAX_TABS 6

typedef struct {
    char lines[MAX_LINES][MAX_COLS];
    int line_lens[MAX_LINES];
    int num_lines;
    int cx;
    int cy;
    int scroll_y;
    char filename[128];
} editor_tab_t;

static int scroll_drag = 0;
static int scroll_drag_offset = 0;
static char temporary_msg[128];
static editor_tab_t g_tabs[MAX_TABS];
static int g_tab_count = 1;
static int g_active_tab = 0;

static int copy_str(char *dst, const char *src, int maxlen);
static int text_len(const char *text);
static int clamp_int(int v, int lo, int hi);
static editor_tab_t *active_tab(void);
static void init_tab(editor_tab_t *tab);
static void create_tab(const char *path);
static void close_tab_if_possible(void);
static void copy_tab(editor_tab_t *dst, const editor_tab_t *src);
static void insert_text_at_cursor(editor_tab_t *tab, const char *text, int *dirty);

static editor_tab_t *active_tab(void) {
    if (g_active_tab < 0) g_active_tab = 0;
    if (g_active_tab >= g_tab_count) g_active_tab = g_tab_count - 1;
    return &g_tabs[g_active_tab];
}

static void init_tab(editor_tab_t *tab) {
    if (!tab) return;
    tab->num_lines = 1;
    tab->cx = 0;
    tab->cy = 0;
    tab->scroll_y = 0;
    tab->filename[0] = '\0';
    for (int i = 0; i < MAX_LINES; i++) {
        tab->line_lens[i] = 0;
        for (int j = 0; j < MAX_COLS; j++) tab->lines[i][j] = '\0';
    }
}

static void load_file(mljos_api_t *api, editor_tab_t *tab, const char *path) {
    if (!tab) return;
    if (!path || !path[0]) return;
    init_tab(tab);
    copy_str(tab->filename, path, sizeof(tab->filename));

    char buffer[4096];
    unsigned int size_out = 0;
    if (api->read_file(path, buffer, sizeof(buffer), &size_out)) {
        tab->num_lines = 0;

        int p = 0;
        while (buffer[p] && tab->num_lines < MAX_LINES) {
            int cur_line = tab->num_lines;
            int cur_col = 0;

            while (buffer[p] && buffer[p] != '\n' && cur_col < MAX_COLS - 1) {
                tab->lines[cur_line][cur_col++] = buffer[p++];
            }
            tab->lines[cur_line][cur_col] = '\0';
            tab->line_lens[cur_line] = cur_col;
            tab->num_lines++;

            if (buffer[p] == '\n') p++;
        }
        if (tab->num_lines == 0) {
            tab->num_lines = 1;
            tab->line_lens[0] = 0;
        }
    } else {
        copy_str(temporary_msg, "Error opening file!", sizeof(temporary_msg));
        init_tab(tab);
    }
}

static void save_file(mljos_api_t *api) {
    editor_tab_t *tab = active_tab();
    char new_name[128];
    int ok = 0;
    if (api->ui && api->launch_flags & MLJOS_LAUNCH_GUI) {
        ok = api->ui->prompt_input("Save File", tab->filename[0] ? "Enter file name (empty=cancel):" : "Enter file name:", new_name, sizeof(new_name));
    } else {
        api->puts("\nSave to: ");
        int len = api->read_line(new_name, sizeof(new_name));
        ok = (len > 0);
    }

    if (ok && new_name[0]) {
        copy_str(tab->filename, new_name, sizeof(tab->filename));
    }

    if (tab->filename[0]) {
        char buffer[4096];
        int bpos = 0;
        for (int l = 0; l < tab->num_lines; l++) {
            for (int c = 0; c < tab->line_lens[l]; c++) {
                if (bpos < 4095) buffer[bpos++] = tab->lines[l][c];
            }
            if (l < tab->num_lines - 1 && bpos < 4095) buffer[bpos++] = '\n';
        }
        buffer[bpos] = '\0';
        if (api->write_file(tab->filename, buffer, bpos)) {
            copy_str(temporary_msg, "File saved successfully.", sizeof(temporary_msg));
        } else {
            copy_str(temporary_msg, "Error saving file!", sizeof(temporary_msg));
        }
    } else {
        copy_str(temporary_msg, "Save cancelled.", sizeof(temporary_msg));
    }
}

static int view_cols(mljos_api_t *api) {
    if (api && api->tui_cols) {
        int c = api->tui_cols();
        if (c > 0) return c;
    }
    return 80;
}

static int view_rows(mljos_api_t *api) {
    if (api && api->tui_rows) {
        int r = api->tui_rows();
        if (r > 0) return r;
    }
    return 25;
}

static void draw_tui_status(mljos_api_t *api) {
    editor_tab_t *tab = active_tab();
    int cols = view_cols(api);
    int status_row = view_rows(api) > 0 ? view_rows(api) - 1 : 0;
    for (int i = 0; i < cols; i++) api->putchar_at(' ', status_row, i);
    
    if (temporary_msg[0]) {
        for (int i = 0; temporary_msg[i] && i < cols; i++) api->putchar_at(temporary_msg[i], status_row, i);
        temporary_msg[0] = '\0';
        return;
    }
    
    char status[256];
    int pos = 0;
    const char *t = "mljOS Edit - ";
    while (*t && pos < 255) status[pos++] = *t++;
    char *fn = tab->filename[0] ? tab->filename : "(new file)";
    while (*fn && pos < 255) status[pos++] = *fn++;
    const char *c = " - Ctrl+O Save - Ctrl+X Exit";
    while (*c && pos < 255) status[pos++] = *c++;
    status[pos] = '\0';
    
    for (int i = 0; i < pos && i < cols; i++) api->putchar_at(status[i], status_row, i);
}

static void draw_tui_screen(mljos_api_t *api) {
    editor_tab_t *tab = active_tab();
    int cols = view_cols(api);
    int text_rows = view_rows(api) > 1 ? view_rows(api) - 1 : 1;

    api->clear_screen();
    for (int i = 0; i < text_rows; i++) {
        int l = tab->scroll_y + i;
        if (l < tab->num_lines) {
            for (int j = 0; j < tab->line_lens[l] && j < cols; j++) {
                api->putchar_at(tab->lines[l][j], i, j);
            }
        }
    }
    draw_tui_status(api);
}

static void handle_key(mljos_api_t *api, int key, int *dirty) {
    editor_tab_t *tab = active_tab();
    if (key == 15) { // Ctrl+O
        save_file(api);
        *dirty = 1;
    } else if (key == 3) { // Ctrl+C
        char line[MAX_COLS];
        int pos = 0;
        for (int i = 0; i < tab->line_lens[tab->cy] && pos < MAX_COLS - 1; ++i) line[pos++] = tab->lines[tab->cy][i];
        line[pos] = '\0';
        if (api->clipboard_set) {
            api->clipboard_set(line);
            copy_str(temporary_msg, "Current line copied.", sizeof(temporary_msg));
            *dirty = 1;
        }
    } else if (key == 1000) { // UP
        if (tab->cy > 0) { tab->cy--; if (tab->cx > tab->line_lens[tab->cy]) tab->cx = tab->line_lens[tab->cy]; }
        if (tab->cy < tab->scroll_y) tab->scroll_y = tab->cy;
        *dirty = 1;
    } else if (key == 1001) { // DOWN
        if (tab->cy < tab->num_lines - 1) { tab->cy++; if (tab->cx > tab->line_lens[tab->cy]) tab->cx = tab->line_lens[tab->cy]; }
        *dirty = 1;
    } else if (key == 1002) { // LEFT
        if (tab->cx > 0) { tab->cx--; }
        else if (tab->cy > 0) { tab->cy--; tab->cx = tab->line_lens[tab->cy]; }
        if (tab->cy < tab->scroll_y) tab->scroll_y = tab->cy;
        *dirty = 1;
    } else if (key == 1003) { // RIGHT
        if (tab->cx < tab->line_lens[tab->cy]) { tab->cx++; }
        else if (tab->cy < tab->num_lines - 1) { tab->cy++; tab->cx = 0; }
        *dirty = 1;
    } else if (key == '\n' || key == '\r') {
        if (tab->num_lines < MAX_LINES) {
            for (int i = tab->num_lines; i > tab->cy + 1; i--) {
                tab->line_lens[i] = tab->line_lens[i - 1];
                for (int j = 0; j < tab->line_lens[i]; j++) tab->lines[i][j] = tab->lines[i - 1][j];
            }
            tab->line_lens[tab->cy + 1] = tab->line_lens[tab->cy] - tab->cx;
            for (int j = 0; j < tab->line_lens[tab->cy + 1]; j++) tab->lines[tab->cy + 1][j] = tab->lines[tab->cy][tab->cx + j];
            tab->line_lens[tab->cy] = tab->cx;
            tab->num_lines++; tab->cy++; tab->cx = 0;
            *dirty = 1;
        }
    } else if (key == '\b' || key == 8) {
        if (tab->cx > 0) {
            for (int j = tab->cx; j < tab->line_lens[tab->cy]; j++) tab->lines[tab->cy][j - 1] = tab->lines[tab->cy][j];
            tab->line_lens[tab->cy]--; tab->cx--;
            *dirty = 1;
        } else if (tab->cy > 0) {
            int old_len = tab->line_lens[tab->cy - 1];
            if (old_len + tab->line_lens[tab->cy] < MAX_COLS) {
                for (int j = 0; j < tab->line_lens[tab->cy]; j++) tab->lines[tab->cy - 1][old_len + j] = tab->lines[tab->cy][j];
                tab->line_lens[tab->cy - 1] += tab->line_lens[tab->cy];
                for (int i = tab->cy; i < tab->num_lines - 1; i++) {
                    tab->line_lens[i] = tab->line_lens[i + 1];
                    for (int j = 0; j < tab->line_lens[i]; j++) tab->lines[i][j] = tab->lines[i + 1][j];
                }
                tab->num_lines--; tab->cy--; tab->cx = old_len;
                if (tab->cy < tab->scroll_y) tab->scroll_y = tab->cy;
                *dirty = 1;
            }
        }
    } else if (key == 22) { // Ctrl+V
        char clip[1024];
        if (api->clipboard_get && api->clipboard_get(clip, sizeof(clip)) > 0) {
            insert_text_at_cursor(tab, clip, dirty);
        }
    } else if (key >= 32 && key <= 126) { // Printable chars
        if (tab->line_lens[tab->cy] < MAX_COLS - 1) {
            for (int j = tab->line_lens[tab->cy]; j > tab->cx; j--) tab->lines[tab->cy][j] = tab->lines[tab->cy][j - 1];
            tab->lines[tab->cy][tab->cx] = (char)key;
            tab->line_lens[tab->cy]++; tab->cx++;
            *dirty = 1;
        }
    }
}

static void create_tab(const char *path) {
    if (g_tab_count >= MAX_TABS) return;
    init_tab(&g_tabs[g_tab_count]);
    if (path && path[0]) copy_str(g_tabs[g_tab_count].filename, path, sizeof(g_tabs[g_tab_count].filename));
    g_active_tab = g_tab_count;
    g_tab_count++;
}

static void close_tab_if_possible(void) {
    int index = g_active_tab;
    if (g_tab_count <= 1) return;
    if (index < 0 || index >= g_tab_count) return;
    for (int i = index; i < g_tab_count - 1; i++) {
        copy_tab(&g_tabs[i], &g_tabs[i + 1]);
    }
    g_tab_count--;
    if (index < g_active_tab) g_active_tab--;
    if (g_active_tab >= g_tab_count) g_active_tab = g_tab_count - 1;
}

static void copy_tab(editor_tab_t *dst, const editor_tab_t *src) {
    if (!dst || !src) return;
    for (int i = 0; i < MAX_LINES; i++) {
        dst->line_lens[i] = src->line_lens[i];
        for (int j = 0; j < MAX_COLS; j++) dst->lines[i][j] = src->lines[i][j];
    }
    dst->num_lines = src->num_lines;
    dst->cx = src->cx;
    dst->cy = src->cy;
    dst->scroll_y = src->scroll_y;
    copy_str(dst->filename, src->filename, sizeof(dst->filename));
}

MLJOS_APP_ENTRY void _start(mljos_api_t *api) {
    g_tab_count = 1;
    g_active_tab = 0;
    for (int i = 0; i < MAX_TABS; i++) init_tab(&g_tabs[i]);
    temporary_msg[0] = '\0';

    if (api && api->open_path && api->open_path[0]) {
        load_file(api, &g_tabs[0], api->open_path);
    }

    if (api && (api->launch_flags & MLJOS_LAUNCH_GUI) && api->ui) {
        api->ui->begin_app("Editor");
        
        int dirty = 1;
        
        uint32_t col_bg = 0x1E1E1E;
        uint32_t col_panel = 0x252526;
        uint32_t col_text = 0xD4D4D4;
        uint32_t col_cursor = 0x007ACC;
        
        for (;;) {
            editor_tab_t *tab = active_tab();
            int w = (int)api->ui->screen_w();
            int h = (int)api->ui->screen_h();
            int tabs_h = 28;
            int top_h = 58;
            int bot_h = 24;
            int lh = 16;
            int scrollbar_w = 12;
            int left_pad = 10;
            int right_pad = 10;
            int text_x = left_pad;
            int text_w = w - left_pad - right_pad - scrollbar_w;
            if (text_w < 8) text_w = 8;
            int max_cols = text_w / 8;
            if (max_cols < 1) max_cols = 1;
            
            // Adjust scroll based on cursor height smoothly
            int max_visible = (h - top_h - bot_h) / lh;
            if (max_visible < 1) max_visible = 1;
            int max_scroll = tab->num_lines - max_visible;
            if (max_scroll < 0) max_scroll = 0;
            if (tab->cy < tab->scroll_y) tab->scroll_y = tab->cy;
            if (tab->cy >= tab->scroll_y + max_visible) tab->scroll_y = tab->cy - max_visible + 1;
            tab->scroll_y = clamp_int(tab->scroll_y, 0, max_scroll);
            
            if (dirty) {
                api->ui->fill_rect(0, 0, w, h, col_bg);
                api->ui->fill_rect(0, 0, w, tabs_h, 0x1B1B1B);
                api->ui->fill_rect(0, tabs_h, w, top_h - tabs_h, col_panel);
                api->ui->fill_rect(0, h - bot_h, w, bot_h, 0x007ACC);

                int tab_x = 8;
                for (int i = 0; i < g_tab_count; i++) {
                    int tab_w = 120;
                    api->ui->fill_rect(tab_x, 4, tab_w, tabs_h - 8, i == g_active_tab ? 0x007ACC : 0x333333);
                    api->ui->draw_text(g_tabs[i].filename[0] ? g_tabs[i].filename : "(new)", tab_x + 8, 8, 0xFFFFFF);
                    if (g_tab_count > 1) {
                        api->ui->fill_rect(tab_x + tab_w - 20, 7, 12, 12, 0x7A1E1E);
                        api->ui->draw_text("x", tab_x + tab_w - 16, 8, 0xFFFFFF);
                    }
                    tab_x += tab_w + 6;
                }
                if (g_tab_count < MAX_TABS) {
                    api->ui->fill_rect(tab_x, 4, 26, tabs_h - 8, 0x333333);
                    api->ui->draw_text("+", tab_x + 9, 8, 0xFFFFFF);
                }

                api->ui->draw_text("Save (Ctrl+O)", 10, tabs_h + 8, 0xFFFFFF);
                api->ui->draw_text("Exit (Ctrl+X)", 140, tabs_h + 8, 0xFFFFFF);
                
                for (int i = 0; i < max_visible; i++) {
                    int l = tab->scroll_y + i;
                    if (l >= tab->num_lines) break;
                    
                    int y = top_h + (i * lh);
                    for (int j = 0; j < tab->line_lens[l] && j < max_cols; j++) {
                        char cc[2] = { tab->lines[l][j], 0 };
                        api->ui->draw_text(cc, text_x + j * 8, y + 2, col_text);
                    }
                    
                    if (l == tab->cy) {
                        if (tab->cx >= 0 && tab->cx < max_cols) {
                            api->ui->fill_rect(text_x + tab->cx * 8, y + 14, 8, 2, col_cursor);
                        }
                    }
                }

                // Scrollbar
                int sb_x = w - right_pad - scrollbar_w;
                int track_y = top_h;
                int track_h = h - top_h - bot_h;
                if (track_h < 1) track_h = 1;
                api->ui->fill_rect(sb_x, track_y, scrollbar_w, track_h, 0x1A1B1E);
                if (max_scroll > 0) {
                    int thumb_h = (track_h * max_visible) / tab->num_lines;
                    if (thumb_h < 16) thumb_h = 16;
                    if (thumb_h > track_h) thumb_h = track_h;
                    int thumb_y = track_y + (int)((uint64_t)(track_h - thumb_h) * (uint64_t)tab->scroll_y / (uint64_t)max_scroll);
                    api->ui->fill_rect(sb_x + 1, thumb_y, scrollbar_w - 2, thumb_h, 0x3C3C3C);
                } else {
                    api->ui->fill_rect(sb_x + 1, track_y + 1, scrollbar_w - 2, track_h - 2, 0x3C3C3C);
                }
                
                // Bottom status
                if (temporary_msg[0]) {
                    api->ui->draw_text(temporary_msg, 10, h - bot_h + 5, 0xFFFFFF);
                    temporary_msg[0] = '\0';
                } else {
                    char s[128];
                    copy_str(s, tab->filename[0] ? tab->filename : "(new file)", sizeof(s));
                    api->ui->draw_text(s, 10, h - bot_h + 5, 0xFFFFFF);
                }
                dirty = 0;
            }
            
            mljos_ui_event_t ev;
            while (api->ui->poll_event(&ev)) {
                if (ev.type == MLJOS_UI_EVENT_EXPOSE) dirty = 1;
                
                if (ev.type == MLJOS_UI_EVENT_MOUSE_LEFT_DOWN) {
                    if (ev.y <= tabs_h) {
                        int tab_x = 8;
                        for (int i = 0; i < g_tab_count; i++) {
                            if (g_tab_count > 1 && ev.x >= tab_x + 100 && ev.x <= tab_x + 112) {
                                g_active_tab = i;
                                close_tab_if_possible();
                                scroll_drag = 0;
                                dirty = 1;
                                break;
                            }
                            if (ev.x >= tab_x && ev.x <= tab_x + 120) {
                                g_active_tab = i;
                                scroll_drag = 0;
                                dirty = 1;
                                break;
                            }
                            tab_x += 126;
                        }
                        if (g_tab_count < MAX_TABS && ev.x >= tab_x && ev.x <= tab_x + 26) {
                            create_tab(NULL);
                            dirty = 1;
                        }
                    } else if (ev.y <= top_h) {
                        if (ev.x >= 5 && ev.x <= 120 && ev.y >= tabs_h) {
                            save_file(api);
                            dirty = 1;
                        } else if (ev.x >= 135 && ev.x <= 250 && ev.y >= tabs_h) {
                            api->ui->end_app();
                            return;
                        }
                    } else if (ev.y >= top_h && ev.y <= h - bot_h) {
                        int sb_x = w - right_pad - scrollbar_w;
                        int track_y = top_h;
                        int track_h = h - top_h - bot_h;
                        if (track_h < 1) track_h = 1;
                        int max_scroll = tab->num_lines - max_visible;
                        if (max_scroll < 0) max_scroll = 0;

                        if (ev.x >= sb_x && ev.x < sb_x + scrollbar_w) {
                            if (max_scroll > 0) {
                                int thumb_h = (track_h * max_visible) / tab->num_lines;
                                if (thumb_h < 16) thumb_h = 16;
                                if (thumb_h > track_h) thumb_h = track_h;
                                int thumb_y = track_y + (int)((uint64_t)(track_h - thumb_h) * (uint64_t)tab->scroll_y / (uint64_t)max_scroll);
                                if (ev.y >= thumb_y && ev.y < thumb_y + thumb_h) {
                                    scroll_drag = 1;
                                    scroll_drag_offset = ev.y - thumb_y;
                                } else if (ev.y < thumb_y) {
                                    tab->scroll_y -= max_visible;
                                } else if (ev.y > thumb_y + thumb_h) {
                                    tab->scroll_y += max_visible;
                                }
                                tab->scroll_y = clamp_int(tab->scroll_y, 0, max_scroll);
                                dirty = 1;
                            }
                            continue;
                        }

                        // Click on text repositions cursor
                        int r = (ev.y - top_h) / lh;
                        int c = (ev.x - text_x) / 8;
                        if (c < 0) c = 0;
                        int clicked_line = tab->scroll_y + r;
                        if (clicked_line < tab->num_lines) {
                            tab->cy = clicked_line;
                            if (c > tab->line_lens[tab->cy]) tab->cx = tab->line_lens[tab->cy];
                            else tab->cx = c;
                            dirty = 1;
                        }
                    }
                }

                if (ev.type == MLJOS_UI_EVENT_MOUSE_LEFT_UP) {
                    scroll_drag = 0;
                }

                if (ev.type == MLJOS_UI_EVENT_MOUSE_MOVE) {
                    if (scroll_drag) {
                        int track_y = top_h;
                        int track_h = h - top_h - bot_h;
                        if (track_h < 1) track_h = 1;
                        int max_scroll = tab->num_lines - max_visible;
                        if (max_scroll < 0) max_scroll = 0;
                        if (max_scroll > 0) {
                            int thumb_h = (track_h * max_visible) / tab->num_lines;
                            if (thumb_h < 16) thumb_h = 16;
                            if (thumb_h > track_h) thumb_h = track_h;
                            if (track_h > thumb_h) {
                                int max_thumb_y = track_y + track_h - thumb_h;
                                int new_thumb_y = ev.y - scroll_drag_offset;
                                if (new_thumb_y < track_y) new_thumb_y = track_y;
                                if (new_thumb_y > max_thumb_y) new_thumb_y = max_thumb_y;
                                tab->scroll_y = (int)((uint64_t)(new_thumb_y - track_y) * (uint64_t)max_scroll / (uint64_t)(track_h - thumb_h));
                                tab->scroll_y = clamp_int(tab->scroll_y, 0, max_scroll);
                                dirty = 1;
                            }
                        }
                    }
                }

                if (ev.type == MLJOS_UI_EVENT_MOUSE_WHEEL) {
                    int max_scroll = tab->num_lines - max_visible;
                    if (max_scroll < 0) max_scroll = 0;
                    if (max_scroll > 0) {
                        tab->scroll_y -= ev.key * 3;
                        tab->scroll_y = clamp_int(tab->scroll_y, 0, max_scroll);
                        dirty = 1;
                    }
                }
                
                if (ev.type == MLJOS_UI_EVENT_KEY_DOWN) {
                    if (ev.key == 20) { // Ctrl+T
                        create_tab(NULL);
                        dirty = 1;
                        continue;
                    }
                    if (ev.key == 24) { // Ctrl+X
                        api->ui->end_app();
                        return;
                    }
                    handle_key(api, ev.key, &dirty);
                }
            }
        }
    } else {
        // TUI Mode
        draw_tui_screen(api);
        while (1) {
            int cols = view_cols(api);
            editor_tab_t *tab = active_tab();
            int cur_col = tab->cx;
            if (cols > 0 && cur_col >= cols) cur_col = cols - 1;
            if (cur_col < 0) cur_col = 0;
            api->set_cursor(tab->cy - tab->scroll_y, cur_col);
            
            int key = api->read_key();
            if (key == 24) break; // Ctrl+X
            
            int dirty = 0;
            handle_key(api, key, &dirty);
            if (dirty) draw_tui_screen(api);
        }
        api->clear_screen();
        api->set_cursor(0, 0);
        api->puts("Leaving mljOS Edit\n");
    }
}

static int copy_str(char *dst, const char *src, int maxlen) {
    int i = 0;
    while (src[i] && i < maxlen - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return i;
}

static int text_len(const char *text) {
    int len = 0;
    while (text[len]) len++;
    return len;
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void insert_text_at_cursor(editor_tab_t *tab, const char *text, int *dirty) {
    if (!tab || !text) return;
    for (int i = 0; text[i]; ++i) {
        char ch = text[i];
        if (ch == '\r') continue;
        if (ch == '\n') {
            if (tab->num_lines >= MAX_LINES) break;
            for (int row = tab->num_lines; row > tab->cy + 1; --row) {
                tab->line_lens[row] = tab->line_lens[row - 1];
                for (int col = 0; col < MAX_COLS; ++col) tab->lines[row][col] = tab->lines[row - 1][col];
            }
            tab->line_lens[tab->cy + 1] = tab->line_lens[tab->cy] - tab->cx;
            for (int col = 0; col < tab->line_lens[tab->cy + 1]; ++col) tab->lines[tab->cy + 1][col] = tab->lines[tab->cy][tab->cx + col];
            tab->lines[tab->cy][tab->cx] = '\0';
            tab->line_lens[tab->cy] = tab->cx;
            tab->num_lines++;
            tab->cy++;
            tab->cx = 0;
            if (dirty) *dirty = 1;
            continue;
        }
        if (ch < 32 || ch > 126) continue;
        if (tab->line_lens[tab->cy] >= MAX_COLS - 1) continue;
        for (int col = tab->line_lens[tab->cy]; col > tab->cx; --col) tab->lines[tab->cy][col] = tab->lines[tab->cy][col - 1];
        tab->lines[tab->cy][tab->cx] = ch;
        tab->line_lens[tab->cy]++;
        tab->cx++;
        if (dirty) *dirty = 1;
    }
}

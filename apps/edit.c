#include "sdk/mljos_api.h"
#include "sdk/mljos_app.h"

MLJOS_APP_DEFINE("Editor", MLJOS_APP_FLAG_TUI | MLJOS_APP_FLAG_GUI);

#define MAX_LINES 128
#define MAX_COLS 256

static char lines[MAX_LINES][MAX_COLS];
static int line_lens[MAX_LINES];
static int num_lines = 1;
static int cx = 0, cy = 0;
static int scroll_y = 0;
static char filename[128];
static char temporary_msg[128];

static int copy_str(char *dst, const char *src, int maxlen);
static int text_len(const char *text);

static void load_file(mljos_api_t *api, const char *path) {
    if (!path || !path[0]) return;
    copy_str(filename, path, sizeof(filename));

    char buffer[4096];
    unsigned int size_out = 0;
    if (api->read_file(path, buffer, sizeof(buffer), &size_out)) {
        num_lines = 0;
        cx = 0;
        cy = 0;
        scroll_y = 0;

        int p = 0;
        while (buffer[p] && num_lines < MAX_LINES) {
            int cur_line = num_lines;
            int cur_col = 0;

            while (buffer[p] && buffer[p] != '\n' && cur_col < MAX_COLS - 1) {
                lines[cur_line][cur_col++] = buffer[p++];
            }
            lines[cur_line][cur_col] = '\0';
            line_lens[cur_line] = cur_col;
            num_lines++;

            if (buffer[p] == '\n') p++;
        }
        if (num_lines == 0) {
            num_lines = 1;
            line_lens[0] = 0;
        }
    } else {
        copy_str(temporary_msg, "Error opening file!", sizeof(temporary_msg));
        num_lines = 1;
        line_lens[0] = 0;
    }
}

static void save_file(mljos_api_t *api) {
    char new_name[128];
    char prompt_text[256];
    copy_str(prompt_text, "Save to: ", sizeof(prompt_text));

    int ok = 0;
    if (api->ui && api->launch_flags & MLJOS_LAUNCH_GUI) {
        ok = api->ui->prompt_input("Save File", filename[0] ? "Enter file name (empty=cancel):" : "Enter file name:", new_name, sizeof(new_name));
    } else {
        // TUI fallback
        api->puts("\nSave to: ");
        int len = api->read_line(new_name, sizeof(new_name));
        ok = (len > 0);
    }

    if (ok && new_name[0]) {
        copy_str(filename, new_name, sizeof(filename));
    }

    if (filename[0]) {
        char buffer[4096];
        int bpos = 0;
        for (int l = 0; l < num_lines; l++) {
            for (int c = 0; c < line_lens[l]; c++) {
                if (bpos < 4095) buffer[bpos++] = lines[l][c];
            }
            if (l < num_lines - 1 && bpos < 4095) buffer[bpos++] = '\n';
        }
        buffer[bpos] = '\0';
        if (api->write_file(filename, buffer, bpos)) {
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
    char *fn = filename[0] ? filename : "(new file)";
    while (*fn && pos < 255) status[pos++] = *fn++;
    const char *c = " - Ctrl+O Save - Ctrl+X Exit";
    while (*c && pos < 255) status[pos++] = *c++;
    status[pos] = '\0';
    
    for (int i = 0; i < pos && i < cols; i++) api->putchar_at(status[i], status_row, i);
}

static void draw_tui_screen(mljos_api_t *api) {
    int cols = view_cols(api);
    int text_rows = view_rows(api) > 1 ? view_rows(api) - 1 : 1;

    api->clear_screen();
    for (int i = 0; i < text_rows; i++) {
        int l = scroll_y + i;
        if (l < num_lines) {
            for (int j = 0; j < line_lens[l] && j < cols; j++) {
                api->putchar_at(lines[l][j], i, j);
            }
        }
    }
    draw_tui_status(api);
}

static void handle_key(mljos_api_t *api, int key, int *dirty) {
    if (key == 15) { // Ctrl+O
        save_file(api);
        *dirty = 1;
    } else if (key == 1000) { // UP
        if (cy > 0) { cy--; if (cx > line_lens[cy]) cx = line_lens[cy]; }
        if (cy < scroll_y) scroll_y = cy;
        *dirty = 1;
    } else if (key == 1001) { // DOWN
        if (cy < num_lines - 1) { cy++; if (cx > line_lens[cy]) cx = line_lens[cy]; }
        *dirty = 1;
    } else if (key == 1002) { // LEFT
        if (cx > 0) { cx--; }
        else if (cy > 0) { cy--; cx = line_lens[cy]; }
        if (cy < scroll_y) scroll_y = cy;
        *dirty = 1;
    } else if (key == 1003) { // RIGHT
        if (cx < line_lens[cy]) { cx++; }
        else if (cy < num_lines - 1) { cy++; cx = 0; }
        *dirty = 1;
    } else if (key == '\n' || key == '\r') {
        if (num_lines < MAX_LINES) {
            for (int i = num_lines; i > cy + 1; i--) {
                line_lens[i] = line_lens[i - 1];
                for (int j = 0; j < line_lens[i]; j++) lines[i][j] = lines[i - 1][j];
            }
            line_lens[cy + 1] = line_lens[cy] - cx;
            for (int j = 0; j < line_lens[cy + 1]; j++) lines[cy + 1][j] = lines[cy][cx + j];
            line_lens[cy] = cx;
            num_lines++; cy++; cx = 0;
            *dirty = 1;
        }
    } else if (key == '\b' || key == 8) {
        if (cx > 0) {
            for (int j = cx; j < line_lens[cy]; j++) lines[cy][j - 1] = lines[cy][j];
            line_lens[cy]--; cx--;
            *dirty = 1;
        } else if (cy > 0) {
            int old_len = line_lens[cy - 1];
            if (old_len + line_lens[cy] < MAX_COLS) {
                for (int j = 0; j < line_lens[cy]; j++) lines[cy - 1][old_len + j] = lines[cy][j];
                line_lens[cy - 1] += line_lens[cy];
                for (int i = cy; i < num_lines - 1; i++) {
                    line_lens[i] = line_lens[i + 1];
                    for (int j = 0; j < line_lens[i]; j++) lines[i][j] = lines[i + 1][j];
                }
                num_lines--; cy--; cx = old_len;
                if (cy < scroll_y) scroll_y = cy;
                *dirty = 1;
            }
        }
    } else if (key >= 32 && key <= 126) { // Printable chars
        if (line_lens[cy] < MAX_COLS - 1) {
            for (int j = line_lens[cy]; j > cx; j--) lines[cy][j] = lines[cy][j - 1];
            lines[cy][cx] = (char)key;
            line_lens[cy]++; cx++;
            *dirty = 1;
        }
    }
}

MLJOS_APP_ENTRY void _start(mljos_api_t *api) {
    num_lines = 1;
    for (int i = 0; i < MAX_LINES; i++) {
        line_lens[i] = 0;
        for (int j = 0; j < MAX_COLS; j++) lines[i][j] = '\0';
    }
    cx = 0; cy = 0; scroll_y = 0;
    filename[0] = '\0'; temporary_msg[0] = '\0';

    if (api && api->open_path && api->open_path[0]) {
        load_file(api, api->open_path);
    }

    if (api && (api->launch_flags & MLJOS_LAUNCH_GUI) && api->ui) {
        api->ui->begin_app("Editor");
        
        int dirty = 1;
        
        uint32_t col_bg = 0x1E1E1E;
        uint32_t col_panel = 0x252526;
        uint32_t col_text = 0xD4D4D4;
        uint32_t col_cursor = 0x007ACC;
        
        for (;;) {
            int w = (int)api->ui->screen_w();
            int h = (int)api->ui->screen_h();
            int top_h = 30;
            int bot_h = 24;
            int lh = 16;
            
            // Adjust scroll based on cursor height smoothly
            int max_visible = (h - top_h - bot_h) / lh;
            if (cy < scroll_y) scroll_y = cy;
            if (cy >= scroll_y + max_visible) scroll_y = cy - max_visible + 1;
            
            if (dirty) {
                // Background
                api->ui->fill_rect(0, 0, w, h, col_bg);
                // Top bar
                api->ui->fill_rect(0, 0, w, top_h, col_panel);
                // Bottom bar
                api->ui->fill_rect(0, h - bot_h, w, bot_h, 0x007ACC);

                // Top toolbar tools
                api->ui->draw_text("Save (Ctrl+O)", 10, 8, 0xFFFFFF);
                api->ui->draw_text("Exit (Ctrl+X)", 140, 8, 0xFFFFFF);
                
                // Draw text lines
                for (int i = 0; i < max_visible; i++) {
                    int l = scroll_y + i;
                    if (l >= num_lines) break;
                    
                    int y = top_h + (i * lh);
                    // Single char rendering for proper cursor calc
                    for (int j = 0; j < line_lens[l]; j++) {
                        char cc[2] = { lines[l][j], 0 };
                        api->ui->draw_text(cc, 10 + j * 8, y + 2, col_text);
                    }
                    
                    // Draw cursor
                    if (l == cy) {
                        api->ui->fill_rect(10 + cx * 8, y + 14, 8, 2, col_cursor);
                    }
                }
                
                // Bottom status
                if (temporary_msg[0]) {
                    api->ui->draw_text(temporary_msg, 10, h - bot_h + 5, 0xFFFFFF);
                    temporary_msg[0] = '\0';
                } else {
                    char s[128];
                    copy_str(s, filename[0] ? filename : "(new file)", sizeof(s));
                    api->ui->draw_text(s, 10, h - bot_h + 5, 0xFFFFFF);
                }
                dirty = 0;
            }
            
            mljos_ui_event_t ev;
            while (api->ui->poll_event(&ev)) {
                if (ev.type == MLJOS_UI_EVENT_EXPOSE) dirty = 1;
                
                if (ev.type == MLJOS_UI_EVENT_MOUSE_LEFT_DOWN) {
                    // Clicking top toolbar
                    if (ev.y <= top_h) {
                        if (ev.x >= 5 && ev.x <= 120) {
                            save_file(api);
                            dirty = 1;
                        } else if (ev.x >= 135 && ev.x <= 250) {
                            api->ui->end_app();
                            return;
                        }
                    } else if (ev.y >= top_h && ev.y <= h - bot_h) {
                        // Click on text repositions cursor
                        int r = (ev.y - top_h) / lh;
                        int c = (ev.x - 10) / 8;
                        if (c < 0) c = 0;
                        int clicked_line = scroll_y + r;
                        if (clicked_line < num_lines) {
                            cy = clicked_line;
                            if (c > line_lens[cy]) cx = line_lens[cy];
                            else cx = c;
                            dirty = 1;
                        }
                    }
                }
                
                if (ev.type == MLJOS_UI_EVENT_KEY_DOWN) {
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
            int cur_col = cx;
            if (cols > 0 && cur_col >= cols) cur_col = cols - 1;
            if (cur_col < 0) cur_col = 0;
            api->set_cursor(cy - scroll_y, cur_col);
            
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

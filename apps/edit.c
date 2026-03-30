#include "sdk/mljos_api.h"
#include "sdk/mljos_app.h"

MLJOS_APP_DEFINE("Editor", MLJOS_APP_FLAG_TUI);

#define MAX_LINES 128
#define MAX_COLS 256

static char lines[MAX_LINES][MAX_COLS];
static int line_lens[MAX_LINES];
static int num_lines = 1;
static int cx = 0, cy = 0;
static int scroll_y = 0;
static char filename[128];
static char temporary_msg[128];

/* Forward declarations so that _start is the very first compiled function */
static int copy_str(char *dst, const char *src, int maxlen);
static int text_len(const char *text);
static int view_cols(mljos_api_t *api);
static int view_rows(mljos_api_t *api);
static void draw_status(mljos_api_t *api);
static void draw_screen(mljos_api_t *api);
static void prompt_save(mljos_api_t *api);

MLJOS_APP_ENTRY void _start(mljos_api_t *api) {
    // Initialization
    num_lines = 1;
    for (int i = 0; i < MAX_LINES; i++) {
        line_lens[i] = 0;
        for (int j = 0; j < MAX_COLS; j++) lines[i][j] = '\0';
    }
    cx = 0;
    cy = 0;
    scroll_y = 0;
    filename[0] = '\0';
    temporary_msg[0] = '\0';

    if (api && api->open_path && api->open_path[0]) {
        // Show the opened path in the UI so Ctrl+O can save back to it.
        copy_str(filename, api->open_path, sizeof(filename));

        char buffer[4096];
        unsigned int size_out = 0;
        buffer[0] = '\0';

        if (api->read_file(api->open_path, buffer, sizeof(buffer), &size_out)) {
            // Reset editor buffer, then split on '\n'.
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

                if (buffer[p] == '\n') p++; // Skip newline between lines
            }

            if (num_lines == 0) {
                num_lines = 1;
                line_lens[0] = 0;
            }
        } else {
            copy_str(temporary_msg, "Error opening file!", sizeof(temporary_msg));
            num_lines = 1; // Keep editor empty
        }
    }

    draw_screen(api);

    while (1) {
        {
            int cols = view_cols(api);
            int cur_col = cx;
            if (cols > 0 && cur_col >= cols) cur_col = cols - 1;
            if (cur_col < 0) cur_col = 0;
            api->set_cursor(cy - scroll_y, cur_col);
        }
        int key = api->read_key();

        if (key == 24) { // Ctrl+X
            break; 
        } else if (key == 15) { // Ctrl+O
            prompt_save(api);
        } else if (key == 1000) { // UP
            if (cy > 0) {
                cy--;
                if (cx > line_lens[cy]) cx = line_lens[cy];
                if (cy < scroll_y) { scroll_y = cy; draw_screen(api); }
            }
        } else if (key == 1001) { // DOWN
            if (cy < num_lines - 1) {
                cy++;
                if (cx > line_lens[cy]) cx = line_lens[cy];
                {
                    int rows = view_rows(api);
                    int text_rows = rows > 1 ? rows - 1 : 1;
                    if (cy >= scroll_y + text_rows) { scroll_y = cy - (text_rows - 1); draw_screen(api); }
                }
            }
        } else if (key == 1002) { // LEFT
            if (cx > 0) {
                cx--;
            } else if (cy > 0) {
                cy--;
                cx = line_lens[cy];
                if (cy < scroll_y) { scroll_y = cy; draw_screen(api); }
            }
        } else if (key == 1003) { // RIGHT
            if (cx < line_lens[cy]) {
                cx++;
            } else if (cy < num_lines - 1) {
                cy++;
                cx = 0;
                {
                    int rows = view_rows(api);
                    int text_rows = rows > 1 ? rows - 1 : 1;
                    if (cy >= scroll_y + text_rows) { scroll_y = cy - (text_rows - 1); draw_screen(api); }
                }
            }
        } else if (key == '\n') { // ENTER
            if (num_lines < MAX_LINES) {
                for (int i = num_lines; i > cy + 1; i--) {
                    line_lens[i] = line_lens[i - 1];
                    for (int j = 0; j < line_lens[i]; j++) lines[i][j] = lines[i - 1][j];
                }
                
                line_lens[cy + 1] = line_lens[cy] - cx;
                for (int j = 0; j < line_lens[cy + 1]; j++) {
                    lines[cy + 1][j] = lines[cy][cx + j];
                }
                line_lens[cy] = cx;
                num_lines++;
                cy++;
                cx = 0;
                {
                    int rows = view_rows(api);
                    int text_rows = rows > 1 ? rows - 1 : 1;
                    if (cy >= scroll_y + text_rows) scroll_y = cy - (text_rows - 1);
                }
                draw_screen(api);
            }
        } else if (key == 8) { // BACKSPACE
            if (cx > 0) {
                for (int j = cx; j < line_lens[cy]; j++) {
                    lines[cy][j - 1] = lines[cy][j];
                }
                line_lens[cy]--;
                cx--;
                draw_screen(api); 
            } else if (cy > 0) {
                int old_len = line_lens[cy - 1];
                if (old_len + line_lens[cy] < MAX_COLS) {
                    for (int j = 0; j < line_lens[cy]; j++) {
                        lines[cy - 1][old_len + j] = lines[cy][j];
                    }
                    line_lens[cy - 1] += line_lens[cy];
                    
                    for (int i = cy; i < num_lines - 1; i++) {
                        line_lens[i] = line_lens[i + 1];
                        for (int j = 0; j < line_lens[i]; j++) lines[i][j] = lines[i + 1][j];
                    }
                    num_lines--;
                    cy--;
                    cx = old_len;
                    if (cy < scroll_y) scroll_y = cy;
                    draw_screen(api);
                }
            }
        } else if (key >= 32 && key <= 126) { // Printable chars
            if (line_lens[cy] < MAX_COLS - 1) {
                for (int j = line_lens[cy]; j > cx; j--) {
                    lines[cy][j] = lines[cy][j - 1];
                }
                lines[cy][cx] = (char)key;
                line_lens[cy]++;
                cx++;
                draw_screen(api);
            }
        }
    }

    api->clear_screen();
    api->set_cursor(0, 0);
    api->puts("Leaving mljOS Edit\n");
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

static int view_cols(mljos_api_t *api) {
    if (api && api->tui_cols) {
        int cols = api->tui_cols();
        if (cols > 0) return cols;
    }
    return 80;
}

static int view_rows(mljos_api_t *api) {
    if (api && api->tui_rows) {
        int rows = api->tui_rows();
        if (rows > 0) return rows;
    }
    return 25;
}

static void draw_status(mljos_api_t *api) {
    int cols = view_cols(api);
    int rows = view_rows(api);
    int status_row = rows > 0 ? rows - 1 : 0;

    for (int i = 0; i < cols; i++) {
        api->putchar_at(' ', status_row, i);
    }
    
    if (temporary_msg[0]) {
        int i = 0;
        for (; temporary_msg[i] && i < cols; i++) {
            api->putchar_at(temporary_msg[i], status_row, i);
        }
        temporary_msg[0] = '\0';
        return;
    }

    char status[256];
    int pos = 0;

    const char *title = "mljOS Edit - ";
    while (*title && pos < (int)sizeof(status) - 1) status[pos++] = *title++;
    
    char *fn = filename[0] ? filename : "(new file)";
    while (*fn && pos < (int)sizeof(status) - 1) status[pos++] = *fn++;
    
    const char *cmds = " - Ctrl+O Save - Ctrl+X Exit";
    while (*cmds && pos < (int)sizeof(status) - 1) status[pos++] = *cmds++;
    
    status[pos] = '\0';
    
    for (int i = 0; i < pos && i < cols; i++) {
        api->putchar_at(status[i], status_row, i);
    }
}

static void draw_screen(mljos_api_t *api) {
    int cols = view_cols(api);
    int rows = view_rows(api);
    int text_rows = rows > 1 ? rows - 1 : 1;

    api->clear_screen();
    for (int i = 0; i < text_rows; i++) {
        int l = scroll_y + i;
        if (l < num_lines) {
            for (int j = 0; j < line_lens[l] && j < cols; j++) {
                api->putchar_at(lines[l][j], i, j);
            }
        }
    }
    draw_status(api);
}

static void prompt_save(mljos_api_t *api) {
    int cols = view_cols(api);
    int rows = view_rows(api);
    int status_row = rows > 0 ? rows - 1 : 0;

    for (int i = 0; i < cols; i++) api->putchar_at(' ', status_row, i);
    const char *p = filename[0] ? "Save to (empty = current): " : "Save to: ";
    for (int i = 0; p[i] && i < cols; i++) api->putchar_at(p[i], status_row, i);
    
    {
        int col = text_len(p);
        if (cols > 0 && col >= cols) col = cols - 1;
        if (col < 0) col = 0;
        api->set_cursor(status_row, col);
    }
    
    char new_name[128];
    int len = api->read_line(new_name, sizeof(new_name));
    if (len > 0) {
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
    draw_screen(api);
}

#include "sdk/mljos_api.h"
#include "sdk/mljos_app.h"

MLJOS_APP_DEFINE("Files", MLJOS_APP_FLAG_TUI | MLJOS_APP_FLAG_GUI);

static int text_len(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void my_strcpy(char *dst, const char *src) {
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

static void my_strncpy(char *dst, const char *src, int n) {
    int i = 0;
    while (i < n - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int my_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static int ends_with(const char *str, const char *suffix) {
    int str_len = text_len(str);
    int suffix_len = text_len(suffix);
    if (str_len < suffix_len) return 0;
    return my_strcmp(str + str_len - suffix_len, suffix) == 0;
}

static void split_path(const char *path, char *parent) {
    int len = text_len(path);
    if (len <= 1) {
        my_strcpy(parent, "/");
        return;
    }
    int last_slash = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') {
            last_slash = i;
            if (i < len - 1) break; // found a slash not at the end
        }
    }
    if (last_slash <= 0) {
        my_strcpy(parent, "/");
    } else {
        my_strncpy(parent, path, last_slash + 1);
    }
}

static void append_path(char *path, int max_len, const char *name) {
    int len = text_len(path);
    if (len > 0 && path[len - 1] != '/') {
        if (len < max_len - 1) {
            path[len] = '/';
            path[len + 1] = '\0';
            len++;
        }
    }
    int idx = 0;
    while (name[idx] && len < max_len - 1) {
        path[len++] = name[idx++];
    }
    path[len] = '\0';
}

#define MAX_ENTRIES 128
#define MAX_NAME_LEN 32

typedef struct {
    char name[MAX_NAME_LEN];
    int is_dir;
} file_entry_t;

static file_entry_t g_entries[MAX_ENTRIES];
static int g_num_entries = 0;
static char g_current_path[256] = "/";

static void refresh_dir(mljos_api_t *api) {
    char buf[4096];
    g_num_entries = 0;
    
    char dummy[2];
    if (!api->list_dir(g_current_path, buf, sizeof(buf))) {
        my_strcpy(g_current_path, "/");
        if (!api->list_dir(g_current_path, buf, sizeof(buf))) return;
    }

    char *p = buf;
    while (*p && g_num_entries < MAX_ENTRIES) {
        if (my_strcmp(p, ".") != 0 && my_strcmp(p, "..") != 0) {
            my_strncpy(g_entries[g_num_entries].name, p, MAX_NAME_LEN);
            char test_path[256];
            my_strcpy(test_path, g_current_path);
            append_path(test_path, sizeof(test_path), p);
            
            g_entries[g_num_entries].is_dir = api->list_dir(test_path, dummy, sizeof(dummy));
            g_num_entries++;
        }
        while (*p) p++;
        p++; // skip null
    }
}

// Simple pixel icon rendering (16x16)
static void draw_folder_icon(mljos_api_t *api, int x, int y) {
    api->ui->fill_rect(x + 2, y + 2, 6, 3, 0xD4A373); // Tab
    api->ui->fill_rect(x + 1, y + 4, 14, 10, 0xFAEDCD); // Front folder
    api->ui->fill_rect(x, y + 5, 16, 9, 0xE9C46A); // Shading
}

static void draw_file_icon(mljos_api_t *api, int x, int y) {
    api->ui->fill_rect(x + 2, y + 1, 12, 14, 0xEDEDED); // Document
    api->ui->fill_rect(x + 10, y + 1, 4, 4, 0xCCCCCC); // Folded corner
    api->ui->fill_rect(x + 4, y + 6, 8, 1, 0xA0A0A0); // Text lines
    api->ui->fill_rect(x + 4, y + 9, 8, 1, 0xA0A0A0);
    api->ui->fill_rect(x + 4, y + 12, 6, 1, 0xA0A0A0);
}

static void draw_app_icon(mljos_api_t *api, int x, int y) {
    api->ui->fill_rect(x + 2, y + 2, 12, 12, 0x457B9D); // App window
    api->ui->fill_rect(x + 2, y + 2, 12, 3, 0x1D3557); // Title bar
    api->ui->fill_rect(x + 10, y + 3, 2, 1, 0xE63946); // Close btn
}

static void draw_image_icon(mljos_api_t *api, int x, int y) {
    api->ui->fill_rect(x + 1, y + 2, 14, 12, 0x2A9D8F); // Picture frame
    api->ui->fill_rect(x + 2, y + 3, 12, 10, 0xE9C46A); // Canvas
    api->ui->fill_rect(x + 8, y + 6, 3, 3, 0xE76F51); // Sun
    api->ui->fill_rect(x + 2, y + 9, 6, 4, 0x264653); // Mountains
}

MLJOS_APP_ENTRY void _start(mljos_api_t *api) {
    if (api && (api->launch_flags & MLJOS_LAUNCH_GUI) && api->ui) {
        api->ui->begin_app("Files");

        if (api->open_path && api->open_path[0]) {
            my_strncpy(g_current_path, api->open_path, sizeof(g_current_path));
        } else if (api->get_cwd) {
            api->get_cwd(g_current_path, sizeof(g_current_path));
            if (g_current_path[0] == '\0') my_strcpy(g_current_path, "/");
        }
        
        refresh_dir(api);

        int dirty = 1;
        int hover_idx = -1;
        int selected_idx = -1; // Track single file selection for deletion

        uint32_t col_bg = 0x1E1E1E;
        uint32_t col_panel = 0x252526;
        uint32_t col_text = 0xD4D4D4;
        uint32_t col_hover = 0x2A2D2E;
        uint32_t col_selected = 0x37373D;
        uint32_t col_btn = 0x0E639C;

        for (;;) {
            int w = (int)api->ui->screen_w();
            int h = (int)api->ui->screen_h();
            
            int top_bar_h = 40;
            int padding = 10;
            int item_h = 30;
            int cols = w / 200;
            if (cols < 1) cols = 1;
            int col_w = (w - padding * 2) / cols;

            if (dirty) {
                // Background & Top bar
                api->ui->fill_rect(0, 0, w, h, col_bg);
                api->ui->fill_rect(0, 0, w, top_bar_h, col_panel);
                
                // Toolbar Buttons
                api->ui->fill_rect(padding, 5, 40, 30, col_btn); // UP
                api->ui->draw_text("UP", padding + 12, 12, 0xFFFFFF);
                
                api->ui->fill_rect(padding + 55, 5, 75, 30, col_btn); // New Dir
                api->ui->draw_text("+Folder", padding + 60, 12, 0xFFFFFF);
                
                api->ui->fill_rect(padding + 135, 5, 60, 30, col_btn); // New File
                api->ui->draw_text("+File", padding + 145, 12, 0xFFFFFF);

                // If an item is selected, show DELETE button dynamically
                if (selected_idx >= 0 && selected_idx < g_num_entries) {
                    api->ui->fill_rect(padding + 200, 5, 65, 30, 0x9B2226); // Red DELETE
                    api->ui->draw_text("DELETE", padding + 208, 12, 0xFFFFFF);
                }

                // Path text
                api->ui->draw_text(g_current_path, padding + 280, 12, col_text);
                
                // Draw grid
                for (int i = 0; i < g_num_entries; i++) {
                    int c = i % cols;
                    int r = i / cols;
                    
                    int x = padding + c * col_w;
                    int y = top_bar_h + padding + r * item_h;
                    
                    if (y + item_h > h - padding) break; // out of screen
                    
                    if (i == hover_idx) {
                        api->ui->fill_rect(x, y, col_w - 5, item_h - 2, col_hover);
                    }
                    if (i == selected_idx) {
                        api->ui->fill_rect(x, y, col_w - 5, item_h - 2, col_selected);
                    }
                    
                    // Custom Icons
                    if (g_entries[i].is_dir) {
                        draw_folder_icon(api, x + 5, y + 6);
                    } else if (ends_with(g_entries[i].name, ".app")) {
                        draw_app_icon(api, x + 5, y + 6);
                    } else if (ends_with(g_entries[i].name, ".bmp")) {
                        draw_image_icon(api, x + 5, y + 6);
                    } else {
                        draw_file_icon(api, x + 5, y + 6);
                    }
                    
                    // Name
                    api->ui->draw_text(g_entries[i].name, x + 30, y + 6, col_text);
                }
                
                dirty = 0;
            }

            mljos_ui_event_t ev;
            while (api->ui->poll_event(&ev)) {
                if (ev.type == MLJOS_UI_EVENT_EXPOSE) {
                    dirty = 1;
                }
                
                if (ev.type == MLJOS_UI_EVENT_KEY_DOWN) {
                    if (ev.key == 27) { 
                        api->ui->end_app(); 
                        return; 
                    } else if (ev.key == '\n' || ev.key == '\r') {
                        // Pressing enter opens the selected file
                        if (selected_idx >= 0) {
                            char file_path[256];
                            my_strcpy(file_path, g_current_path);
                            append_path(file_path, sizeof(file_path), g_entries[selected_idx].name);

                            if (g_entries[selected_idx].is_dir) {
                                my_strcpy(g_current_path, file_path);
                                refresh_dir(api);
                                selected_idx = -1;
                                dirty = 1;
                            } else if (ends_with(g_entries[selected_idx].name, ".app") && api->launch_app) {
                                api->launch_app(file_path);
                            } else if (api->launch_app_args) {
                                // Default application is edit!
                                api->launch_app_args("edit", file_path);
                            }
                        }
                    }
                }
                
                if (ev.type == MLJOS_UI_EVENT_MOUSE_MOVE) {
                    int old_hover = hover_idx;
                    hover_idx = -1;
                    if (ev.y > top_bar_h + padding) {
                        int rel_y = ev.y - (top_bar_h + padding);
                        int r = rel_y / item_h;
                        int c = (ev.x - padding) / col_w;
                        if (c >= 0 && c < cols) {
                            int idx = r * cols + c;
                            if (idx >= 0 && idx < g_num_entries) hover_idx = idx;
                        }
                    }
                    if (old_hover != hover_idx) dirty = 1;
                }
                
                if (ev.type == MLJOS_UI_EVENT_MOUSE_LEFT_DOWN) {
                    int clicked_up = (ev.y >= 5 && ev.y <= 35 && ev.x >= padding && ev.x <= padding + 40);
                    int clicked_newdir = (ev.y >= 5 && ev.y <= 35 && ev.x >= padding + 55 && ev.x <= padding + 130);
                    int clicked_newfile = (ev.y >= 5 && ev.y <= 35 && ev.x >= padding + 135 && ev.x <= padding + 195);
                    int clicked_del = (ev.y >= 5 && ev.y <= 35 && ev.x >= padding + 200 && ev.x <= padding + 265);
                    int clicked_idx = -1;

                    if (ev.y > top_bar_h + padding) {
                        int rel_y = ev.y - (top_bar_h + padding);
                        int r = rel_y / item_h;
                        int c = (ev.x - padding) / col_w;
                        if (c >= 0 && c < cols) {
                            int idx = r * cols + c;
                            if (idx >= 0 && idx < g_num_entries) {
                                clicked_idx = idx;
                            }
                        }
                    }

                    if (clicked_up) {
                        char parent[256];
                        split_path(g_current_path, parent);
                        if (my_strcmp(g_current_path, parent) != 0) {
                            my_strcpy(g_current_path, parent);
                            refresh_dir(api);
                            selected_idx = -1;
                            dirty = 1;
                        }
                    } else if (clicked_newdir) {
                        char out_buf[64];
                        if (api->ui->prompt_input("Create Folder", "Enter new folder name:", out_buf, sizeof(out_buf))) {
                            if (out_buf[0]) {
                                char new_path[256];
                                my_strcpy(new_path, g_current_path);
                                append_path(new_path, sizeof(new_path), out_buf);
                                if (api->mkdir) api->mkdir(new_path);
                                refresh_dir(api);
                                selected_idx = -1;
                                dirty = 1;
                            }
                        } else dirty = 1; // Refresh UI after prompt overlay
                    } else if (clicked_newfile) {
                        char out_buf[64];
                        if (api->ui->prompt_input("Create File", "Enter new file name:", out_buf, sizeof(out_buf))) {
                            if (out_buf[0]) {
                                char new_path[256];
                                my_strcpy(new_path, g_current_path);
                                append_path(new_path, sizeof(new_path), out_buf);
                                if (api->write_file) api->write_file(new_path, "", 0);
                                refresh_dir(api);
                                selected_idx = -1;
                                dirty = 1;
                            }
                        } else dirty = 1;
                    } else if (clicked_del && selected_idx >= 0) {
                        char target[256];
                        my_strcpy(target, g_current_path);
                        append_path(target, sizeof(target), g_entries[selected_idx].name);
                        
                        // To show off dialogs, lets ask before delete
                        char prompt_msg[128];
                        my_strcpy(prompt_msg, "Delete ");
                        my_strncpy(prompt_msg + 7, g_entries[selected_idx].name, 100);
                        append_path(prompt_msg, sizeof(prompt_msg), "? [Enter]");
                        
                        char out_buf[16];
                        if (api->ui->prompt_input("Confirm Deletion", prompt_msg, out_buf, sizeof(out_buf))) {
                            if (api->rm) api->rm(target);
                            selected_idx = -1;
                            refresh_dir(api);
                        }
                        dirty = 1;
                    } else if (clicked_idx >= 0) {
                        // Double click behavior (if already selected, open it)
                        if (selected_idx == clicked_idx) {
                            if (g_entries[clicked_idx].is_dir) {
                                append_path(g_current_path, sizeof(g_current_path), g_entries[clicked_idx].name);
                                refresh_dir(api);
                                selected_idx = -1; // Reset selection on navigate
                                dirty = 1;
                            } else {
                                char file_path[256];
                                my_strcpy(file_path, g_current_path);
                                append_path(file_path, sizeof(file_path), g_entries[clicked_idx].name);

                                if (ends_with(g_entries[clicked_idx].name, ".app") && api->launch_app) {
                                    api->launch_app(file_path);
                                } else if (api->launch_app_args) {
                                    api->launch_app_args("edit", file_path);
                                }
                            }
                        } else {
                            // First click selects the item
                            selected_idx = clicked_idx;
                            dirty = 1;
                        }
                    } else if (ev.y > top_bar_h) {
                        // Clicking empty space deselects
                        if (selected_idx != -1) {
                            selected_idx = -1;
                            dirty = 1;
                        }
                    }
                }
            }
        }
    }

    // TUI mode fallback
    char buf[128];
    api->puts("Welcome to mljOS Files (TUI mode)!\n");
    if (api->get_cwd) {
        api->get_cwd(g_current_path, sizeof(g_current_path));
    }
    api->puts("Current path: ");
    api->puts(g_current_path);
    api->puts("\n");
    
    for (;;) {
        api->puts("files> ");
        int len = api->read_line(buf, sizeof(buf));
        if (len == 0) continue;
        if (buf[0] == 'q') break;
        api->puts("Not supported in TUI, use LS command instead.\n");
    }
}

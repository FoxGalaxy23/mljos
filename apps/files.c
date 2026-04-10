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
#define MAX_TABS 8

typedef struct {
    char name[MAX_NAME_LEN];
    int is_dir;
} file_entry_t;

typedef struct {
    file_entry_t entries[MAX_ENTRIES];
    int num_entries;
    int hover_idx;
    int selected_idx;
    char current_path[256];
} files_tab_t;

static files_tab_t g_tabs[MAX_TABS];
static int g_tab_count = 1;
static int g_active_tab = 0;

static files_tab_t *active_tab(void) {
    if (g_active_tab < 0) g_active_tab = 0;
    if (g_active_tab >= g_tab_count) g_active_tab = g_tab_count - 1;
    return &g_tabs[g_active_tab];
}

static void init_tab(files_tab_t *tab, const char *path) {
    if (!tab) return;
    tab->num_entries = 0;
    tab->hover_idx = -1;
    tab->selected_idx = -1;
    if (path && path[0]) my_strncpy(tab->current_path, path, sizeof(tab->current_path));
    else my_strcpy(tab->current_path, "/");
}

static void close_tab_if_possible(int index) {
    if (g_tab_count <= 1) return;
    if (index < 0 || index >= g_tab_count) return;
    for (int i = index; i < g_tab_count - 1; i++) {
        g_tabs[i] = g_tabs[i + 1];
    }
    g_tab_count--;
    if (index < g_active_tab) g_active_tab--;
    if (g_active_tab >= g_tab_count) g_active_tab = g_tab_count - 1;
}

static void refresh_dir(mljos_api_t *api, files_tab_t *tab) {
    if (!tab) return;
    char buf[4096];
    tab->num_entries = 0;
    
    char dummy[2];
    if (!api->list_dir(tab->current_path, buf, sizeof(buf))) {
        my_strcpy(tab->current_path, "/");
        if (!api->list_dir(tab->current_path, buf, sizeof(buf))) return;
    }

    char *p = buf;
    while (*p && tab->num_entries < MAX_ENTRIES) {
        if (my_strcmp(p, ".") != 0 && my_strcmp(p, "..") != 0) {
            my_strncpy(tab->entries[tab->num_entries].name, p, MAX_NAME_LEN);
            char test_path[256];
            my_strcpy(test_path, tab->current_path);
            append_path(test_path, sizeof(test_path), p);
            
            tab->entries[tab->num_entries].is_dir = api->list_dir(test_path, dummy, sizeof(dummy));
            tab->num_entries++;
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

        for (int i = 0; i < MAX_TABS; i++) init_tab(&g_tabs[i], "/");
        g_tab_count = 1;
        g_active_tab = 0;

        if (api->open_path && api->open_path[0]) {
            init_tab(&g_tabs[0], api->open_path);
        } else if (api->get_cwd) {
            api->get_cwd(g_tabs[0].current_path, sizeof(g_tabs[0].current_path));
            if (g_tabs[0].current_path[0] == '\0') my_strcpy(g_tabs[0].current_path, "/");
        }
        
        refresh_dir(api, &g_tabs[0]);

        int dirty = 1;

        uint32_t col_bg = 0x1E1E1E;
        uint32_t col_panel = 0x252526;
        uint32_t col_text = 0xD4D4D4;
        uint32_t col_hover = 0x2A2D2E;
        uint32_t col_selected = 0x37373D;
        uint32_t col_btn = 0x0E639C;

        for (;;) {
            files_tab_t *tab = active_tab();
            int w = (int)api->ui->screen_w();
            int h = (int)api->ui->screen_h();
            
            int tabs_h = 28;
            int top_bar_h = 68;
            int padding = 10;
            int item_h = 30;
            int cols = w / 200;
            if (cols < 1) cols = 1;
            int col_w = (w - padding * 2) / cols;

            if (dirty) {
                api->ui->fill_rect(0, 0, w, h, col_bg);
                api->ui->fill_rect(0, 0, w, tabs_h, 0x1B1B1B);
                api->ui->fill_rect(0, tabs_h, w, top_bar_h - tabs_h, col_panel);

                int tab_x = padding;
                for (int i = 0; i < g_tab_count; i++) {
                    api->ui->fill_rect(tab_x, 4, 120, tabs_h - 8, i == g_active_tab ? 0x0E639C : 0x333333);
                    api->ui->draw_text(g_tabs[i].current_path, tab_x + 8, 8, 0xFFFFFF);
                    if (g_tab_count > 1) {
                        api->ui->fill_rect(tab_x + 100, 7, 12, 12, 0x7A1E1E);
                        api->ui->draw_text("x", tab_x + 104, 8, 0xFFFFFF);
                    }
                    tab_x += 126;
                }
                if (g_tab_count < MAX_TABS) {
                    api->ui->fill_rect(tab_x, 4, 26, tabs_h - 8, 0x333333);
                    api->ui->draw_text("+", tab_x + 9, 8, 0xFFFFFF);
                }

                api->ui->fill_rect(padding, tabs_h + 5, 40, 30, col_btn);
                api->ui->draw_text("UP", padding + 12, tabs_h + 12, 0xFFFFFF);
                
                api->ui->fill_rect(padding + 55, tabs_h + 5, 75, 30, col_btn);
                api->ui->draw_text("+Folder", padding + 60, tabs_h + 12, 0xFFFFFF);
                
                api->ui->fill_rect(padding + 135, tabs_h + 5, 60, 30, col_btn);
                api->ui->draw_text("+File", padding + 145, tabs_h + 12, 0xFFFFFF);

                if (tab->selected_idx >= 0 && tab->selected_idx < tab->num_entries) {
                    api->ui->fill_rect(padding + 200, tabs_h + 5, 65, 30, 0x9B2226);
                    api->ui->draw_text("DELETE", padding + 208, tabs_h + 12, 0xFFFFFF);
                }

                api->ui->draw_text(tab->current_path, padding + 280, tabs_h + 12, col_text);
                
                for (int i = 0; i < tab->num_entries; i++) {
                    int c = i % cols;
                    int r = i / cols;
                    
                    int x = padding + c * col_w;
                    int y = top_bar_h + padding + r * item_h;
                    
                    if (y + item_h > h - padding) break; // out of screen
                    
                    if (i == tab->hover_idx) {
                        api->ui->fill_rect(x, y, col_w - 5, item_h - 2, col_hover);
                    }
                    if (i == tab->selected_idx) {
                        api->ui->fill_rect(x, y, col_w - 5, item_h - 2, col_selected);
                    }
                    
                    if (tab->entries[i].is_dir) {
                        draw_folder_icon(api, x + 5, y + 6);
                    } else if (ends_with(tab->entries[i].name, ".app")) {
                        draw_app_icon(api, x + 5, y + 6);
                    } else if (ends_with(tab->entries[i].name, ".bmp")) {
                        draw_image_icon(api, x + 5, y + 6);
                    } else {
                        draw_file_icon(api, x + 5, y + 6);
                    }
                    
                    api->ui->draw_text(tab->entries[i].name, x + 30, y + 6, col_text);
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
                        if (tab->selected_idx >= 0) {
                            char file_path[256];
                            my_strcpy(file_path, tab->current_path);
                            append_path(file_path, sizeof(file_path), tab->entries[tab->selected_idx].name);

                            if (tab->entries[tab->selected_idx].is_dir) {
                                my_strcpy(tab->current_path, file_path);
                                refresh_dir(api, tab);
                                tab->selected_idx = -1;
                                dirty = 1;
                            } else if (ends_with(tab->entries[tab->selected_idx].name, ".app") && api->launch_app) {
                                api->launch_app(file_path);
                            } else if (api->launch_app_args) {
                                api->launch_app_args("edit", file_path);
                            }
                        }
                    }
                }
                
                if (ev.type == MLJOS_UI_EVENT_MOUSE_MOVE) {
                    int old_hover = tab->hover_idx;
                    tab->hover_idx = -1;
                    if (ev.y > top_bar_h + padding) {
                        int rel_y = ev.y - (top_bar_h + padding);
                        int r = rel_y / item_h;
                        int c = (ev.x - padding) / col_w;
                        if (c >= 0 && c < cols) {
                            int idx = r * cols + c;
                            if (idx >= 0 && idx < tab->num_entries) tab->hover_idx = idx;
                        }
                    }
                    if (old_hover != tab->hover_idx) dirty = 1;
                }
                
                if (ev.type == MLJOS_UI_EVENT_MOUSE_LEFT_DOWN) {
                    int clicked_up = (ev.y >= tabs_h + 5 && ev.y <= tabs_h + 35 && ev.x >= padding && ev.x <= padding + 40);
                    int clicked_newdir = (ev.y >= tabs_h + 5 && ev.y <= tabs_h + 35 && ev.x >= padding + 55 && ev.x <= padding + 130);
                    int clicked_newfile = (ev.y >= tabs_h + 5 && ev.y <= tabs_h + 35 && ev.x >= padding + 135 && ev.x <= padding + 195);
                    int clicked_del = (ev.y >= tabs_h + 5 && ev.y <= tabs_h + 35 && ev.x >= padding + 200 && ev.x <= padding + 265);
                    int clicked_idx = -1;

                    if (ev.y >= 4 && ev.y <= tabs_h - 4) {
                        int tab_x = padding;
                        for (int i = 0; i < g_tab_count; i++) {
                            if (g_tab_count > 1 && ev.x >= tab_x + 100 && ev.x <= tab_x + 112) {
                                close_tab_if_possible(i);
                                dirty = 1;
                                break;
                            }
                            if (ev.x >= tab_x && ev.x <= tab_x + 120) {
                                g_active_tab = i;
                                dirty = 1;
                                break;
                            }
                            tab_x += 126;
                        }
                        if (g_tab_count < MAX_TABS && ev.x >= tab_x && ev.x <= tab_x + 26) {
                            init_tab(&g_tabs[g_tab_count], tab->current_path);
                            refresh_dir(api, &g_tabs[g_tab_count]);
                            g_active_tab = g_tab_count;
                            g_tab_count++;
                            dirty = 1;
                        }
                    }

                    if (ev.y > top_bar_h + padding) {
                        int rel_y = ev.y - (top_bar_h + padding);
                        int r = rel_y / item_h;
                        int c = (ev.x - padding) / col_w;
                        if (c >= 0 && c < cols) {
                            int idx = r * cols + c;
                            if (idx >= 0 && idx < tab->num_entries) {
                                clicked_idx = idx;
                            }
                        }
                    }

                    if (clicked_up) {
                        char parent[256];
                        split_path(tab->current_path, parent);
                        if (my_strcmp(tab->current_path, parent) != 0) {
                            my_strcpy(tab->current_path, parent);
                            refresh_dir(api, tab);
                            tab->selected_idx = -1;
                            dirty = 1;
                        }
                    } else if (clicked_newdir) {
                        char out_buf[64];
                        if (api->ui->prompt_input("Create Folder", "Enter new folder name:", out_buf, sizeof(out_buf))) {
                            if (out_buf[0]) {
                                char new_path[256];
                                my_strcpy(new_path, tab->current_path);
                                append_path(new_path, sizeof(new_path), out_buf);
                                if (api->mkdir) api->mkdir(new_path);
                                refresh_dir(api, tab);
                                tab->selected_idx = -1;
                                dirty = 1;
                            }
                        } else dirty = 1; // Refresh UI after prompt overlay
                    } else if (clicked_newfile) {
                        char out_buf[64];
                        if (api->ui->prompt_input("Create File", "Enter new file name:", out_buf, sizeof(out_buf))) {
                            if (out_buf[0]) {
                                char new_path[256];
                                my_strcpy(new_path, tab->current_path);
                                append_path(new_path, sizeof(new_path), out_buf);
                                if (api->write_file) api->write_file(new_path, "", 0);
                                refresh_dir(api, tab);
                                tab->selected_idx = -1;
                                dirty = 1;
                            }
                        } else dirty = 1;
                    } else if (clicked_del && tab->selected_idx >= 0) {
                        char target[256];
                        my_strcpy(target, tab->current_path);
                        append_path(target, sizeof(target), tab->entries[tab->selected_idx].name);
                        
                        char prompt_msg[128];
                        my_strcpy(prompt_msg, "Delete ");
                        my_strncpy(prompt_msg + 7, tab->entries[tab->selected_idx].name, 100);
                        append_path(prompt_msg, sizeof(prompt_msg), "? [Enter]");
                        
                        char out_buf[16];
                        if (api->ui->prompt_input("Confirm Deletion", prompt_msg, out_buf, sizeof(out_buf))) {
                            if (api->rm) api->rm(target);
                            tab->selected_idx = -1;
                            refresh_dir(api, tab);
                        }
                        dirty = 1;
                    } else if (clicked_idx >= 0) {
                        if (tab->selected_idx == clicked_idx) {
                            if (tab->entries[clicked_idx].is_dir) {
                                append_path(tab->current_path, sizeof(tab->current_path), tab->entries[clicked_idx].name);
                                refresh_dir(api, tab);
                                tab->selected_idx = -1;
                                dirty = 1;
                            } else {
                                char file_path[256];
                                my_strcpy(file_path, tab->current_path);
                                append_path(file_path, sizeof(file_path), tab->entries[clicked_idx].name);

                                if (ends_with(tab->entries[clicked_idx].name, ".app") && api->launch_app) {
                                    api->launch_app(file_path);
                                } else if (api->launch_app_args) {
                                    api->launch_app_args("edit", file_path);
                                }
                            }
                        } else {
                            tab->selected_idx = clicked_idx;
                            dirty = 1;
                        }
                    } else if (ev.y > top_bar_h) {
                        if (tab->selected_idx != -1) {
                            tab->selected_idx = -1;
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
        api->get_cwd(g_tabs[0].current_path, sizeof(g_tabs[0].current_path));
    }
    api->puts("Current path: ");
    api->puts(g_tabs[0].current_path);
    api->puts("\n");
    
    for (;;) {
        api->puts("files> ");
        int len = api->read_line(buf, sizeof(buf));
        if (len == 0) continue;
        if (buf[0] == 'q') break;
        api->puts("Not supported in TUI, use LS command instead.\n");
    }
}

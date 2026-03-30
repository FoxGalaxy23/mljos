#include "sdk/mljos_api.h"
#include "sdk/mljos_app.h"

MLJOS_APP_DEFINE("Calculator", MLJOS_APP_FLAG_TUI | MLJOS_APP_FLAG_GUI);

static int text_len(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static int hit_test_button(int mx, int my, int grid_x, int grid_y, int btn_w, int btn_h, int gap, int *out_r, int *out_c) {
    if (mx < grid_x || my < grid_y) return 0;
    int relx = mx - grid_x;
    int rely = my - grid_y;
    int cell_w = btn_w + gap;
    int cell_h = btn_h + gap;
    int c = relx / cell_w;
    int r = rely / cell_h;
    if (c < 0 || c >= 4 || r < 0 || r >= 4) return 0;
    int bx = grid_x + c * cell_w;
    int by = grid_y + r * cell_h;
    if (mx < bx || mx >= bx + btn_w || my < by || my >= by + btn_h) return 0;
    if (out_r) *out_r = r;
    if (out_c) *out_c = c;
    return 1;
}

MLJOS_APP_ENTRY void _start(mljos_api_t *api) {
    if (api && (api->launch_flags & MLJOS_LAUNCH_GUI) && api->ui) {
        api->ui->begin_app("Calculator");

        // Simple GUI calculator: clickable buttons + display (adaptive layout).

        char expr[64];
        int expr_len = 0;
        expr[0] = '\0';

        const char *labels[4][4] = {
            { "7", "8", "9", "/" },
            { "4", "5", "6", "*" },
            { "1", "2", "3", "-" },
            { "C", "0", "=", "+" },
        };

        int dirty = 1;
        int last_w = -1;
        int last_h = -1;
        int hover_r = -1, hover_c = -1;
        int active_r = -1, active_c = -1;

        for (;;) {
            int w = (int)api->ui->screen_w();
            int h = (int)api->ui->screen_h();
            if (w != last_w || h != last_h) {
                last_w = w;
                last_h = h;
                dirty = 1;
            }

            int pad = w / 30;
            if (pad < 8) pad = 8;
            if (pad > 24) pad = 24;

            int gap = w / 80;
            if (gap < 4) gap = 4;
            if (gap > 12) gap = 12;

            int footer_h = 20;
            int avail_w = w - pad * 2;
            int avail_h = h - pad * 2 - footer_h;
            if (avail_w < 80) avail_w = 80;
            if (avail_h < 120) avail_h = 120;

            int disp_h = avail_h / 5;
            if (disp_h < 32) disp_h = 32;
            if (disp_h > 80) disp_h = 80;

            int btn_w = (avail_w - 3 * gap) / 4;
            int btn_h = (avail_h - disp_h - 4 * gap) / 4;
            if (btn_w < 24) btn_w = 24;
            if (btn_h < 24) btn_h = 24;

            int grid_w = 4 * btn_w + 3 * gap;
            int grid_h = 4 * btn_h + 3 * gap;
            int total_h = disp_h + gap + grid_h;

            int left = (w - grid_w) / 2;
            if (left < 0) left = 0;
            int top = pad;
            if (total_h + footer_h + pad * 2 <= h) {
                top = (h - footer_h - total_h) / 2;
                if (top < pad) top = pad;
            }

            int grid_x = left;
            int grid_y = top + disp_h + gap;
            int disp_x = left;
            int disp_y = top;

            uint32_t col_bg = 0x000000;
            uint32_t col_disp = 0x202020;
            uint32_t col_btn = 0x303030;
            uint32_t col_btn_hover = 0x3E3E3E;
            uint32_t col_btn_active = 0x525252;
            uint32_t col_text = 0xFFFFFF;
            uint32_t col_hint = 0xA0A0A0;
            if (w >= 900 || h >= 700) {
                col_disp = 0x252525;
                col_btn = 0x353535;
                col_btn_hover = 0x4A4A4A;
                col_btn_active = 0x606060;
                col_hint = 0xB0B0B0;
            }

            int label_scale = btn_h / 24;
            if (label_scale < 1) label_scale = 1;
            if (label_scale > 2) label_scale = 2;
            int disp_scale = disp_h / 24;
            if (disp_scale < 1) disp_scale = 1;
            if (disp_scale > 3) disp_scale = 3;

            if (dirty) {
                api->ui->fill_rect(0, 0, w, h, col_bg);
                api->ui->fill_rect(disp_x, disp_y, grid_w, disp_h, col_disp);

                {
                    const char *disp = expr_len ? expr : "0";
                    int tw = text_len(disp) * 8 * disp_scale;
                    int tx = disp_x + 10;
                    if (tw + 20 < grid_w) tx = disp_x + (grid_w - tw) / 2;
                    int ty = disp_y + (disp_h - 16 * disp_scale) / 2;
                    if (api->ui->draw_text_scale && disp_scale > 1) {
                        api->ui->draw_text_scale(disp, tx, ty, col_text, disp_scale);
                    } else {
                        api->ui->draw_text(disp, tx, ty, col_text);
                    }
                }

                api->ui->draw_text("Esc - exit", pad, h - 20, col_hint);

                for (int r = 0; r < 4; ++r) {
                    for (int c = 0; c < 4; ++c) {
                        int bx = grid_x + c * (btn_w + gap);
                        int by = grid_y + r * (btn_h + gap);
                        uint32_t bc = col_btn;
                        if (r == active_r && c == active_c) bc = col_btn_active;
                        else if (r == hover_r && c == hover_c) bc = col_btn_hover;
                        api->ui->fill_rect(bx, by, btn_w, btn_h, bc);
                        {
                            const char *lab = labels[r][c];
                            int lw = text_len(lab) * 8 * label_scale;
                            int lx = bx + (btn_w - lw) / 2;
                            int ly = by + (btn_h - 16 * label_scale) / 2;
                            if (api->ui->draw_text_scale && label_scale > 1) {
                                api->ui->draw_text_scale(lab, lx, ly, col_text, label_scale);
                            } else {
                                api->ui->draw_text(lab, lx, ly, col_text);
                            }
                        }
                    }
                }
                dirty = 0;
            }

            mljos_ui_event_t ev;
            while (api->ui->poll_event(&ev)) {
                if (ev.type == MLJOS_UI_EVENT_EXPOSE) {
                    dirty = 1;
                    continue;
                }
                if (ev.type == MLJOS_UI_EVENT_KEY_DOWN) {
                    if (ev.key == 27) { api->ui->end_app(); return; } // Esc
                    if (ev.key == '\n' || ev.key == '\r') {
                        // Treat Enter as '='
                        ev.type = MLJOS_UI_EVENT_MOUSE_LEFT_DOWN;
                        ev.x = grid_x + 2 * (btn_w + gap) + 1;
                        ev.y = grid_y + 3 * (btn_h + gap) + 1;
                    } else if ((ev.key >= '0' && ev.key <= '9') || ev.key == '+' || ev.key == '-' || ev.key == '*' || ev.key == '/') {
                        if (expr_len < (int)sizeof(expr) - 2) {
                            expr[expr_len++] = (char)ev.key;
                            expr[expr_len] = '\0';
                            dirty = 1;
                        }
                    } else if (ev.key == 'c' || ev.key == 'C') {
                        expr_len = 0; expr[0] = '\0';
                        dirty = 1;
                    }
                }

                if (ev.type == MLJOS_UI_EVENT_MOUSE_MOVE) {
                    int new_hover_r = -1;
                    int new_hover_c = -1;
                    if (hit_test_button(ev.x, ev.y, grid_x, grid_y, btn_w, btn_h, gap, &new_hover_r, &new_hover_c)) {
                        // ok
                    }
                    if (new_hover_r != hover_r || new_hover_c != hover_c) {
                        hover_r = new_hover_r;
                        hover_c = new_hover_c;
                        dirty = 1;
                    }
                }

                if (ev.type == MLJOS_UI_EVENT_MOUSE_LEFT_DOWN) {
                    int click_r = -1, click_c = -1;
                    if (hit_test_button(ev.x, ev.y, grid_x, grid_y, btn_w, btn_h, gap, &click_r, &click_c)) {
                        active_r = click_r;
                        active_c = click_c;
                        const char *lab = labels[click_r][click_c];
                        char ch = lab[0];
                        if (ch == 'C') {
                            expr_len = 0; expr[0] = '\0';
                            dirty = 1;
                        } else if (ch == '=') {
                            // Parse "a op b" (single op) from expr
                            int a = 0, b = 0;
                            char op = 0;
                            int i = 0;
                            int sign_a = 1;
                            if (expr[i] == '-') { sign_a = -1; i++; }
                            while (expr[i] >= '0' && expr[i] <= '9') { a = a * 10 + (expr[i] - '0'); i++; }
                            a *= sign_a;
                            if (expr[i]) { op = expr[i]; i++; }
                            int sign_b = 1;
                            if (expr[i] == '-') { sign_b = -1; i++; }
                            while (expr[i] >= '0' && expr[i] <= '9') { b = b * 10 + (expr[i] - '0'); i++; }
                            b *= sign_b;

                            int ok = 1;
                            int res = 0;
                            if (op == '+') res = a + b;
                            else if (op == '-') res = a - b;
                            else if (op == '*') res = a * b;
                            else if (op == '/') { if (b != 0) res = a / b; else ok = 0; }
                            else ok = 0;

                            if (!ok) {
                                expr_len = 0;
                                expr[0] = '\0';
                            } else {
                                // int -> string
                                char out[32];
                                int p = 0;
                                int t = res;
                                if (t == 0) out[p++] = '0';
                                else {
                                    if (t < 0) { out[p++] = '-'; t = -t; }
                                    char tmp[16];
                                    int tp = 0;
                                    while (t > 0 && tp < (int)sizeof(tmp)) { tmp[tp++] = (char)('0' + (t % 10)); t /= 10; }
                                    while (tp > 0) out[p++] = tmp[--tp];
                                }
                                out[p] = '\0';
                                expr_len = 0;
                                for (int k = 0; out[k] && expr_len < (int)sizeof(expr) - 1; ++k) expr[expr_len++] = out[k];
                                expr[expr_len] = '\0';
                            }
                            dirty = 1;
                        } else {
                            if (expr_len < (int)sizeof(expr) - 2) {
                                expr[expr_len++] = ch;
                                expr[expr_len] = '\0';
                                dirty = 1;
                            }
                        }
                    }

                    if (active_r != -1 || active_c != -1) {
                        active_r = -1;
                        active_c = -1;
                        dirty = 1;
                    }
                }

            }
        }
    }

    char buf[128];
    api->puts("Welcome to mljOS Calculator!\n");
    api->puts("Type 'quit' to exit.\n");
    
    while(1) {
        api->puts("calc> ");
        int len = api->read_line(buf, sizeof(buf));
        if (len == 0) continue;
        
        if (buf[0] == 'q' && buf[1] == 'u' && buf[2] == 'i' && buf[3] == 't') {
            break;
        }
        
        int a = 0, b = 0;
        char op = 0;
        int i = 0;
        
        while(buf[i] && buf[i] == ' ') i++;
        while(buf[i] >= '0' && buf[i] <= '9') {
            a = a * 10 + (buf[i] - '0');
            i++;
        }
        while(buf[i] && buf[i] == ' ') i++;
        if (buf[i]) {
            op = buf[i];
            i++;
        }
        while(buf[i] && buf[i] == ' ') i++;
        while(buf[i] >= '0' && buf[i] <= '9') {
            b = b * 10 + (buf[i] - '0');
            i++;
        }
        
        if (op == 0) continue;
        
        int res = 0;
        if (op == '+') res = a + b;
        else if (op == '-') res = a - b;
        else if (op == '*') res = a * b;
        else if (op == '/') {
            if (b != 0) res = a / b;
            else {
                api->puts("Error: Division by zero\n");
                continue;
            }
        } else {
            api->puts("Error: Unknown operator\n");
            continue;
        }
        
        char out[16];
        int out_idx = 0;
        if (res == 0) {
            out[out_idx++] = '0';
        } else {
            int temp = res;
            if (res < 0) {
                api->putchar('-');
                temp = -res;
            }
            char tmp_buf[16];
            int tmp_idx = 0;
            while(temp > 0) {
                tmp_buf[tmp_idx++] = '0' + (temp % 10);
                temp /= 10;
            }
            while(tmp_idx > 0) out[out_idx++] = tmp_buf[--tmp_idx];
        }
        out[out_idx++] = '\n';
        out[out_idx] = '\0';
        api->puts(out);
    }
}

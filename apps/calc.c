#include "sdk/mljos_api.h"
#include "sdk/mljos_app.h"

MLJOS_APP_DEFINE("Calculator", MLJOS_APP_FLAG_TUI | MLJOS_APP_FLAG_GUI);

MLJOS_APP_ENTRY void _start(mljos_api_t *api) {
    if (api && (api->launch_flags & MLJOS_LAUNCH_GUI) && api->ui) {
        api->ui->begin_app("Calculator");

        // Simple GUI calculator: clickable buttons + display.
        const int w = (int)api->ui->screen_w();
        const int h = (int)api->ui->screen_h();
        const int top = 10;
        const int left = 10;
        const int disp_h = 40;
        const int btn_w = 60;
        const int btn_h = 40;
        const int gap = 6;

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

        for (;;) {
            int grid_x = left;
            int grid_y = top + disp_h + 10;

            if (dirty) {
                api->ui->fill_rect(0, 0, w, h, 0x000000);
                api->ui->fill_rect(left, top, 4 * btn_w + 3 * gap, disp_h, 0x202020);
                api->ui->draw_text(expr_len ? expr : "0", left + 10, top + 10, 0xFFFFFF);
                api->ui->draw_text("Esc - exit", left, h - 20, 0xA0A0A0);

                for (int r = 0; r < 4; ++r) {
                    for (int c = 0; c < 4; ++c) {
                        int bx = grid_x + c * (btn_w + gap);
                        int by = grid_y + r * (btn_h + gap);
                        api->ui->fill_rect(bx, by, btn_w, btn_h, 0x303030);
                        api->ui->draw_text(labels[r][c], bx + 24, by + 12, 0xFFFFFF);
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

                if (ev.type == MLJOS_UI_EVENT_MOUSE_LEFT_DOWN) {
                    // Hit test buttons
                    for (int r = 0; r < 4; ++r) {
                        for (int c = 0; c < 4; ++c) {
                            int bx = grid_x + c * (btn_w + gap);
                            int by = grid_y + r * (btn_h + gap);
                            if (ev.x >= bx && ev.x < bx + btn_w && ev.y >= by && ev.y < by + btn_h) {
                                const char *lab = labels[r][c];
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
                        }
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

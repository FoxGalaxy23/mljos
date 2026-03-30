#include "sdk/mljos_api.h"
#include "sdk/mljos_app.h"

MLJOS_APP_DEFINE("Time", MLJOS_APP_FLAG_TUI | MLJOS_APP_FLAG_GUI);

MLJOS_APP_ENTRY void _start(mljos_api_t *api) {
    if (api && (api->launch_flags & MLJOS_LAUNCH_GUI) && api->ui) {
        api->ui->begin_app("Time");

        int last_ss = -1;
        int last_w = -1;
        int last_h = -1;

        for (;;) {
            unsigned char hh, mm, ss;
            api->get_time(&hh, &mm, &ss);

            int w = (int)api->ui->screen_w();
            int h = (int)api->ui->screen_h();
            if (w != last_w || h != last_h) {
                last_w = w;
                last_h = h;
                last_ss = -1;
            }

            if ((int)ss != last_ss) {
                last_ss = (int)ss;

                int cx = w / 2;
                int cy = h / 2;
                int minwh = w < h ? w : h;
                int r = (minwh / 2) - 20;
                if (r < 6) r = minwh / 3;
                if (r < 4) r = 4;

                uint32_t col_bg = 0x000000;
                uint32_t col_face = 0x404040;
                uint32_t col_text = 0xFFFFFF;
                uint32_t col_hint = 0xA0A0A0;
                if (minwh >= 700) {
                    col_face = 0x505050;
                    col_hint = 0xB0B0B0;
                }

                api->ui->fill_rect(0, 0, w, h, col_bg);
                api->ui->draw_text("Esc - exit", 10, h - 20, col_hint);

                // Clock face: draw as a coarse circle using rectangles (no line API yet)
                for (int y = -r; y <= r; y += 2) {
                    for (int x = -r; x <= r; x += 2) {
                        int d = x * x + y * y;
                        if (d >= (r - 2) * (r - 2) && d <= r * r) {
                            api->ui->fill_rect(cx + x, cy + y, 2, 2, col_face);
                        }
                    }
                }

                int hour_angle = ((int)(hh % 12) * 30) + (int)mm / 2;

                static const int32_t sin60[60] = {
                    0,10,20,30,39,48,56,63,69,74,78,81,83,84,83,81,78,74,69,63,56,48,39,30,20,10,
                    0,-10,-20,-30,-39,-48,-56,-63,-69,-74,-78,-81,-83,-84,-83,-81,-78,-74,-69,-63,-56,-48,-39,-30,-20,-10
                };
                static const int32_t cos60[60] = {
                    84,83,81,78,74,69,63,56,48,39,30,20,10,0,-10,-20,-30,-39,-48,-56,-63,-69,-74,-78,-81,-83,
                    -84,-83,-81,-78,-74,-69,-63,-56,-48,-39,-30,-20,-10,0,10,20,30,39,48,56,63,69,74,78,81,83
                };

                int si = (ss % 60);
                int mi = (mm % 60);
                int sdx = sin60[si], sdy = -cos60[si];
                int mdx = sin60[mi], mdy = -cos60[mi];

                // Hour: map angle to 0..59 too (rough)
                int hi = (hour_angle / 6) % 60;
                int hdx = sin60[hi], hdy = -cos60[hi];

                // Draw hour hand
                for (int t = 0; t < r * 40 / 100; ++t) {
                    int px = cx + (hdx * t) / 84;
                    int py = cy + (hdy * t) / 84;
                    api->ui->fill_rect(px, py, 3, 3, col_text);
                }
                // Draw minute hand
                for (int t = 0; t < r * 65 / 100; ++t) {
                    int px = cx + (mdx * t) / 84;
                    int py = cy + (mdy * t) / 84;
                    api->ui->fill_rect(px, py, 2, 2, 0x00FF00);
                }
                // Draw second hand
                for (int t = 0; t < r * 85 / 100; ++t) {
                    int px = cx + (sdx * t) / 84;
                    int py = cy + (sdy * t) / 84;
                    api->ui->fill_rect(px, py, 1, 1, 0xFF0000);
                }

                // Digital time (scaled)
                {
                    char tbuf[9];
                    tbuf[0] = '0' + (hh / 10);
                    tbuf[1] = '0' + (hh % 10);
                    tbuf[2] = ':';
                    tbuf[3] = '0' + (mm / 10);
                    tbuf[4] = '0' + (mm % 10);
                    tbuf[5] = ':';
                    tbuf[6] = '0' + (ss / 10);
                    tbuf[7] = '0' + (ss % 10);
                    tbuf[8] = '\0';

                    int scale = r / 28;
                    if (scale < 1) scale = 1;
                    if (scale > 3) scale = 3;
                    int tw = 8 * scale * 8;
                    int tx = cx - (tw / 2);
                    int ty = cy + r / 2;
                    if (ty + 16 * scale > h - 24) ty = h - 24 - 16 * scale;
                    if (api->ui->draw_text_scale && scale > 1) {
                        api->ui->draw_text_scale(tbuf, tx, ty, col_text, scale);
                    } else {
                        api->ui->draw_text(tbuf, tx, ty, col_text);
                    }
                }
            }

            // Handle events
            mljos_ui_event_t ev;
            while (api->ui->poll_event(&ev)) {
                if (ev.type == MLJOS_UI_EVENT_EXPOSE) {
                    last_ss = -1;
                    continue;
                }
                if (ev.type == MLJOS_UI_EVENT_KEY_DOWN && ev.key == 27) {
                    api->ui->end_app();
                    return;
                }
            }
        }
    }

    unsigned char h, m, s;
    api->get_time(&h, &m, &s);
    
    api->putchar('0' + (h / 10));
    api->putchar('0' + (h % 10));
    api->putchar(':');
    api->putchar('0' + (m / 10));
    api->putchar('0' + (m % 10));
    api->putchar(':');
    api->putchar('0' + (s / 10));
    api->putchar('0' + (s % 10));
    api->putchar('\n');
}

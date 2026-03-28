#include "sdk/mljos_api.h"
#include "sdk/mljos_app.h"

MLJOS_APP_DEFINE("Time", MLJOS_APP_FLAG_TUI | MLJOS_APP_FLAG_GUI);

MLJOS_APP_ENTRY void _start(mljos_api_t *api) {
    if (api && (api->launch_flags & MLJOS_LAUNCH_GUI) && api->ui) {
        api->ui->begin_app("Time");

        const int w = (int)api->ui->screen_w();
        const int h = (int)api->ui->screen_h();
        const int cx = w / 2;
        const int cy = h / 2;
        const int r = (w < h ? w : h) / 3;

        int last_ss = -1;

        for (;;) {
            unsigned char hh, mm, ss;
            api->get_time(&hh, &mm, &ss);

            if ((int)ss != last_ss) {
                last_ss = (int)ss;

                api->ui->fill_rect(0, 0, w, h, 0x000000);
                api->ui->draw_text("Esc - exit", 10, h - 20, 0xA0A0A0);

                // Clock face: draw as a coarse circle using rectangles (no line API yet)
                for (int y = -r; y <= r; y += 2) {
                    for (int x = -r; x <= r; x += 2) {
                        int d = x * x + y * y;
                        if (d >= (r - 2) * (r - 2) && d <= r * r) {
                            api->ui->fill_rect(cx + x, cy + y, 2, 2, 0x404040);
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
                    api->ui->fill_rect(px, py, 3, 3, 0xFFFFFF);
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

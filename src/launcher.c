#include "launcher.h"

#include "console.h"
#include "disk.h"
#include "fs.h"
#include "kmem.h"
#include "kstring.h"
#include "rtc.h"
#include "shell.h"
#include "task.h"
#include "ui.h"
#include "users.h"
#include "wm.h"

static void api_get_time(uint8_t *h, uint8_t *m, uint8_t *s) {
    get_rtc_time(h, m, s);
}

static void api_get_date(uint8_t *d, uint8_t *mo, uint16_t *y) {
    get_rtc_date(d, mo, y);
}

static void fill_minimal_gui_api(mljos_api_t *api) {
    if (!api) return;
    api->launch_flags = MLJOS_LAUNCH_GUI;
    api->ui = ui_api();
    api->get_time = api_get_time;
    api->get_date = api_get_date;
}

static int load_app_image(const char *app_path, void **out_image, uint32_t *out_size) {
    if (!app_path || !out_image || !out_size) return 0;

    // 2MiB max per app (mapped as a single 2MiB page at MLJOS_APP_VADDR).
    int maxlen = 2 * 1024 * 1024;
    char *buf = (char *)kmem_alloc((uint64_t)maxlen, 16);
    uint32_t size = 0;

    int ok = 0;
    if (users_system_is_installed()) {
        // Prefer disk, fallback to RAM.
        ok = disk_read_file(app_path, buf, maxlen, &size);
        if (!ok) ok = fs_read_file(app_path, buf, maxlen, &size);
    } else {
        ok = fs_read_file(app_path, buf, maxlen, &size);
        if (!ok) ok = disk_read_file(app_path, buf, maxlen, &size);
    }

    if (!ok || size == 0) return 0;
    *out_image = buf;
    *out_size = size;
    return 1;
}

int launcher_launch_gui(const char *name) {
    return launcher_launch_gui_args(name, NULL);
}

int launcher_launch_gui_args(const char *name, const char *open_path) {
    if (!name || !name[0]) return 0;

    char app_path[128];
    if (!fs_resolve_app_command(name, app_path, sizeof(app_path))) return 0;

    void *image = NULL;
    uint32_t image_size = 0;
    if (!load_app_image(app_path, &image, &image_size)) return 0;

    wm_window_t *w = wm_window_create(name, 520, 360);
    if (!w) return 0;

    task_t *t = task_create_app(name, image, image_size);
    if (!t) {
        wm_window_destroy(w);
        return 0;
    }
    
    if (open_path) {
        int i = 0;
        while (open_path[i] && i < (int)sizeof(t->shell_open_path) - 1) {
            t->shell_open_path[i] = open_path[i];
            i++;
        }
        t->shell_open_path[i] = '\0';
    }

    // Initialize full system API for the app
    shell_init_task_api(t);

    fill_minimal_gui_api(&t->api);

    if (strcmp(name, "terminal") == 0) {
        console_t *c = console_create();
        task_attach_console(t, c);
        console_bind_target(c,
            wm_window_client_pixels(w),
            (uint32_t)wm_window_client_w(w),
            (uint32_t)wm_window_client_h(w),
            wm_window_client_pitch_bytes(w));
        console_set_visible(c, 1);
        console_redraw(c);
    }

    task_attach_window(t, w);
    wm_window_set_owner(w, t);
    wm_window_focus(w);
    return 1;
}

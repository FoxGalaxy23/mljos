#include "terminal_app.h"

#include "console.h"
#include "shell.h"
#include "task.h"
#include "wm.h"

static void terminal_task_main(void *arg) {
    (void)arg;
    task_t *t = task_current();
    if (t && t->window) {
        console_bind_target(
            wm_window_client_pixels(t->window),
            (uint32_t)wm_window_client_w(t->window),
            (uint32_t)wm_window_client_h(t->window),
            wm_window_client_pitch_bytes(t->window)
        );
        console_set_visible(1);
        clear_screen();
        wm_mark_dirty();
    }
    shell_run();
    task_exit();
}

int terminal_spawn(void) {
    wm_window_t *existing = wm_terminal_window();
    if (existing) {
        wm_window_focus(existing);
        return 1;
    }

    wm_window_t *term_win = wm_window_create("Terminal", 820, 520);
    if (!term_win) return 0;
    wm_set_terminal_window(term_win);

    task_t *term_task = task_create_kernel("terminal", terminal_task_main, NULL);
    if (!term_task) {
        wm_window_destroy(term_win);
        wm_set_terminal_window(NULL);
        return 0;
    }

    task_attach_window(term_task, term_win);
    wm_window_set_owner(term_win, term_task);
    wm_window_focus(term_win);
    return 1;
}


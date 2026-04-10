#include "terminal_app.h"

#include "console.h"
#include "shell.h"
#include "task.h"
#include "ui.h"
#include "wm.h"

static void fill_terminal_gui_api(task_t *t) {
    if (!t) return;
    shell_init_task_api(t);
    t->api.launch_flags = MLJOS_LAUNCH_GUI;
    t->api.ui = ui_api();
}

static void terminal_task_main(void *arg) {
    task_t *t = task_current();
    wm_window_t *host = (wm_window_t *)arg;
    if (t && host) {
        task_attach_window(t, host);
    }
    shell_run();
    task_exit();
}

static int terminal_add_tab(wm_window_t *term_win) {
    task_t *term_task = task_create_kernel("terminal", terminal_task_main, NULL);
    if (!term_task) return 0;

    console_t *c = console_create();
    if (!c) return 0;

    task_attach_console(term_task, c);
    task_attach_window(term_task, term_win);
    term_task->arg = term_win;
    fill_terminal_gui_api(term_task);

    {
        int tab_index = wm_window_terminal_tab_count(term_win);
        char title[24];
        int n = tab_index + 1;
        int pos = 0;
        const char *prefix = "Shell ";
        while (prefix[pos] && pos < (int)sizeof(title) - 1) {
            title[pos] = prefix[pos];
            pos++;
        }
        if (n >= 10 && pos < (int)sizeof(title) - 1) title[pos++] = (char)('0' + (n / 10) % 10);
        if (pos < (int)sizeof(title) - 1) title[pos++] = (char)('0' + (n % 10));
        title[pos] = '\0';
        if (!wm_window_add_terminal_tab(term_win, term_task, title)) return 0;
    }

    wm_window_focus(term_win);
    return 1;
}

int terminal_spawn(void) {
    wm_window_t *existing = wm_terminal_window();
    if (existing) {
        return terminal_add_tab(existing);
    }

    wm_window_t *term_win = wm_window_create("Terminal", 820, 520);
    if (!term_win) return 0;
    wm_set_terminal_window(term_win);
    if (!terminal_add_tab(term_win)) {
        wm_window_destroy(term_win);
        wm_set_terminal_window(NULL);
        return 0;
    }
    return 1;
}

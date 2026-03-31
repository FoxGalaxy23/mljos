#include "shell.h"
#include "apps_registry.h"
#include "console.h"
#include "disk.h"
#include "fs.h"
#include "launcher.h"
#include "io.h"
#include "kstring.h"
#include "rtc.h"
#include "task.h"
#include "ui.h"
#include "usb.h"
#include "users.h"
#include "wm.h"
#include "sdk/mljos_app.h"

static int app_read_file(const char *path, char *buf, int maxlen, unsigned int *size_out);
static int app_write_file(const char *path, const char *buf, unsigned int size);
static void os_set_cursor(int row, int col);
static void os_putchar_at(char ch, int row, int col);
static int os_tui_cols(void);
static int os_tui_rows(void);
static int os_read_key(void);
static int os_list_dir(const char *path, char *out, int out_size);
static int os_get_cwd(char *out, int out_size);
static int os_mkdir(const char *path);
static int os_rm(const char *path);
static void os_get_time(uint8_t *h, uint8_t *m, uint8_t *s);
static void os_get_date(uint8_t *d, uint8_t *mo, uint16_t *y);
static int os_launch_app(const char *name_or_path);
static int os_launch_app_args(const char *name_or_path, const char *open_path);

typedef enum storage_target {
    STORAGE_RAM = 0,
    STORAGE_DISK = 1
} storage_target_t;

typedef enum shell_location {
    SHELL_ROOT = 0,
    SHELL_STORAGE = 1
} shell_location_t;

static int shell_disk_primary_mode(void);

static storage_target_t g_kernel_active_storage = STORAGE_RAM;
static shell_location_t g_kernel_shell_location = SHELL_STORAGE;
static uint32_t g_kernel_launch_flags = 0;
static char g_kernel_open_path[128];

static storage_target_t *active_storage_ptr(void) {
    task_t *t = task_current();
    if (!t) return &g_kernel_active_storage;
    return (storage_target_t *)&t->shell_active_storage;
}

static shell_location_t *shell_location_ptr(void) {
    task_t *t = task_current();
    if (!t) return &g_kernel_shell_location;
    return (shell_location_t *)&t->shell_location;
}

static uint32_t *launch_flags_ptr(void) {
    task_t *t = task_current();
    if (!t) return &g_kernel_launch_flags;
    return &t->shell_launch_flags;
}

static char *open_path_ptr(void) {
    task_t *t = task_current();
    if (!t) return g_kernel_open_path;
    return t->shell_open_path;
}

#define SHELL_OPEN_PATH_MAX 128
#define active_storage (*active_storage_ptr())
#define shell_location (*shell_location_ptr())
#define g_launch_flags (*launch_flags_ptr())

mljos_api_t os_api = {
    .puts = puts,
    .putchar = putchar,
    .clear_screen = clear_screen,
    .read_line = read_line,
    .read_file = app_read_file,
    .write_file = app_write_file,
    .set_cursor = os_set_cursor,
    .putchar_at = os_putchar_at,
    .tui_cols = os_tui_cols,
    .tui_rows = os_tui_rows,
    .read_key = os_read_key,
    .list_dir = os_list_dir,
    .get_cwd = os_get_cwd,
    .mkdir = os_mkdir,
    .rm = os_rm,
    .get_time = os_get_time,
    .get_date = os_get_date,
    .open_path = g_kernel_open_path,
    .run_shell = shell_run,
    .launch_app = os_launch_app,
    .launch_app_args = os_launch_app_args,
    .launch_flags = 0,
    .ui = NULL,
};

#define HISTORY_SIZE 16

static void *g_kernel_shell_jmp_env[8];
static int g_kernel_jmp_ready = 0;
static int g_shell_booted = 0;

static void **shell_jmp_env_ptr(void) {
    task_t *t = task_current();
    if (!t) return g_kernel_shell_jmp_env;
    return t->shell_jmp_env;
}

static int *shell_jmp_ready_ptr(void) {
    task_t *t = task_current();
    if (!t) return &g_kernel_jmp_ready;
    return &t->shell_jmp_ready;
}

static char (*shell_history_ptr(void))[128] {
    task_t *t = task_current();
    if (!t) return NULL;
    return t->shell_history;
}

static int *shell_history_count_ptr(void) {
    task_t *t = task_current();
    if (!t) return NULL;
    return &t->shell_history_count;
}

static int *shell_history_pos_ptr(void) {
    task_t *t = task_current();
    if (!t) return NULL;
    return &t->shell_history_pos;
}

void shell_boot(void) {
    if (g_shell_booted) return;
    g_shell_booted = 1;
    os_api.ui = ui_api();
    users_init();
    (void)users_load_from_disk();
    fs_init();
    users_bootstrap_fs();
}

void shell_init_task_api(task_t *t) {
    if (!t) return;
    t->api = os_api;
    t->api.open_path = t->shell_open_path;
    t->api.launch_flags = 0;
    t->api.ui = NULL;
}

static void os_set_cursor(int row, int col) {
    cursor_row = row;
    cursor_col = col;
    update_cursor();
}

static void os_putchar_at(char ch, int row, int col) {
    putchar_at(ch, row, col);
}

static int os_tui_cols(void) {
    return VGA_COLS;
}

static int os_tui_rows(void) {
    return VGA_ROWS;
}

static int os_read_key(void) {
    for (;;) {
        task_t *t = task_current();
        if (t && t->killed) task_exit();
        if (t && t->window) {
            mljos_ui_event_t ev;
            while (wm_window_poll_event(t->window, &ev)) {
                if (ev.type != MLJOS_UI_EVENT_KEY_DOWN) continue;
                if (ev.key == 3 && *shell_jmp_ready_ptr()) __builtin_longjmp(shell_jmp_env_ptr(), 1); // Ctrl+C
                return ev.key;
            }
        }
        task_yield();
    }
}

void shell_set_launch_flags(uint32_t flags) {
    g_launch_flags = flags;
}

// UI API moved to src/ui.c (window-aware).

static storage_target_t resolve_storage(const char *path, const char **out_path) {
    if (strncmp(path, "disk:", 5) == 0) {
        if (out_path) *out_path = path + 5;
        return STORAGE_DISK;
    }
    if (strncmp(path, "ram:", 4) == 0) {
        if (out_path) *out_path = path + 4;
        return STORAGE_RAM;
    }
    if (out_path) *out_path = path;
    return active_storage;
}

static int os_list_dir(const char *path, char *out, int out_size) {
    const char *p;
    storage_target_t target = resolve_storage(path, &p);
    if (shell_disk_primary_mode() || target == STORAGE_DISK) {
        return disk_list_dir_file_names(p, out, out_size);
    }
    return fs_list_dir_file_names(p, out, out_size);
}

static int os_get_cwd(char *out, int out_size) {
    if (shell_disk_primary_mode() || active_storage == STORAGE_DISK) {
        const char *cwd = disk_get_cwd_path();
        if (!cwd || !out || out_size < 1) return 0;
        int i = 0;
        while (cwd[i] && i < out_size - 1) {
            out[i] = cwd[i];
            i++;
        }
        out[i] = '\0';
        return 1;
    }
    
    if (out && out_size > 1) {
        fs_get_cwd_path(out, out_size);
        return 1;
    }
    return 0;
}

static int os_mkdir(const char *path) {
    const char *p;
    storage_target_t target = resolve_storage(path, &p);
    if (shell_disk_primary_mode() || target == STORAGE_DISK) {
        return disk_ensure_directory(p);
    }
    cmd_mkdir_p(p);
    return 1;
}

static int os_rm(const char *path) {
    const char *p;
    storage_target_t target = resolve_storage(path, &p);
    if (shell_disk_primary_mode() || target == STORAGE_DISK) {
        cmd_disk_rm(p);
        return 1;
    }
    cmd_rm(p);
    return 1;
}

static void os_get_time(uint8_t *h, uint8_t *m, uint8_t *s) {
    get_rtc_time(h, m, s);
}

static void os_get_date(uint8_t *d, uint8_t *mo, uint16_t *y) {
    get_rtc_date(d, mo, y);
}

static int os_launch_app(const char *name_or_path) {
    // Currently relying on the GUI launcher for all async launches
    return launcher_launch_gui(name_or_path);
}

static int os_launch_app_args(const char *name_or_path, const char *open_path) {
    return launcher_launch_gui_args(name_or_path, open_path);
}

// Shell history is per-task; stored in task_t fields.

static void handle_command(char *line);
static int shell_disk_primary_mode(void);
static int shell_try_launch_app_command(const char *name);
void shell_exec_app_command(const char *name);
static int shell_try_launch_script_command(const char *name);
static int shell_exec_script_file(const char *path, int quiet_errors);
static void shell_run_autorun_scripts(void);

static int app_read_file(const char *path, char *buf, int maxlen, unsigned int *size_out) {
    const char *p;
    storage_target_t target = resolve_storage(path, &p);
    if (shell_disk_primary_mode() || target == STORAGE_DISK) {
        return disk_read_file(p, buf, maxlen, (uint32_t*)size_out);
    }
    return fs_read_file(p, buf, maxlen, (uint32_t*)size_out);
}

static int app_write_file(const char *path, const char *buf, unsigned int size) {
    const char *p;
    storage_target_t target = resolve_storage(path, &p);
    if (shell_disk_primary_mode() || target == STORAGE_DISK) {
        return disk_write_file(p, buf, size);
    }
    return fs_write_file(p, buf, (uint32_t)size);
}

static int shell_disk_primary_mode(void) {
    return users_system_is_installed();
}

static void print_disk_storage_name(void) {
    int active_disk = disk_get_active_device();

    puts("disk");
    if (active_disk >= 0 && active_disk <= 9) putchar('0' + active_disk);
}

static const char *storage_name(storage_target_t storage) {
    return storage == STORAGE_DISK ? "disk" : "ram";
}

static void print_active_path(void) {
    if (shell_disk_primary_mode() && active_storage == STORAGE_DISK) {
        puts(disk_get_cwd_path());
        return;
    }

    if (shell_location == SHELL_ROOT) {
        putchar('/');
        return;
    }

    if (active_storage == STORAGE_DISK) {
        print_disk_storage_name();
        putchar(':');
        puts(disk_get_cwd_path());
    } else {
        puts(storage_name(active_storage));
        putchar(':');
        fs_print_prompt_path();
    }
}

static void cmd_time(void) {
    shell_exec_app_command("time");
}

static void cmd_date(void) {
    shell_exec_app_command("date");
}

static void cmd_echo(const char *rest) {
    char app_path[128];
    if (fs_resolve_app_command("echo", app_path, sizeof(app_path))) {
        if (rest) {
            int i = 0;
            while (rest[i] && i < 127) {
                open_path_ptr()[i] = rest[i];
                i++;
            }
            open_path_ptr()[i] = '\0';
        } else {
            open_path_ptr()[0] = '\0';
        }
        shell_exec_app_command("echo");
    }
}

static void cmd_reboot(void) {
    uint8_t old_color = COLOR;
    COLOR = COLOR_ALERT;
    puts("Rebooting...\n");
    COLOR = old_color;
    outb(0x64, 0xFE);
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

static void cmd_shutdown(void) {
    uint8_t old_color = COLOR;
    COLOR = COLOR_ALERT;
    puts("Shutdown...\n");
    COLOR = old_color;
    outw(0x604, 0x2000);
    outb(0x64, 0xFE);
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

static void push_history(const char *line) {
    if (!line || !line[0]) return;

    char (*history)[128] = shell_history_ptr();
    int *history_count = shell_history_count_ptr();
    if (!history || !history_count) return;

    int idx = (*history_count) % HISTORY_SIZE;
    int i;
    for (i = 0; i < 127 && line[i]; ++i) history[idx][i] = line[i];
    history[idx][i] = '\0';
    (*history_count)++;
}

static const char *history_get(int pos_from_latest) {
    char (*history)[128] = shell_history_ptr();
    int *history_count = shell_history_count_ptr();
    if (!history || !history_count) return NULL;
    if (*history_count == 0 || pos_from_latest < 0) return NULL;

    {
        int avail = *history_count < HISTORY_SIZE ? *history_count : HISTORY_SIZE;
        int idx;
        if (pos_from_latest >= avail) return NULL;

        idx = (*history_count - 1 - pos_from_latest) % HISTORY_SIZE;
        if (idx < 0) idx += HISTORY_SIZE;
        return history[idx];
    }
}

static void clear_input_visual(int prompt_row, int prompt_col) {
    for (int c = prompt_col; c < VGA_COLS; ++c) putchar_at(' ', prompt_row, c);
    cursor_row = prompt_row;
    cursor_col = prompt_col;
    update_cursor();
}

static int read_line_internal(char *buf, int maxlen, int hide_input, int allow_history) {
    int len = 0;
    int prompt_row = cursor_row;
    int prompt_col = cursor_col;
    uint8_t old_color = COLOR;
    int *history_count = shell_history_count_ptr();
    int *history_pos = shell_history_pos_ptr();

    if (history_pos) *history_pos = -1;
    COLOR = COLOR_DEFAULT;
    update_cursor();

    while (1) {
        int c = os_read_key();
        if (c == 0) continue;

        // Special arrows (from WM key translation).
        if (c == 1000) { // Up
            if (!allow_history || !history_count || !history_pos || *history_count == 0) continue;
            if (*history_pos < 0) *history_pos = 0;
            else if (*history_pos < HISTORY_SIZE - 1) (*history_pos)++;
            const char *h = history_get(*history_pos);
            if (h) {
                clear_input_visual(prompt_row, prompt_col);
                int i = 0;
                while (h[i] && i < maxlen - 1) {
                    buf[i] = h[i];
                    putchar_at(h[i], cursor_row, cursor_col++);
                    if (cursor_col >= VGA_COLS) {
                        cursor_col = 0;
                        cursor_row++;
                        scroll_if_needed();
                    }
                    i++;
                }
                len = i;
                buf[len] = '\0';
                update_cursor();
            }
            continue;
        }
        if (c == 1001) { // Down
            if (!allow_history || !history_count || !history_pos || *history_count == 0) continue;
            if (*history_pos <= 0) {
                *history_pos = -1;
                clear_input_visual(prompt_row, prompt_col);
                len = 0;
                buf[0] = '\0';
                update_cursor();
                continue;
            }
            (*history_pos)--;
            const char *h = history_get(*history_pos);
            if (h) {
                clear_input_visual(prompt_row, prompt_col);
                int i = 0;
                while (h[i] && i < maxlen - 1) {
                    buf[i] = h[i];
                    putchar_at(h[i], cursor_row, cursor_col++);
                    if (cursor_col >= VGA_COLS) {
                        cursor_col = 0;
                        cursor_row++;
                        scroll_if_needed();
                    }
                    i++;
                }
                len = i;
                buf[len] = '\0';
                update_cursor();
            }
            continue;
        }
        if (c == 1002) { // Left
            if (len > 0) {
                if (cursor_col == 0) {
                    if (cursor_row > 0) {
                        cursor_row--;
                        cursor_col = VGA_COLS - 1;
                    } else cursor_col = 0;
                } else cursor_col--;
                update_cursor();
            }
            continue;
        }
        if (c == 1003) { // Right
            if (len < maxlen - 1) {
                if (cursor_col == VGA_COLS - 1) {
                    cursor_col = 0;
                    cursor_row++;
                    scroll_if_needed();
                } else cursor_col++;
                update_cursor();
            }
            continue;
        }

        if (c == '\n' || c == '\r') {
            putchar('\n');
            buf[len] = '\0';
            if (allow_history && len > 0) push_history(buf);
            COLOR = old_color;
            update_cursor();
            return len;
        }

        if (c == '\b') {
            if (len > 0) {
                if (cursor_col == 0) {
                    if (cursor_row > 0) {
                        cursor_row--;
                        cursor_col = VGA_COLS - 1;
                    } else cursor_col = 0;
                } else cursor_col--;
                putchar_at(' ', cursor_row, cursor_col);
                len--;
                buf[len] = '\0';
                update_cursor();
            }
            continue;
        }

        if (c == '\t') {
            int t = 4 - (len % 4);
            while (t-- && len < maxlen - 1) {
                buf[len++] = ' ';
                putchar_at(' ', cursor_row, cursor_col);
                cursor_col++;
                if (cursor_col >= VGA_COLS) {
                    cursor_col = 0;
                    cursor_row++;
                    scroll_if_needed();
                }
            }
            buf[len] = '\0';
            update_cursor();
            continue;
        }

        if (len < maxlen - 1) {
            buf[len++] = (char)c;
            if (!hide_input) {
                putchar_at((char)c, cursor_row, cursor_col);
                cursor_col++;
                if (cursor_col >= VGA_COLS) {
                    cursor_col = 0;
                    cursor_row++;
                    scroll_if_needed();
                }
            }
            buf[len] = '\0';
            update_cursor();
        }
    }
}

int read_line(char *buf, int maxlen) {
    return read_line_internal(buf, maxlen, 0, 1);
}

int read_secret_line(char *buf, int maxlen) {
    return read_line_internal(buf, maxlen, 1, 0);
}

static int split_args(char *line, char **argv, int maxargv) {
    int argc = 0;
    char *p = line;

    while (*p && argc < maxargv) {
        while (*p == ' ') p++;
        if (!*p) break;

        argv[argc++] = p;
        if (*p == '"') {
            char *dst = p;
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) p++;
                *dst++ = *p++;
            }
            if (*p == '"') p++;
            *dst = '\0';
        } else {
            char *dst = p;
            while (*p && *p != ' ') {
                if (*p == '\\' && p[1]) p++;
                *dst++ = *p++;
            }
            if (*p == ' ') {
                *dst = '\0';
                p++;
            } else *dst = '\0';
        }
    }
    return argc;
}

static void join_args(char *out, int out_size, char **argv, int start, int argc) {
    int pos = 0;
    out[0] = '\0';

    for (int i = start; i < argc && pos < out_size - 1; i++) {
        if (i > start && pos < out_size - 1) out[pos++] = ' ';
        for (int j = 0; argv[i][j] && pos < out_size - 1; j++) out[pos++] = argv[i][j];
    }
    out[pos] = '\0';
}

static int parse_octal_mode(const char *text, uint16_t *mode_out) {
    uint16_t value = 0;
    int len = 0;

    if (!text || !text[0]) return 0;
    while (text[len]) {
        if (text[len] < '0' || text[len] > '7') return 0;
        value = (uint16_t)((value << 3) | (text[len] - '0'));
        len++;
    }
    if (len > 4) return 0;
    *mode_out = value;
    return 1;
}

static int parse_decimal_number(const char *text, int *value_out) {
    int value = 0;

    if (!text || !text[0]) return 0;
    for (int i = 0; text[i]; i++) {
        if (text[i] < '0' || text[i] > '9') return 0;
        value = (value * 10) + (text[i] - '0');
    }

    *value_out = value;
    return 1;
}

static void print_usb_help(void) {
    puts("USB: usb, usb controllers, usb ports <controller>, usb reset <controller> <port>, usb probe <controller> <port>, usb storage <controller> <port>, usb read <controller> <port> [lba]\n");
}

// Removed mkdir_disk_parents as it is no longer used

static void cmd_mkdir_active(const char *path, int create_parents);
static void cmd_rmdir_active(const char *path);
static void run_install_wizard(void);

static void print_prompt(void) {
    const user_account_t *user = users_current();
    uint8_t old_color = COLOR;

    COLOR = COLOR_PROMPT;
    puts(user ? user->username : "root");
    puts("@mljOS:");
    print_active_path();
    puts(users_is_root() ? "# " : "$ ");
    COLOR = old_color;
}

static void print_storage_help(void) {
    puts("Users: login, logout, whoami, id [user], users, groups [user], su <user>, sudo <cmd>\n");
    puts("Admin: useradd <name> <pass> [admin], userdel <name>, passwd [user], chmod <mode> <path>, chown <user> <path>, umask [mode]\n");
    if (shell_disk_primary_mode()) puts("Root: disk-backed session, cd <path>, cd /, pwd\n");
    else puts("Root: ls, cd ram, cd disk, cd /\n");
    puts("Files: ls [path], cd <path>, pwd, mkdir <path>, mkdir -p <path>, rmdir <path>, touch <path>, rm <path>, cat <path>, write <path> <text>, cp <src> <dst>\n");
    puts("Disk: disk devices, disk use <n>, disk format, disk ls/cd/pwd/mkdir/write/cat/rm\n");
    puts("Apps: bundled apps are stored in /apps and can be launched by name, like calc or edit\n");
    puts("Apps (GUI): `open <app>` launches the app in a window (if it supports GUI)\n");
    puts("Editor: `edit [path]` opens a file in the built-in editor\n");
    puts("System: install, exec <app|path>, usb, clear, help, shutdown, reboot\n");
    puts("Scripts: .scri in /system/autorun run on boot (run by typing file name)\n");
    print_usb_help();
}

static int shell_try_launch_app_command(const char *name) {
    char app_path[128];

    if (!name || !name[0]) return 0;

    // Prefer TUI for hybrid apps; GUI-only apps run in their own windowed task.
    uint32_t flags = apps_registry_app_flags(name);
    if ((flags & MLJOS_APP_FLAG_GUI) && !(flags & MLJOS_APP_FLAG_TUI)) {
        return launcher_launch_gui(name);
    }

    if (!fs_resolve_app_command(name, app_path, sizeof(app_path))) return 0;

    if (shell_disk_primary_mode() || active_storage == STORAGE_DISK) {
        if (disk_can_exec_path(app_path)) {
            cmd_disk_exec(app_path);
            return 1;
        }
        // Fallback to RAM in live mode if not found on disk
        if (!shell_disk_primary_mode() && fs_can_exec_path(app_path)) {
            cmd_ram_exec(app_path);
            return 1;
        }
        return 0;
    }

    if (!fs_can_exec_path(app_path)) return 0;
    cmd_ram_exec(app_path);
    return 1;
}

void shell_exec_app_command(const char *name) {
    char app_path[128];
    mljos_api_t *api = task_current_api();
    if (!api || !api->puts) api = &os_api;
    uint32_t prev_flags = api->launch_flags;
    api->launch_flags = g_launch_flags;

    if (!name || !name[0]) {
        puts("exec: missing application name\n");
        api->launch_flags = prev_flags;
        return;
    }
    if (!fs_resolve_app_command(name, app_path, sizeof(app_path))) {
        puts("exec: invalid application name\n");
        api->launch_flags = prev_flags;
        return;
    }

    if (shell_disk_primary_mode() || active_storage == STORAGE_DISK) {
        if (disk_can_exec_path(app_path)) {
            cmd_disk_exec(app_path);
        } else if (disk_get_system_device() >= 0 && disk_get_system_device() != disk_get_active_device()) {
            int current = disk_get_active_device();
            disk_select_device(disk_get_system_device());
            if (disk_can_exec_path(app_path)) {
                cmd_disk_exec(app_path);
                disk_select_device(current);
            } else {
                disk_select_device(current);
                if (!shell_disk_primary_mode() && fs_can_exec_path(app_path)) {
                    cmd_ram_exec(app_path);
                } else {
                    cmd_disk_exec(app_path); // Let it show the "file not found" error
                }
            }
        } else if (!shell_disk_primary_mode() && fs_can_exec_path(app_path)) {
            cmd_ram_exec(app_path);
        } else {
            cmd_disk_exec(app_path); // Let it show the "file not found" error
        }
    } else {
        cmd_ram_exec(app_path);
    }

    api->launch_flags = prev_flags;
}

static void enter_logged_user_home(void) {
    const user_account_t *user = users_current();

    if (shell_disk_primary_mode()) {
        active_storage = STORAGE_DISK;
        shell_location = SHELL_STORAGE;
        disk_prepare_session();
        if (user && user->home[0]) {
            (void)disk_ensure_directory(user->home);
            cmd_disk_cd(user->home);
        } else {
            cmd_disk_cd("/");
        }
        return;
    }

    active_storage = STORAGE_RAM;
    shell_location = SHELL_STORAGE;
    fs_enter_home();
}

static void enter_storage(storage_target_t storage) {
    if (shell_disk_primary_mode() && storage == STORAGE_RAM) {
        puts("cd: ram storage is disabled in installed mode\n");
        return;
    }
    active_storage = storage;
    shell_location = SHELL_STORAGE;
    if (storage == STORAGE_DISK) disk_prepare_session();
    else fs_enter_home();
}

static int is_at_storage_root(void) {
    if (shell_disk_primary_mode()) return 0;
    if (shell_location != SHELL_STORAGE) return 0;
    if (active_storage == STORAGE_DISK) return strcmp(disk_get_cwd_path(), "/") == 0;
    return strcmp(storage_name(active_storage), "ram") == 0 && fs_current_dir() == fs_root;
}

static void reset_user_session(void) {
    enter_logged_user_home();
}

static void do_login(void) {
    char username[32];
    char password[32];

    while (1) {
        puts("login: ");
        read_line(username, sizeof(username));
        puts("password: ");
        read_secret_line(password, sizeof(password));
        if (users_login(username, password)) {
            reset_user_session();
            puts("Login successful\n\n");
            return;
        }
        puts("Login incorrect\n\n");
    }
}

static void cmd_cd_active(const char *path) {
    if (!path || !path[0]) {
        if (shell_disk_primary_mode()) enter_logged_user_home();
        else if (shell_location == SHELL_STORAGE && active_storage == STORAGE_RAM) cmd_cd(NULL);
        else puts("cd: missing operand\n");
        return;
    }

    if (strcmp(path, "/") == 0) {
        if (shell_disk_primary_mode()) {
            cmd_disk_cd("/");
            return;
        }
        if (shell_location == SHELL_ROOT) return;
        if (active_storage == STORAGE_DISK) cmd_disk_cd("/");
        else cmd_cd("/");
        shell_location = SHELL_ROOT;
        return;
    }

    if (shell_disk_primary_mode()) {
        cmd_disk_cd(path);
        return;
    }

    if (shell_location == SHELL_ROOT) {
        if (strcmp(path, "ram") == 0) enter_storage(STORAGE_RAM);
        else if (strcmp(path, "disk") == 0) enter_storage(STORAGE_DISK);
        else puts("cd: unknown storage\n");
        return;
    }

    if (strcmp(path, "..") == 0 && is_at_storage_root()) {
        shell_location = SHELL_ROOT;
        return;
    }

    if (active_storage == STORAGE_DISK) cmd_disk_cd(path);
    else cmd_cd(path);
}

// Removed redundant cmd_*_active functions as they are now handled by standalone apps

static void cmd_write_active(const char *path, const char *text) {
    if (shell_disk_primary_mode()) {
        cmd_disk_write(path, text);
        return;
    }
    if (shell_location == SHELL_ROOT) {
        puts("write: select a storage first with cd ram or cd disk\n");
        return;
    }
    if (active_storage == STORAGE_DISK) cmd_disk_write(path, text);
    else cmd_write(path, text);
}

static void handle_sudo(char **argv, int argc) {
    char command[128];
    char password[32];

    if (argc < 2) {
        puts("sudo: missing command\n");
        return;
    }
    if (users_is_root()) {
        join_args(command, sizeof(command), argv, 1, argc);
        handle_command(command);
        return;
    }
    if (!users_current_can_sudo()) {
        puts("sudo: user is not in sudoers\n");
        return;
    }

    puts("sudo password: ");
    read_secret_line(password, sizeof(password));
    if (!users_begin_sudo(password)) {
        puts("sudo: authentication failed\n");
        return;
    }

    join_args(command, sizeof(command), argv, 1, argc);
    handle_command(command);
    users_end_sudo();
}

static void handle_su(char **argv, int argc) {
    char password[32];

    if (argc < 2) {
        puts("su: missing user\n");
        return;
    }

    puts("password: ");
    read_secret_line(password, sizeof(password));
    if (!users_su(argv[1], password)) {
        puts("su: authentication failed\n");
        return;
    }

    reset_user_session();
}

static void handle_passwd(char **argv, int argc) {
    const user_account_t *user = users_current();
    const char *target = NULL;
    char password[32];

    if (argc == 1) {
        target = user ? user->username : NULL;
    } else if (argc == 2) {
        if (!users_can_manage_accounts()) {
            puts("passwd: only root can change another user's password\n");
            return;
        }
        target = argv[1];
    } else {
        puts("passwd: usage passwd [user]\n");
        return;
    }

    puts("new password: ");
    read_secret_line(password, sizeof(password));
    if (!users_set_password(target, password)) puts("passwd: unable to update password\n");
    else puts("passwd: password updated\n");
}

static void handle_useradd(char **argv, int argc) {
    uint8_t role = USER_ROLE_USER;
    int can_sudo = 0;

    if (!users_can_manage_accounts()) {
        puts("useradd: only root can add users\n");
        return;
    }
    if (argc < 3) {
        puts("useradd: usage useradd <name> <pass> [admin]\n");
        return;
    }
    if (argc > 3 && strcmp(argv[3], "admin") == 0) {
        role = USER_ROLE_ADMIN;
        can_sudo = 1;
    }

    if (!users_add(argv[1], argv[2], role, can_sudo)) puts("useradd: failed to create user\n");
    else puts("useradd: user created\n");
}

static void handle_userdel(char **argv, int argc) {
    if (!users_can_manage_accounts()) {
        puts("userdel: only root can delete users\n");
        return;
    }
    if (argc < 2) {
        puts("userdel: usage userdel <name>\n");
        return;
    }
    if (!users_remove(argv[1])) puts("userdel: failed to remove user\n");
    else puts("userdel: user removed\n");
}

static void handle_umask(char **argv, int argc) {
    uint16_t mode;
    uint16_t current = fs_get_umask();
    char a = '0' + (char)((current >> 6) & 0x7);
    char b = '0' + (char)((current >> 3) & 0x7);
    char c = '0' + (char)(current & 0x7);

    if (argc < 2) {
        putchar(a);
        putchar(b);
        putchar(c);
        putchar('\n');
        return;
    }
    if (!parse_octal_mode(argv[1], &mode)) {
        puts("umask: use octal mode like 022 or 077\n");
        return;
    }
    fs_set_umask(mode);
}

static void run_install_wizard(void) {
    char username[32];
    char user_password[32];
    char root_password[32];
    char autologin_answer[8];
    int enable_autologin = 0;

    puts("installer user name: ");
    read_line(username, sizeof(username));
    puts("installer user password: ");
    read_secret_line(user_password, sizeof(user_password));
    puts("new root password: ");
    read_secret_line(root_password, sizeof(root_password));
    puts("enable autologin after install? (y/n): ");
    read_line(autologin_answer, sizeof(autologin_answer));
    enable_autologin = autologin_answer[0] == 'y' || autologin_answer[0] == 'Y';

    if (!users_setup_install_owner(username, user_password, root_password, enable_autologin)) {
        puts("install: unable to prepare user accounts\n");
        return;
    }

    cmd_disk_install();
    if (!fs_sync_to_disk()) {
        puts("install: warning, failed to copy RAM filesystem to FAT32\n");
        return;
    }
    users_persist();
    puts("install: filesystem and users copied to disk\n");
}

static int shell_ends_with(const char *text, const char *suffix) {
    unsigned int text_len = strlen(text);
    unsigned int suffix_len = strlen(suffix);
    if (suffix_len > text_len) return 0;
    return strcmp(text + (text_len - suffix_len), suffix) == 0;
}

static char *shell_trim_left(char *s) {
    while (s && (*s == ' ' || *s == '\t')) s++;
    return s;
}

static void shell_path_join(char *out, int out_size, const char *dir, const char *leaf) {
    int pos = 0;
    if (!out || out_size <= 0) return;

    if (dir) {
        while (dir[pos] && pos < out_size - 1) {
            out[pos] = dir[pos];
            pos++;
        }
    }

    if (pos > 0 && out[pos - 1] != '/' && pos < out_size - 1) out[pos++] = '/';

    if (leaf) {
        for (int i = 0; leaf[i] && pos < out_size - 1; i++) out[pos++] = leaf[i];
    }

    out[pos] = '\0';
}

static int shell_exec_script_file(const char *path, int quiet_errors) {
    enum { SCRIPT_BUF_SIZE = 4096, LINE_BUF_SIZE = 128 };
    char script_buf[SCRIPT_BUF_SIZE];
    char line_buf[LINE_BUF_SIZE];
    unsigned int script_size = 0;
    unsigned int i = 0;
    int read_ok = 0;

    if (!path || !path[0]) return 0;

    // app_read_file chooses RAM vs disk based on current shell state.
    read_ok = app_read_file(path, script_buf, (int)sizeof(script_buf) - 1, &script_size);
    if (!read_ok) {
        if (!quiet_errors) {
            puts("script: unable to read ");
            puts(path);
            putchar('\n');
        }
        return 0;
    }

    while (i < script_size) {
        unsigned int l = 0;

        while (i < script_size && script_buf[i] != '\n' && script_buf[i] != '\r') {
            if (l < sizeof(line_buf) - 1) line_buf[l++] = script_buf[i];
            i++;
        }

        line_buf[l] = '\0';

        if (i < script_size && script_buf[i] == '\r') i++;
        if (i < script_size && script_buf[i] == '\n') i++;

        char *cmd = shell_trim_left(line_buf);
        if (!cmd[0] || cmd[0] == '#') continue;

        handle_command(cmd);
    }

    return 1;
}

static int shell_try_launch_script_command(const char *name) {
    if (!name || !name[0]) return 0;
    if (!shell_ends_with(name, ".scri")) return 0;

    // Consider it "handled" by extension: if it exists but can't be read,
    // show an error instead of falling back to "Unknown command".
    (void)shell_exec_script_file(name, 0);
    return 1;
}

static void shell_run_autorun_scripts(void) {
    char names_buf[1024];
    const char *dir_path = "/system/autorun";
    int ok;
    int p = 0;
    char script_path[192];

    names_buf[0] = '\0';

    if (shell_disk_primary_mode() || active_storage == STORAGE_DISK)
        ok = disk_list_dir_file_names(dir_path, names_buf, (int)sizeof(names_buf));
    else
        ok = fs_list_dir_file_names(dir_path, names_buf, (int)sizeof(names_buf));

    if (!ok || !names_buf[0]) return;

    while (names_buf[p]) {
        int n = 0;
        char filename[64];

        while (names_buf[p] && names_buf[p] != '\n' && n < (int)sizeof(filename) - 1) {
            filename[n++] = names_buf[p++];
        }
        filename[n] = '\0';
        if (names_buf[p] == '\n') p++;

        if (shell_ends_with(filename, ".scri")) {
            shell_path_join(script_path, (int)sizeof(script_path), dir_path, filename);
            (void)shell_exec_script_file(script_path, 1);
        }
    }
}

static void handle_command(char *line) {
    uint8_t old_color = COLOR;
    char line_copy[128];
    char *argv[12];
    char joined[128];
    int argc;

    COLOR = COLOR_DEFAULT;
    if (line[0] == '\0') {
        COLOR = old_color;
        return;
    }

    strcpy(line_copy, line);
    argc = split_args(line_copy, argv, 12);
    if (argc == 0) {
        COLOR = old_color;
        return;
    }

    if (strcmp(argv[0], "time") == 0) {
        cmd_time();
    } else if (strcmp(argv[0], "date") == 0) {
        cmd_date();
    } else if (strcmp(argv[0], "echo") == 0) {
        if (argc > 1) join_args(joined, sizeof(joined), argv, 1, argc);
        else joined[0] = '\0';
        cmd_echo(joined);
    } else if (strcmp(argv[0], "reboot") == 0) {
        cmd_reboot();
    } else if (strcmp(argv[0], "shutdown") == 0) {
        cmd_shutdown();
    } else if (strcmp(argv[0], "clear") == 0) {
        shell_exec_app_command("clear");
    } else if (strcmp(argv[0], "login") == 0 || strcmp(argv[0], "logout") == 0) {
        do_login();
    } else if (strcmp(argv[0], "whoami") == 0) {
        const user_account_t *user = users_current();
        puts(user ? user->username : "unknown");
        putchar('\n');
    } else if (strcmp(argv[0], "id") == 0) {
        users_print_id(argc > 1 ? argv[1] : NULL);
    } else if (strcmp(argv[0], "groups") == 0) {
        users_print_groups(argc > 1 ? argv[1] : NULL);
    } else if (strcmp(argv[0], "users") == 0) {
        users_list();
    } else if (strcmp(argv[0], "su") == 0) {
        handle_su(argv, argc);
    } else if (strcmp(argv[0], "sudo") == 0) {
        handle_sudo(argv, argc);
    } else if (strcmp(argv[0], "useradd") == 0) {
        handle_useradd(argv, argc);
    } else if (strcmp(argv[0], "userdel") == 0) {
        handle_userdel(argv, argc);
    } else if (strcmp(argv[0], "passwd") == 0) {
        handle_passwd(argv, argc);
    } else if (strcmp(argv[0], "umask") == 0) {
        handle_umask(argv, argc);
    } else if (strcmp(argv[0], "chmod") == 0) {
        if (shell_disk_primary_mode()) puts("chmod: FAT32 metadata is not implemented for installed mode\n");
        else if (shell_location == SHELL_ROOT || active_storage != STORAGE_RAM) puts("chmod: available only in ram storage\n");
        else if (argc > 2) cmd_chmod(argv[1], argv[2]);
        else puts("chmod: usage chmod <mode> <path>\n");
    } else if (strcmp(argv[0], "chown") == 0) {
        if (shell_disk_primary_mode()) puts("chown: FAT32 metadata is not implemented for installed mode\n");
        else if (shell_location == SHELL_ROOT || active_storage != STORAGE_RAM) puts("chown: available only in ram storage\n");
        else if (argc > 2) cmd_chown(argv[1], argv[2]);
        else puts("chown: usage chown <user> <path>\n");
    } else if (strcmp(argv[0], "install") == 0) {
        if (!users_effective_is_root()) puts("install: requires root\n");
        else run_install_wizard();
    } else if (strcmp(argv[0], "ls") == 0) {
        if (argc > 1) {
            strncpy(open_path_ptr(), argv[1], SHELL_OPEN_PATH_MAX);
            open_path_ptr()[SHELL_OPEN_PATH_MAX - 1] = '\0';
        } else open_path_ptr()[0] = '\0';
        shell_exec_app_command("ls");
        open_path_ptr()[0] = '\0';
    } else if (strcmp(argv[0], "cd") == 0) {
        cmd_cd_active(argc > 1 ? argv[1] : NULL);
    } else if (strcmp(argv[0], "pwd") == 0) {
        shell_exec_app_command("pwd");
    } else if (strcmp(argv[0], "mkdir") == 0) {
        if (argc > 1) {
            strncpy(open_path_ptr(), argv[argc - 1], SHELL_OPEN_PATH_MAX);
            open_path_ptr()[SHELL_OPEN_PATH_MAX - 1] = '\0';
        } else open_path_ptr()[0] = '\0';
        shell_exec_app_command("mkdir");
        open_path_ptr()[0] = '\0';
    } else if (strcmp(argv[0], "rmdir") == 0) {
        if (argc > 1) {
            strncpy(open_path_ptr(), argv[1], SHELL_OPEN_PATH_MAX);
            open_path_ptr()[SHELL_OPEN_PATH_MAX - 1] = '\0';
        } else open_path_ptr()[0] = '\0';
        shell_exec_app_command("rm"); // use rm app for rmdir if simple
        open_path_ptr()[0] = '\0';
    } else if (strcmp(argv[0], "touch") == 0) {
        if (argc > 1) {
            strncpy(open_path_ptr(), argv[1], SHELL_OPEN_PATH_MAX);
            open_path_ptr()[SHELL_OPEN_PATH_MAX - 1] = '\0';
        } else open_path_ptr()[0] = '\0';
        shell_exec_app_command("touch");
        open_path_ptr()[0] = '\0';
    } else if (strcmp(argv[0], "rm") == 0) {
        if (argc > 1) {
            strncpy(open_path_ptr(), argv[1], SHELL_OPEN_PATH_MAX);
            open_path_ptr()[SHELL_OPEN_PATH_MAX - 1] = '\0';
        } else open_path_ptr()[0] = '\0';
        shell_exec_app_command("rm");
        open_path_ptr()[0] = '\0';
    } else if (strcmp(argv[0], "cat") == 0) {
        if (argc > 1) {
            strncpy(open_path_ptr(), argv[1], SHELL_OPEN_PATH_MAX);
            open_path_ptr()[SHELL_OPEN_PATH_MAX - 1] = '\0';
        } else open_path_ptr()[0] = '\0';
        shell_exec_app_command("cat");
        open_path_ptr()[0] = '\0';
    } else if (strcmp(argv[0], "cp") == 0) {
        if (shell_disk_primary_mode()) {
            if (argc > 2) {
                if (!disk_copy_file(argv[1], argv[2])) puts("cp: failed to copy file on disk\n");
            } else puts("cp: missing operands\n");
        } else if (shell_location == SHELL_ROOT) puts("cp: select a storage first with cd ram\n");
        else if (active_storage == STORAGE_DISK) puts("cp: available only for ram storage\n");
        else if (argc > 2) cmd_cp(argv[1], argv[2]);
        else puts("cp: missing operands\n");
    } else if (strcmp(argv[0], "write") == 0) {
        if (argc > 2) {
            join_args(joined, sizeof(joined), argv, 2, argc);
            cmd_write_active(argv[1], joined);
        } else puts("write: missing file or text\n");
    } else if (strcmp(argv[0], "edit") == 0 || strcmp(argv[0], "microcoder") == 0) {
        if (argc > 1) {
            // Allow "some path.txt" by re-joining all remaining args.
            join_args(joined, sizeof(joined), argv, 1, argc);
            strncpy(open_path_ptr(), joined, SHELL_OPEN_PATH_MAX);
            open_path_ptr()[SHELL_OPEN_PATH_MAX - 1] = '\0';
        } else {
            open_path_ptr()[0] = '\0';
        }

        shell_exec_app_command(argv[0]);

        // Clear after launching app to avoid leaking path into the next run
        open_path_ptr()[0] = '\0';
    } else if (strcmp(argv[0], "disk") == 0) {
        if (argc < 2) puts("disk: missing command (devices, use, format, probe, ls, cd, pwd, mkdir, write, cat, rm)\n");
        else if (strcmp(argv[1], "devices") == 0 || strcmp(argv[1], "list") == 0) {
            cmd_disk_devices();
        } else if (strcmp(argv[1], "probe") == 0) {
            disk_probe_devices_reset();
            cmd_disk_devices();
        } else if (strcmp(argv[1], "use") == 0) {
            int disk_index = -1;

            if (argc < 3) puts("disk use: missing disk index\n");
            else if (!parse_decimal_number(argv[2], &disk_index)) puts("disk use: invalid disk index\n");
            else if (!disk_select_device(disk_index)) puts("disk use: disk not found\n");
            else {
                disk_prepare_session();
                puts("disk use: switched to disk");
                putchar('0' + disk_index);
                putchar('\n');
            }
        }
        else if (strcmp(argv[1], "format") == 0) {
            if (!users_effective_is_root()) puts("disk format: requires root\n");
            else cmd_disk_format();
        } else if (strcmp(argv[1], "ls") == 0) {
            if (argc > 2) cmd_disk_ls(argv[2]);
            else cmd_disk_ls(NULL);
        } else if (strcmp(argv[1], "cd") == 0) {
            if (argc > 2) cmd_disk_cd(argv[2]);
            else cmd_disk_cd("/");
        } else if (strcmp(argv[1], "pwd") == 0) {
            cmd_disk_pwd();
        } else if (strcmp(argv[1], "mkdir") == 0) {
            if (argc > 2) cmd_disk_mkdir(argv[2]);
            else puts("disk mkdir: missing path\n");
        } else if (strcmp(argv[1], "rm") == 0) {
            if (argc > 2) cmd_disk_rm(argv[2]);
            else puts("disk rm: missing path\n");
        } else if (strcmp(argv[1], "cat") == 0) {
            if (argc > 2) cmd_disk_cat(argv[2]);
            else puts("disk cat: missing path\n");
        } else if (strcmp(argv[1], "write") == 0) {
            if (argc > 3) {
                join_args(joined, sizeof(joined), argv, 3, argc);
                cmd_disk_write(argv[2], joined);
            } else puts("disk write: missing path or text\n");
        } else puts("disk: unknown command\n");
    } else if (strcmp(argv[0], "exec") == 0) {
        if (argc > 1) shell_exec_app_command(argv[1]);
        else puts("exec: missing application name\n");
    } else if (strcmp(argv[0], "open") == 0) {
        if (argc < 2) {
            puts("open: missing application name\n");
        } else {
            uint32_t flags = apps_registry_app_flags(argv[1]);
            if (!(flags & MLJOS_APP_FLAG_GUI)) {
                puts("open: application has no GUI\n");
            } else if (!launcher_launch_gui(argv[1])) {
                puts("open: failed to launch application\n");
            }
        }
    } else if (strcmp(argv[0], "help") == 0) {
        puts("Commands: time, date, echo, shutdown, reboot, clear, help\n");
        print_storage_help();
    } else if (strcmp(argv[0], "usb") == 0) {
        if (argc == 1 || strcmp(argv[1], "controllers") == 0 || strcmp(argv[1], "list") == 0) {
            cmd_usb_list();
        } else if (strcmp(argv[1], "ports") == 0) {
            int controller_index = 0;

            if (argc < 3) puts("usb ports: missing controller index\n");
            else if (!parse_decimal_number(argv[2], &controller_index)) puts("usb ports: invalid controller index\n");
            else cmd_usb_ports(controller_index);
        } else if (strcmp(argv[1], "reset") == 0) {
            int controller_index = 0;
            int port_index = 0;

            if (argc < 4) puts("usb reset: missing controller index or port\n");
            else if (!parse_decimal_number(argv[2], &controller_index)) puts("usb reset: invalid controller index\n");
            else if (!parse_decimal_number(argv[3], &port_index)) puts("usb reset: invalid port\n");
            else cmd_usb_reset(controller_index, port_index);
        } else if (strcmp(argv[1], "probe") == 0) {
            int controller_index = 0;
            int port_index = 0;

            if (argc < 4) puts("usb probe: missing controller index or port\n");
            else if (!parse_decimal_number(argv[2], &controller_index)) puts("usb probe: invalid controller index\n");
            else if (!parse_decimal_number(argv[3], &port_index)) puts("usb probe: invalid port\n");
            else cmd_usb_probe(controller_index, port_index);
        } else if (strcmp(argv[1], "storage") == 0) {
            int controller_index = 0;
            int port_index = 0;

            if (argc < 4) puts("usb storage: missing controller index or port\n");
            else if (!parse_decimal_number(argv[2], &controller_index)) puts("usb storage: invalid controller index\n");
            else if (!parse_decimal_number(argv[3], &port_index)) puts("usb storage: invalid port\n");
            else cmd_usb_storage(controller_index, port_index);
        } else if (strcmp(argv[1], "read") == 0) {
            int controller_index = 0;
            int port_index = 0;
            int lba = 0;

            if (argc < 4) puts("usb read: missing controller index or port\n");
            else if (!parse_decimal_number(argv[2], &controller_index)) puts("usb read: invalid controller index\n");
            else if (!parse_decimal_number(argv[3], &port_index)) puts("usb read: invalid port\n");
            else if (argc > 4 && !parse_decimal_number(argv[4], &lba)) puts("usb read: invalid lba\n");
            else cmd_usb_read(controller_index, port_index, (uint32_t)lba);
        } else {
            puts("usb: unknown command\n");
            print_usb_help();
        }
    } else if (shell_try_launch_script_command(argv[0])) {
        // Script executed.
    } else if (!shell_try_launch_app_command(argv[0])) {
        COLOR = COLOR_ERROR;
        puts("Unknown command: ");
        puts(argv[0]);
        putchar('\n');
        COLOR = COLOR_DEFAULT;
    }

    COLOR = old_color;
}

void shell_run(void) {
    shell_boot();

    task_t *t = task_current();
    if (t && !t->api.puts) shell_init_task_api(t);
    if (t && !t->fs_cwd) fs_enter_home();

    clear_screen();
    puts("Welcome to mljOS by foxgalaxy23\n");
    puts("Accounts are loaded from disk when available. Fallback users: root/root and guest/guest\n");
    if (shell_disk_primary_mode()) puts("Installed mode detected: user shell is disk-backed and ram storage is hidden from normal navigation.\n");
    else puts("Live mode detected: ram storage stays primary and disk is optional.\n");
    print_storage_help();
    putchar('\n');

    if (users_try_autologin()) {
        reset_user_session();
        puts("Autologin enabled\n\n");
    } else {
        do_login();
    }

    if (__builtin_setjmp(shell_jmp_env_ptr())) {
        COLOR = COLOR_DEFAULT;
        puts("\n^C\n");
        users_end_sudo();
    }
    *shell_jmp_ready_ptr() = 1;
    shell_run_autorun_scripts();

    {
        char linebuf[128];
        while (1) {
            print_prompt();
            (void)read_line(linebuf, sizeof(linebuf));
            handle_command(linebuf);
        }
    }
}

#include "shell.h"
#include "console.h"
#include "disk.h"
#include "fs.h"
#include "io.h"
#include "kstring.h"
#include "rtc.h"

#define HISTORY_SIZE 16

static const char scancode_map_normal[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',
    0, '*', 0, ' ', 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static const char scancode_map_shift[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?',
    0, '*', 0, ' ', 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static char history[HISTORY_SIZE][128];
static int history_count = 0;
static int history_pos = -1;

static void cmd_time(void) {
    uint8_t h, m, s;
    get_rtc_time(&h, &m, &s);
    putchar('0' + (h / 10));
    putchar('0' + (h % 10));
    putchar(':');
    putchar('0' + (m / 10));
    putchar('0' + (m % 10));
    putchar(':');
    putchar('0' + (s / 10));
    putchar('0' + (s % 10));
    putchar('\n');
}

static void cmd_date(void) {
    uint8_t d, mo;
    uint16_t y;
    char ybuf[5];

    get_rtc_date(&d, &mo, &y);
    putchar('0' + (d / 10));
    putchar('0' + (d % 10));
    putchar('.');
    putchar('0' + (mo / 10));
    putchar('0' + (mo % 10));
    putchar('.');

    ybuf[4] = '\0';
    ybuf[3] = '0' + (y % 10); y /= 10;
    ybuf[2] = '0' + (y % 10); y /= 10;
    ybuf[1] = '0' + (y % 10); y /= 10;
    ybuf[0] = '0' + (y % 10);
    puts(ybuf);
    putchar('\n');
}

static void cmd_echo(const char *rest) {
    if (!rest) {
        putchar('\n');
        return;
    }
    puts(rest);
    putchar('\n');
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

    int idx = history_count % HISTORY_SIZE;
    int i;
    for (i = 0; i < 127 && line[i]; ++i) history[idx][i] = line[i];
    history[idx][i] = '\0';
    history_count++;
}

static const char *history_get(int pos_from_latest) {
    if (history_count == 0 || pos_from_latest < 0) return NULL;

    int avail = history_count < HISTORY_SIZE ? history_count : HISTORY_SIZE;
    if (pos_from_latest >= avail) return NULL;

    int idx = (history_count - 1 - pos_from_latest) % HISTORY_SIZE;
    if (idx < 0) idx += HISTORY_SIZE;
    return history[idx];
}

static void clear_input_visual(int prompt_row, int prompt_col) {
    for (int c = prompt_col; c < VGA_COLS; ++c) putchar_at(' ', prompt_row, c);
    cursor_row = prompt_row;
    cursor_col = prompt_col;
    update_cursor();
}

static int read_line(char *buf, int maxlen) {
    int len = 0;
    int prompt_row = cursor_row;
    int prompt_col = cursor_col;
    int shift_down = 0;
    int capslock = 0;
    uint8_t old_color = COLOR;

    history_pos = -1;
    COLOR = COLOR_DEFAULT;
    update_cursor();

    while (1) {
        while (!(inb(0x64) & 1)) {
            __asm__ volatile ("nop");
        }

        uint8_t sc = inb(0x60);

        if (sc == 0x2A || sc == 0x36) {
            shift_down = 1;
            continue;
        }
        if (sc == 0xAA || sc == 0xB6) {
            shift_down = 0;
            continue;
        }
        if (sc == 0x3A) {
            capslock = !capslock;
            continue;
        }

        if (sc == 0x48) {
            if (history_count == 0) continue;
            if (history_pos < 0) history_pos = 0;
            else if (history_pos < HISTORY_SIZE - 1) history_pos++;

            const char *h = history_get(history_pos);
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

        if (sc == 0x50) {
            if (history_count == 0) continue;
            if (history_pos <= 0) {
                history_pos = -1;
                clear_input_visual(prompt_row, prompt_col);
                len = 0;
                buf[0] = '\0';
            } else {
                history_pos--;
                const char *h = history_get(history_pos);
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
            }
            continue;
        }

        if (sc == 0x4B) {
            if (len > 0) {
                if (cursor_col == 0) {
                    if (cursor_row > 0) {
                        cursor_row--;
                        cursor_col = VGA_COLS - 1;
                    } else {
                        cursor_col = 0;
                    }
                } else {
                    cursor_col--;
                }
                update_cursor();
            }
            continue;
        }

        if (sc == 0x4D) {
            if (len < maxlen - 1) {
                if (cursor_col == VGA_COLS - 1) {
                    cursor_col = 0;
                    cursor_row++;
                    scroll_if_needed();
                } else {
                    cursor_col++;
                }
                update_cursor();
            }
            continue;
        }

        if (sc & 0x80) continue;

        char c = 0;
        if (sc < 128) {
            int is_letter = 0;
            if ((sc >= 0x10 && sc <= 0x19) || (sc >= 0x1E && sc <= 0x26) || (sc >= 0x2C && sc <= 0x32)) {
                is_letter = 1;
            }
            int use_shift_map = shift_down;
            if (capslock && is_letter) use_shift_map = !use_shift_map;
            c = use_shift_map ? scancode_map_shift[sc] : scancode_map_normal[sc];
        }
        if (c == 0) continue;

        if (c == '\n' || c == '\r') {
            putchar('\n');
            buf[len] = '\0';
            if (len > 0) push_history(buf);
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
                    } else {
                        cursor_col = 0;
                    }
                } else {
                    cursor_col--;
                }
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
            buf[len++] = c;
            putchar_at(c, cursor_row, cursor_col);
            cursor_col++;
            if (cursor_col >= VGA_COLS) {
                cursor_col = 0;
                cursor_row++;
                scroll_if_needed();
            }
            buf[len] = '\0';
            update_cursor();
        }
    }
}

static int split_args(char *line, char **argv, int maxargv) {
    int argc = 0;
    char *p = line;

    while (*p && argc < maxargv) {
        while (*p == ' ') p++;
        if (!*p) break;

        char *dst = p;
        argv[argc++] = dst;

        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) p++;
                *dst++ = *p++;
            }
            if (*p == '"') p++;
            *dst = '\0';
        } else {
            while (*p && *p != ' ') {
                if (*p == '\\' && p[1]) p++;
                *dst++ = *p++;
            }
            if (*p == ' ') {
                char *space = p;
                *dst = '\0';
                p = space + 1;
            } else {
                *dst = '\0';
            }
        }
        while (*p == ' ') p++;
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

static void print_prompt(void) {
    uint8_t old_color = COLOR;
    COLOR = COLOR_PROMPT;
    puts("root@mljOS:");
    if (current_dir && strcmp(current_dir->name, "/") != 0) puts(current_dir->name);
    else puts("/");
    puts("# ");
    COLOR = old_color;
}

static void handle_command(char *line) {
    uint8_t old_color = COLOR;
    COLOR = COLOR_DEFAULT;

    if (line[0] == '\0') {
        COLOR = old_color;
        return;
    }

    char line_copy[128];
    char *argv[10];
    char joined[128];
    strcpy(line_copy, line);

    int argc = split_args(line_copy, argv, 10);
    if (argc == 0) {
        COLOR = old_color;
        return;
    }

    if (strcmp(argv[0], "time") == 0) {
        cmd_time();
    } else if (strcmp(argv[0], "date") == 0) {
        cmd_date();
    } else if (strcmp(argv[0], "echo") == 0) {
        if (argc > 1) {
            join_args(joined, sizeof(joined), argv, 1, argc);
            cmd_echo(joined);
        }
        else cmd_echo("");
    } else if (strcmp(argv[0], "reboot") == 0) {
        cmd_reboot();
    } else if (strcmp(argv[0], "shutdown") == 0) {
        cmd_shutdown();
    } else if (strcmp(argv[0], "clear") == 0) {
        clear_screen();
    } else if (strcmp(argv[0], "ls") == 0) {
        cmd_ls();
    } else if (strcmp(argv[0], "cd") == 0) {
        if (argc > 1) cmd_cd(argv[1]);
        else puts("cd: missing operand\n");
    } else if (strcmp(argv[0], "mkdir") == 0) {
        if (argc > 1) cmd_mkdir(argv[1]);
        else puts("mkdir: missing operand\n");
    } else if (strcmp(argv[0], "touch") == 0) {
        if (argc > 1) cmd_touch(argv[1]);
        else puts("touch: missing operand\n");
    } else if (strcmp(argv[0], "rm") == 0) {
        if (argc > 1) cmd_rm(argv[1]);
        else puts("rm: missing operand\n");
    } else if (strcmp(argv[0], "cat") == 0) {
        if (argc > 1) cmd_cat(argv[1]);
        else puts("cat: missing operand\n");
    } else if (strcmp(argv[0], "cp") == 0) {
        if (argc > 2) cmd_cp(argv[1], argv[2]);
        else puts("cp: missing operands\n");
    } else if (strcmp(argv[0], "write") == 0) {
        if (argc > 2) {
            join_args(joined, sizeof(joined), argv, 2, argc);
            cmd_write(argv[1], joined);
        }
        else puts("write: missing file or text\n");
    } else if (strcmp(argv[0], "disk") == 0) {
        if (argc < 2) puts("disk: missing command (format, ls, cd, pwd, mkdir, write, cat, rm)\n");
        else if (strcmp(argv[1], "format") == 0) cmd_disk_format();
        else if (strcmp(argv[1], "ls") == 0) {
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
        }
        else if (strcmp(argv[1], "rm") == 0) {
            if (argc > 2) cmd_disk_rm(argv[2]);
            else puts("disk rm: missing path\n");
        } else if (strcmp(argv[1], "cat") == 0) {
            if (argc > 2) cmd_disk_cat(argv[2]);
            else puts("disk cat: missing path\n");
        } else if (strcmp(argv[1], "write") == 0) {
            if (argc > 3) {
                join_args(joined, sizeof(joined), argv, 3, argc);
                cmd_disk_write(argv[2], joined);
            }
            else puts("disk write: missing path or text\n");
        } else {
            puts("disk: unknown command\n");
        }
    } else if (strcmp(argv[0], "help") == 0) {
        puts("Commands: time, date, echo, shutdown, reboot, clear, help\n");
        puts("FS: ls, cd, mkdir, touch, rm, cat, cp, write\n");
        puts("Disk FAT32: disk format, disk ls [path], disk cd <path>, disk pwd\n");
        puts("Disk FAT32: disk mkdir <path>\n");
        puts("Disk FAT32: disk cat <path>, disk write <path> <text>, disk rm <path>\n");
        puts("Quotes: disk mkdir \"My Folder\", disk write \"My Folder/File.txt\" \"hello world\"\n");
    } else {
        COLOR = COLOR_ERROR;
        puts("Unknown command: ");
        puts(argv[0]);
        putchar('\n');
        COLOR = COLOR_DEFAULT;
    }

    COLOR = old_color;
}

void shell_run(void) {
    fs_init();
    clear_screen();
    puts("Welcome to mljOS by foxgalaxy23\n");
    puts("Commands: time, date, echo, shutdown, reboot, clear, help\n");
    puts("FS Commands: ls, cd, mkdir, touch, rm, cat, cp, write\n");
    puts("Disk FAT32: disk format, disk ls [path], disk cd <path>, disk pwd\n");
    puts("Disk FAT32: disk mkdir <path>\n");
    puts("Disk FAT32: disk cat <path>, disk write <path> <text>, disk rm <path>\n\n");

    char linebuf[128];
    while (1) {
        print_prompt();
        (void)read_line(linebuf, sizeof(linebuf));
        handle_command(linebuf);
    }
}

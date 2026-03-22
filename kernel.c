/* kernel.c - mljOS (integrated) */

/* базовые typedef'ы */
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef int            int32_t;

#ifndef NULL
#define NULL ((void*)0)
#endif

/* === ЦВЕТОВЫЕ КОНСТАНТЫ === */
#define COLOR_DEFAULT   0x0F /* Светло-серый на черном */
#define COLOR_PROMPT    0x0A /* Светло-зеленый на черном */
#define COLOR_ERROR     0x0C /* Светло-красный на черном */
#define COLOR_ALERT     0x0E /* Желтый на черном */
/* ========================== */

/* VGA text buffer */
volatile uint8_t *vga_buffer = (volatile uint8_t *)0xB8000;
const int VGA_COLS = 80;
const int VGA_ROWS = 25;
uint8_t COLOR = COLOR_DEFAULT; /* Инициализация основным цветом */

/* курсор */
int cursor_row = 0;
int cursor_col = 0;

/* портовые операции */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

/* простая memmove для kernel (используем для scroll) */
static void *kmemmove(void *dst, const void *src, unsigned int n) {
    unsigned char *d = (unsigned char*)dst;
    const unsigned char *s = (const unsigned char*)src;
    if (d == s) return dst;
    if (d < s) {
        for (unsigned int i = 0; i < n; ++i) d[i] = s[i];
    } else {
        for (unsigned int i = n; i-- > 0;) d[i] = s[i];
    }
    return dst;
}

/* обновление аппаратного курсора (VGA text) */
static inline void update_cursor() {
    uint16_t pos = (uint16_t)(cursor_row * VGA_COLS + cursor_col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

/* прокрутка — копируем память вверх и очищаем последнюю строку */
void scroll_if_needed() {
    if (cursor_row < VGA_ROWS) return;
    const int row_bytes = VGA_COLS * 2;
    /* copy rows 1..VGA_ROWS-1 to 0..VGA_ROWS-2 */
    kmemmove((void*)vga_buffer, (const void*)(vga_buffer + row_bytes), (VGA_ROWS - 1) * row_bytes);
    /* clear last row */
    int r = VGA_ROWS - 1;
    for (int c = 0; c < VGA_COLS; ++c) {
        vga_buffer[(r*VGA_COLS + c) * 2] = ' ';
        vga_buffer[(r*VGA_COLS + c) * 2 + 1] = COLOR_DEFAULT; /* Использовать COLOR_DEFAULT для очистки */
    }
    cursor_row = VGA_ROWS - 1;
    cursor_col = 0;
    update_cursor();
}

/* вывести символ на позицию */
void putchar_at(char ch, int row, int col) {
    if (row < 0 || row >= VGA_ROWS || col < 0 || col >= VGA_COLS) return;
    vga_buffer[(row*VGA_COLS + col) * 2] = (uint8_t)ch;
    vga_buffer[(row*VGA_COLS + col) * 2 + 1] = COLOR;
}

/* putchar с обработкой спецсимволов */
void putchar(char ch) {
    if (ch == '\n') {
        cursor_col = 0;
        cursor_row++;
        scroll_if_needed();
        update_cursor();
        return;
    } else if (ch == '\r') {
        cursor_col = 0;
        update_cursor();
        return;
    } else if (ch == '\t') {
        int t = 4 - (cursor_col % 4);
        while (t--) putchar(' ');
        return;
    }

    putchar_at(ch, cursor_row, cursor_col);
    cursor_col++;
    if (cursor_col >= VGA_COLS) {
        cursor_col = 0;
        cursor_row++;
        scroll_if_needed();
    }
    update_cursor();
}

/* puts */
void puts(const char *s) {
    for (int i = 0; s[i]; ++i) putchar(s[i]);
}

/* clear screen */
void clear_screen() {
    for (int i = 0; i < VGA_COLS * VGA_ROWS; ++i) {
        vga_buffer[i*2] = ' ';
        vga_buffer[i*2 + 1] = COLOR_DEFAULT; /* Использовать COLOR_DEFAULT для очистки */
    }
    cursor_row = 0;
    cursor_col = 0;
    COLOR = COLOR_DEFAULT; /* Устанавливаем основной цвет */
    update_cursor();
}

/* Scancode maps (set 1). Compiler будет дополнять нулями, если нужно. */
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

/* RTC helpers */
static inline uint8_t bcd_to_bin(uint8_t val) {
    return (val & 0x0F) + ((val >> 4) * 10);
}

uint8_t read_rtc_register(uint8_t reg) {
    outb(0x70, (uint8_t)(reg | 0x80));
    return inb(0x71);
}

void get_rtc_time(uint8_t *hh, uint8_t *mm, uint8_t *ss) {
    while (read_rtc_register(0x0A) & 0x80) { }
    uint8_t s = read_rtc_register(0x00);
    uint8_t m = read_rtc_register(0x02);
    uint8_t h = read_rtc_register(0x04);
    uint8_t statusB = read_rtc_register(0x0B);
    if (!(statusB & 0x04)) {
        s = bcd_to_bin(s);
        m = bcd_to_bin(m);
        h = bcd_to_bin(h);
    }
    *hh = h; *mm = m; *ss = s;
}

void get_rtc_date(uint8_t *day, uint8_t *month, uint16_t *year) {
    while (read_rtc_register(0x0A) & 0x80) { }
    uint8_t d = read_rtc_register(0x07);
    uint8_t mo = read_rtc_register(0x08);
    uint8_t y = read_rtc_register(0x09);
    uint8_t statusB = read_rtc_register(0x0B);
    if (!(statusB & 0x04)) {
        d = bcd_to_bin(d);
        mo = bcd_to_bin(mo);
        y = bcd_to_bin(y);
    }
    *day = d;
    *month = mo;
    *year = 2000 + (uint16_t)y;
}

/* ==================== СТРОКОВЫЕ ФУНКЦИИ ==================== */
int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}
char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}
unsigned int strlen(const char *s) {
    unsigned int len = 0;
    while (s[len]) len++;
    return len;
}
char *strncpy(char *dest, const char *src, unsigned int n) {
    unsigned int i;
    for (i = 0; i < n && src[i] != '\0'; i++) dest[i] = src[i];
    for ( ; i < n; i++) dest[i] = '\0';
    return dest;
}

/* ======================== RamFS ============================ */
#define FS_FILE 0
#define FS_DIR  1
#define MAX_FS_NODES 256
#define FS_POOL_SIZE (64 * 1024)

typedef struct fs_node {
    char name[32];
    uint8_t flags;
    uint32_t size;
    struct fs_node *parent;
    struct fs_node *child;
    struct fs_node *sibling;
    char *content;
} fs_node_t;

fs_node_t fs_nodes[MAX_FS_NODES];
int fs_node_count = 0;
char fs_data_pool[FS_POOL_SIZE];
int fs_data_offset = 0;
fs_node_t *fs_root = NULL;
fs_node_t *current_dir = NULL;

void fs_init() {
    fs_root = &fs_nodes[fs_node_count++];
    strcpy(fs_root->name, "/");
    fs_root->flags = FS_DIR;
    fs_root->parent = fs_root;
    current_dir = fs_root;
}

fs_node_t *fs_find_child(fs_node_t *dir, const char *name) {
    if (!dir || dir->flags != FS_DIR) return NULL;
    if (strcmp(name, ".") == 0) return dir;
    if (strcmp(name, "..") == 0) return dir->parent;
    
    fs_node_t *curr = dir->child;
    while (curr) {
        if (strcmp(curr->name, name) == 0) return curr;
        curr = curr->sibling;
    }
    return NULL;
}

fs_node_t *fs_create_node(fs_node_t *dir, const char *name, uint8_t flags) {
    if (fs_node_count >= MAX_FS_NODES) return NULL;
    if (fs_find_child(dir, name)) return NULL;
    
    fs_node_t *new_node = &fs_nodes[fs_node_count++];
    strncpy(new_node->name, name, 31);
    new_node->name[31] = '\0';
    new_node->flags = flags;
    new_node->parent = dir;
    new_node->child = NULL;
    new_node->sibling = dir->child;
    dir->child = new_node;
    new_node->size = 0;
    new_node->content = NULL;
    return new_node;
}

void fs_delete_node(fs_node_t *dir, const char *name) {
    if (!dir || dir->flags != FS_DIR) return;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return;
    
    fs_node_t *prev = NULL;
    fs_node_t *curr = dir->child;
    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            if (prev) prev->sibling = curr->sibling;
            else dir->child = curr->sibling;
            return;
        }
        prev = curr;
        curr = curr->sibling;
    }
}

void cmd_ls() {
    if (!current_dir) return;
    fs_node_t *curr = current_dir->child;
    while (curr) {
        uint8_t old_color = COLOR;
        if (curr->flags == FS_DIR) COLOR = 0x09;
        else COLOR = COLOR_DEFAULT;
        
        puts(curr->name);
        puts("  ");
        COLOR = old_color;
        curr = curr->sibling;
    }
    putchar('\n');
}

void cmd_cd(const char *path) {
    if (!path || !path[0]) return;
    if (strcmp(path, "/") == 0) { current_dir = fs_root; return; }
    fs_node_t *target = fs_find_child(current_dir, path);
    if (!target) puts("cd: no such file or directory\n");
    else if (target->flags != FS_DIR) puts("cd: not a directory\n");
    else current_dir = target;
}

void cmd_mkdir(const char *name) {
    if (!fs_create_node(current_dir, name, FS_DIR)) puts("mkdir: cannot create directory\n");
}

void cmd_touch(const char *name) {
    if (!fs_create_node(current_dir, name, FS_FILE)) puts("touch: cannot create file\n");
}

void cmd_rm(const char *name) {
    fs_delete_node(current_dir, name);
}

void cmd_cat(const char *name) {
    fs_node_t *file = fs_find_child(current_dir, name);
    if (!file) puts("cat: no such file\n");
    else if (file->flags != FS_FILE) puts("cat: is a directory\n");
    else {
        if (file->size > 0 && file->content) {
            for (uint32_t i = 0; i < file->size; i++) putchar(file->content[i]);
        }
        putchar('\n');
    }
}

void cmd_write(const char *name, const char *text) {
    fs_node_t *file = fs_find_child(current_dir, name);
    if (!file) {
        file = fs_create_node(current_dir, name, FS_FILE);
        if (!file) { puts("write: cannot create file\n"); return; }
    }
    if (file->flags != FS_FILE) { puts("write: is a directory\n"); return; }
    
    unsigned int len = strlen(text);
    if (fs_data_offset + len > FS_POOL_SIZE) { puts("write: out of space\n"); return; }
    
    file->content = &fs_data_pool[fs_data_offset];
    for (unsigned int i = 0; i < len; i++) file->content[i] = text[i];
    file->size = len;
    fs_data_offset += len;
}

void cmd_cp(const char *src_name, const char *dst_name) {
    fs_node_t *src = fs_find_child(current_dir, src_name);
    if (!src) { puts("cp: no such file\n"); return; }
    if (src->flags == FS_DIR) { puts("cp: omitting directory\n"); return; }
    
    fs_node_t *dst = fs_create_node(current_dir, dst_name, FS_FILE);
    if (!dst) { puts("cp: cannot create destination\n"); return; }
    
    if (src->size > 0 && src->content) {
        if (fs_data_offset + src->size > FS_POOL_SIZE) { puts("cp: out of space\n"); return; }
        dst->content = &fs_data_pool[fs_data_offset];
        for (uint32_t i = 0; i < src->size; i++) dst->content[i] = src->content[i];
        dst->size = src->size;
        fs_data_offset += src->size;
    }
}

/* Промпт */
void print_prompt() {
    uint8_t old_color = COLOR;
    COLOR = COLOR_PROMPT; /* Устанавливаем цвет промпта */
    puts("root@mljOS:");
    if (current_dir) {
        if (strcmp(current_dir->name, "/") == 0) puts("/");
        else puts(current_dir->name);
    } else {
        puts("/");
    }
    puts("# ");
    COLOR = old_color;
}

/* Команды */
void cmd_time() {
    uint8_t h,m,s;
    get_rtc_time(&h,&m,&s);
    char t1 = '0' + (h / 10);
    char t2 = '0' + (h % 10);
    char t3 = '0' + (m / 10);
    char t4 = '0' + (m % 10);
    char t5 = '0' + (s / 10);
    char t6 = '0' + (s % 10);
    putchar(t1); putchar(t2); putchar(':');
    putchar(t3); putchar(t4); putchar(':');
    putchar(t5); putchar(t6);
    putchar('\n');
}

void cmd_date() {
    uint8_t d, mo;
    uint16_t y;
    get_rtc_date(&d, &mo, &y);
    char a = '0' + (d / 10);
    char b = '0' + (d % 10);
    char c = '0' + (mo / 10);
    char e = '0' + (mo % 10);
    uint16_t g = y;
    char ybuf[5];
    ybuf[4] = '\0';
    ybuf[3] = '0' + (g % 10); g /= 10;
    ybuf[2] = '0' + (g % 10); g /= 10;
    ybuf[1] = '0' + (g % 10); g /= 10;
    ybuf[0] = '0' + (g % 10);
    putchar(a); putchar(b); putchar('.');
    putchar(c); putchar(e); putchar('.');
    puts(ybuf);
    putchar('\n');
}

void cmd_echo(const char *rest) {
    if (!rest) { putchar('\n'); return; }
    puts(rest);
    putchar('\n');
}

void cmd_reboot() {
    uint8_t old_color = COLOR;
    COLOR = COLOR_ALERT; /* Устанавливаем цвет предупреждения */
    puts("Rebooting...\n");
    COLOR = old_color;
    outb(0x64, 0xFE);
    for (;;) { __asm__ volatile ("hlt"); }
}

void cmd_shutdown() {
    uint8_t old_color = COLOR;
    COLOR = COLOR_ALERT; /* Устанавливаем цвет предупреждения */
    puts("Shutdown...\n");
    COLOR = old_color;
    outw(0x604, 0x2000);
    outb(0x64, 0xFE);
    for (;;) { __asm__ volatile ("hlt"); }
}



/* История и ввод: реализация read_line с поддержкой shift/caps/history */
#define HISTORY_SIZE 16
static char history[HISTORY_SIZE][128];
static int history_count = 0;
static int history_pos = -1;

static void push_history(const char *line) {
    if (!line || !line[0]) return;
    int idx = history_count % HISTORY_SIZE;
    /* strncpy-like */
    int i;
    for (i = 0; i < 127 && line[i]; ++i) history[idx][i] = line[i];
    history[idx][i] = '\0';
    history_count++;
}

static const char* history_get(int pos_from_latest) {
    if (history_count == 0) return NULL;
    if (pos_from_latest < 0) return NULL;
    int avail = history_count < HISTORY_SIZE ? history_count : HISTORY_SIZE;
    if (pos_from_latest >= avail) return NULL;
    int idx = (history_count - 1 - pos_from_latest) % HISTORY_SIZE;
    if (idx < 0) idx += HISTORY_SIZE;
    return history[idx];
}

/* Визуальная очистка ввода от prompt_col до конца строки (простая версия) */
static void clear_input_visual(int prompt_row, int prompt_col) {
    for (int c = prompt_col; c < VGA_COLS; ++c) {
        putchar_at(' ', prompt_row, c);
    }
    cursor_row = prompt_row;
    cursor_col = prompt_col;
    update_cursor();
}

/* read_line: улучшенная, блокирующая, с поллингом PS/2, поддержкой истории */
int read_line(char *buf, int maxlen) {
    int len = 0;
    int prompt_row = cursor_row;
    int prompt_col = cursor_col;
    history_pos = -1;
    
    uint8_t old_color = COLOR; /* Сохраняем цвет промпта */
    COLOR = COLOR_DEFAULT;     /* Устанавливаем цвет ввода */

    update_cursor();

    int shift_down = 0;
    int capslock = 0;

    while (1) {
        while (!(inb(0x64) & 1)) { __asm__ volatile ("nop"); }
        uint8_t sc = inb(0x60);

        /* Shift press/release */
        if (sc == 0x2A || sc == 0x36) { shift_down = 1; continue; }
        if (sc == 0xAA || sc == 0xB6) { shift_down = 0; continue; }
        /* CapsLock */
        if (sc == 0x3A) { capslock = !capslock; continue; }

        /* Up/Down/Left/Right (basic) */
        if (sc == 0x48) { /* Up */
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
                    if (cursor_col >= VGA_COLS) { cursor_col = 0; cursor_row++; scroll_if_needed(); }
                    i++;
                }
                len = i; buf[len] = '\0';
                update_cursor();
            }
            continue;
        } else if (sc == 0x50) { /* Down */
            if (history_count == 0) continue;
            if (history_pos <= 0) {
                history_pos = -1;
                clear_input_visual(prompt_row, prompt_col);
                len = 0; buf[0] = '\0';
            } else {
                history_pos--;
                const char *h = history_get(history_pos);
                if (h) {
                    clear_input_visual(prompt_row, prompt_col);
                    int i = 0;
                    while (h[i] && i < maxlen - 1) {
                        buf[i] = h[i];
                        putchar_at(h[i], cursor_row, cursor_col++);
                        if (cursor_col >= VGA_COLS) { cursor_col = 0; cursor_row++; scroll_if_needed(); }
                        i++;
                    }
                    len = i; buf[len] = '\0';
                    update_cursor();
                }
            }
            continue;
        } else if (sc == 0x4B) { /* Left */
            if (len > 0) {
                if (cursor_col == 0) {
                    if (cursor_row > 0) { cursor_row--; cursor_col = VGA_COLS - 1; }
                    else cursor_col = 0;
                } else cursor_col--;
                update_cursor();
            }
            continue;
        } else if (sc == 0x4D) { /* Right */
            if (len < maxlen - 1) {
                if (cursor_col == VGA_COLS - 1) { cursor_col = 0; cursor_row++; scroll_if_needed(); }
                else cursor_col++;
                update_cursor();
            }
            continue;
        }

        /* ignore releases */
        if (sc & 0x80) continue;

        /* map scancode -> char */
        char c = 0;
        if (sc < 128) {
            int is_letter = 0;
            if ((sc >= 0x10 && sc <= 0x19) || (sc >= 0x1E && sc <= 0x26) || (sc >= 0x2C && sc <= 0x32)) is_letter = 1;
            int use_shift_map = shift_down;
            if (capslock && is_letter) use_shift_map = !use_shift_map;
            c = use_shift_map ? scancode_map_shift[sc] : scancode_map_normal[sc];
        }
        if (c == 0) continue;

        /* Enter */
        if (c == '\n' || c == '\r') {
            putchar('\n');
            buf[len] = '\0';
            if (len > 0) push_history(buf);
            COLOR = old_color; /* Восстанавливаем цвет для следующего промпта */
            update_cursor();
            return len;
        }

        /* Backspace */
        if (c == '\b') {
            if (len > 0) {
                if (cursor_col == 0) {
                    if (cursor_row > 0) { cursor_row--; cursor_col = VGA_COLS - 1; }
                    else cursor_col = 0;
                } else cursor_col--;
                putchar_at(' ', cursor_row, cursor_col);
                len--; buf[len] = '\0';
                update_cursor();
            }
            continue;
        }

        /* Tab: 4 spaces */
        if (c == '\t') {
            int t = 4 - (len % 4);
            while (t-- && len < maxlen - 1) {
                buf[len++] = ' ';
                putchar_at(' ', cursor_row, cursor_col);
                cursor_col++;
                if (cursor_col >= VGA_COLS) { cursor_col = 0; cursor_row++; scroll_if_needed(); }
            }
            buf[len] = '\0';
            update_cursor();
            continue;
        }

        /* обычный символ */
        if (len < maxlen - 1) {
            buf[len++] = c;
            putchar_at(c, cursor_row, cursor_col);
            cursor_col++;
            if (cursor_col >= VGA_COLS) { cursor_col = 0; cursor_row++; scroll_if_needed(); }
            buf[len] = '\0';
            update_cursor();
        }
    }
}

/* простая парсер-функция: разделяем строку по пробелам */
int split_args(char *line, char **argv, int maxargv) {
    int argc = 0;
    char *p = line;
    while (*p && argc < maxargv) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) { *p = '\0'; p++; }
    }
    return argc;
}

/* handle command */
void handle_command(char *line) {
    uint8_t old_color = COLOR;
    COLOR = COLOR_DEFAULT; /* Команды должны выводить результат обычным цветом */
    
    if (line[0] == '\0') {
        COLOR = old_color;
        return;
    }
    
    char line_copy[128];
    strcpy(line_copy, line);
    
    char *argv[10];
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
            int offset = (argv[1] - line_copy);
            cmd_echo(line + offset);
        } else {
            cmd_echo("");
        }
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
            int offset = (argv[2] - line_copy);
            cmd_write(argv[1], line + offset);
        } else puts("write: missing file or text\n");
    } else if (strcmp(argv[0], "help") == 0) {
        puts("Commands: time, date, echo, shutdown, reboot, clear, help\n");
        puts("FS: ls, cd, mkdir, touch, rm, cat, cp, write\n");
    } else {
        COLOR = COLOR_ERROR; /* Цвет ошибки */
        puts("Unknown command: ");
        puts(argv[0]);
        putchar('\n');
        COLOR = COLOR_DEFAULT; /* Возвращаем стандартный цвет */
    }
    
    COLOR = old_color;
}

/* main */
void kernel_main() {
    fs_init();
    clear_screen();
    puts("Welcome to mljOS by foxgalaxy23\n");
    puts("Commands: time, date, echo, shutdown, reboot, clear, help\n");
    puts("FS Commands: ls, cd, mkdir, touch, rm, cat, cp, write\n\n");

    char linebuf[128];
    while (1) {
        print_prompt();
        int len = read_line(linebuf, sizeof(linebuf));
        (void)len;
        handle_command(linebuf);
    }
}

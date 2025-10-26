typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef int            int32_t;

volatile uint8_t *vga_buffer = (volatile uint8_t *)0xB8000;
const int VGA_COLS = 80;
const int VGA_ROWS = 25;
const uint8_t COLOR = 0x0F;

static int cursor_row = 0;
static int cursor_col = 0;

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

void scroll_if_needed() {
    if (cursor_row >= VGA_ROWS) {
        for (int r = 1; r < VGA_ROWS; ++r) {
            for (int c = 0; c < VGA_COLS; ++c) {
                vga_buffer[((r-1)*VGA_COLS + c) * 2] = vga_buffer[(r*VGA_COLS + c) * 2];
                vga_buffer[((r-1)*VGA_COLS + c) * 2 + 1] = vga_buffer[(r*VGA_COLS + c) * 2 + 1];
            }
        }
        int r = VGA_ROWS - 1;
        for (int c = 0; c < VGA_COLS; ++c) {
            vga_buffer[(r*VGA_COLS + c) * 2] = ' ';
            vga_buffer[(r*VGA_COLS + c) * 2 + 1] = COLOR;
        }
        cursor_row = VGA_ROWS - 1;
    }
}

void putchar_at(char ch, int row, int col) {
    if (row < 0 || row >= VGA_ROWS || col < 0 || col >= VGA_COLS) return;
    vga_buffer[(row*VGA_COLS + col) * 2] = ch;
    vga_buffer[(row*VGA_COLS + col) * 2 + 1] = COLOR;
}

void putchar(char ch) {
    if (ch == '\n') {
        cursor_col = 0;
        cursor_row++;
        scroll_if_needed();
        return;
    } else if (ch == '\r') {
        cursor_col = 0;
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
}

void puts(const char *s) {
    for (int i = 0; s[i]; ++i) putchar(s[i]);
}

void clear_screen() {
    for (int i = 0; i < VGA_COLS * VGA_ROWS; ++i) {
        vga_buffer[i*2] = ' ';
        vga_buffer[i*2 + 1] = COLOR;
    }
    cursor_row = 0;
    cursor_col = 0;
}
static const char scancode_map[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b', /* Backspace */
    '\t', /* Tab */
    'q','w','e','r','t','y','u','i','o','p','[',']','\n', /* Enter */
    0, /* Ctrl */
    'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, /* Left shift */
    '\\','z','x','c','v','b','n','m',',','.','/',
    0, /* Right shift */
    '*',
    0, /* Alt */
    ' ', /* Space bar */
    0, /* CapsLock */
    0,0,0,0,0,0,0,0,0,0, /* F1-F10 */
    0, /* Num lock */
    0, /* Scroll Lock */
    0, /* Home key */
    0, /* Up Arrow */
    0, /* Page Up */
    '-',
    0, /* Left Arrow */
    0,
    0, /* Right Arrow */
    '+',
    0, /* End key */
    0, /* Down Arrow */
    0, /* Page Down */
    0, /* Insert Key */
    0, /* Delete Key */
    0,0,0, /* F11-F12 and others */
};

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

void print_prompt() {
    puts("System : ");
}

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
    puts("Rebooting...\n");
    outb(0x64, 0xFE);
    for (;;) { __asm__ volatile ("hlt"); }
}

void cmd_shutdown() {
    puts("Shutdown...\n");
    outw(0x604, 0x2000);
    outb(0x64, 0xFE);
    for (;;) { __asm__ volatile ("hlt"); }
}

int starts_with(const char *s, const char *prefix) {
    int i = 0;
    while (prefix[i]) {
        if (s[i] == '\0' || s[i] != prefix[i]) return 0;
        i++;
    }
    return 1;
}

int read_line(char *buf, int maxlen) {
    int len = 0;
    while (1) {
        while (!(inb(0x64) & 1)) {
            __asm__ volatile ("nop");
        }
        uint8_t sc = inb(0x60);

        if (sc & 0x80) continue;

        char c = 0;
        if (sc < 128) c = scancode_map[sc];
        if (c == 0) continue;

        if (c == '\n' || c == '\r') {
            putchar('\n');
            buf[len] = '\0';
            return len;
        } else if (c == '\b') {
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
            }
        } else {
            if (len < maxlen - 1) {
                buf[len++] = c;
                putchar(c);
            }
        }
    }
}

void handle_command(char *line) {
    if (line[0] == '\0') return;
    if (starts_with(line, "time")) {
        cmd_time();
    } else if (starts_with(line, "date")) {
        cmd_date();
    } else if (starts_with(line, "echo ")) {
        cmd_echo(line + 5);
    } else if (starts_with(line, "echo")) {
        cmd_echo("");
    } else if (starts_with(line, "reboot")) {
        cmd_reboot();
    } else if (starts_with(line, "shutdown")) {
        cmd_shutdown();
    } else if (starts_with(line, "clear")) {
        clear_screen();
    } else {
        puts("Unknown command: ");
        puts(line);
        putchar('\n');
    }
}

void kernel_main() {
    clear_screen();
    puts("Welcome to mljOS by foxgalaxy23\n");
    puts("Commands: time, date, echo, shutdown, reboot, clear\n\n");

    char linebuf[128];
    while (1) {
        print_prompt();
        int len = read_line(linebuf, sizeof(linebuf));
        (void)len;
        handle_command(linebuf);
    }
}

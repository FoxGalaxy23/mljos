#ifndef MLJOS_API_H
#define MLJOS_API_H

typedef struct {
    void (*puts)(const char *);
    void (*putchar)(char);
    void (*clear_screen)(void);
    int (*read_line)(char *buf, int maxlen);
    int (*read_file)(const char *path, char *buf, int maxlen, unsigned int *size_out);
    int (*write_file)(const char *path, const char *buf, unsigned int size);
    void (*set_cursor)(int row, int col);
    void (*putchar_at)(char ch, int row, int col);
    int (*read_key)(void);
    const char *open_path; // Optional: path to open on app startup (edit, etc.)
} mljos_api_t;

#endif

#ifndef MLJOS_API_H
#define MLJOS_API_H

#include "common.h"

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
    int (*list_dir)(const char *path, char *out, int out_size);
    int (*get_cwd)(char *out, int out_size);
    int (*mkdir)(const char *path);
    int (*rm)(const char *path);
    void (*get_time)(uint8_t *h, uint8_t *m, uint8_t *s);
    void (*get_date)(uint8_t *d, uint8_t *mo, uint16_t *y);
    const char *open_path; // Optional: path to open on app startup (edit, etc.)
} mljos_api_t;

#endif

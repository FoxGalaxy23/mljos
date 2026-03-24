#ifndef MLJOS_API_H
#define MLJOS_API_H

typedef struct {
    void (*puts)(const char *);
    void (*putchar)(char);
    void (*clear_screen)(void);
    int (*read_line)(char *buf, int maxlen);
} mljos_api_t;

#endif

#ifndef MLJOS_API_H
#define MLJOS_API_H

#include "common.h"

// Launch flags
#define MLJOS_LAUNCH_GUI  (1u << 0)

typedef enum {
    MLJOS_UI_EVENT_NONE = 0,
    MLJOS_UI_EVENT_KEY_DOWN = 1,
    MLJOS_UI_EVENT_MOUSE_LEFT_DOWN = 2,
    MLJOS_UI_EVENT_MOUSE_LEFT_UP = 3,
    MLJOS_UI_EVENT_MOUSE_MOVE = 4,
    // Mouse wheel: use `key` as signed delta (positive = wheel up).
    MLJOS_UI_EVENT_MOUSE_WHEEL = 5,
    // Request app to repaint (e.g. after window move/show/restore)
    MLJOS_UI_EVENT_EXPOSE = 6,
} mljos_ui_event_type_t;

typedef struct {
    uint32_t type;     // mljos_ui_event_type_t
    int32_t x;         // screen pixels
    int32_t y;         // screen pixels
    int32_t key;       // ASCII for KEY_DOWN when available
} mljos_ui_event_t;

typedef struct {
    // Drawing (absolute screen pixels)
    uint32_t (*screen_w)(void);
    uint32_t (*screen_h)(void);
    void (*fill_rect)(int x, int y, int w, int h, uint32_t rgb);
    void (*draw_text)(const char *s, int x, int y, uint32_t rgb);
    // Scaled bitmap text (scale >= 1). Optional; may be NULL on older runtimes.
    void (*draw_text_scale)(const char *s, int x, int y, uint32_t rgb, int scale);

    // App lifecycle (modal/fullscreen)
    void (*begin_app)(const char *title);
    void (*end_app)(void);

    // Input (non-blocking; returns 1 if event written)
    int (*poll_event)(mljos_ui_event_t *out_event);
    
    // System Dialogs (blocking kernel overlay)
    int (*prompt_input)(const char *title, const char *prompt, char *out_buf, int max_len);
} mljos_ui_api_t;

typedef struct {
    void (*puts)(const char *);
    void (*putchar)(char);
    void (*clear_screen)(void);
    int (*read_line)(char *buf, int maxlen);
    int (*read_file)(const char *path, char *buf, int maxlen, unsigned int *size_out);
    int (*write_file)(const char *path, const char *buf, unsigned int size);
    void (*set_cursor)(int row, int col);
    void (*putchar_at)(char ch, int row, int col);
    // Current TUI size in character cells (per-task console).
    int (*tui_cols)(void);
    int (*tui_rows)(void);
    int (*read_key)(void);
    int (*list_dir)(const char *path, char *out, int out_size);
    int (*get_cwd)(char *out, int out_size);
    int (*mkdir)(const char *path);
    int (*rm)(const char *path);
    void (*get_time)(uint8_t *h, uint8_t *m, uint8_t *s);
    void (*get_date)(uint8_t *d, uint8_t *mo, uint16_t *y);
    const char *open_path; // Optional: path to open on app startup (edit, etc.)
    // Optional: run the built-in shell within the current task/window.
    void (*run_shell)(void);
    // Launch an executable application asynchronously by name or path
    int (*launch_app)(const char *name_or_path);
    // Launch an executable application asynchronously and set its open_path
    int (*launch_app_args)(const char *name_or_path, const char *open_path);

    // GUI mode / graphics (optional)
    uint32_t launch_flags;   // MLJOS_LAUNCH_*
    mljos_ui_api_t *ui;      // NULL if UI is unavailable
} mljos_api_t;

#endif

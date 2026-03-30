#ifndef WM_H
#define WM_H

#include "common.h"
#include "sdk/mljos_api.h"

typedef struct wm_window wm_window_t;
struct task;

void wm_init(void);
void wm_pump_input(void);
void wm_compose_if_dirty(void);
void wm_mark_dirty(void);

// Window lifecycle
wm_window_t *wm_window_create(const char *title, int client_w_px, int client_h_px);
void wm_window_destroy(wm_window_t *w);
void wm_window_set_title(wm_window_t *w, const char *title);
void wm_window_focus(wm_window_t *w);
void wm_window_set_owner(wm_window_t *w, struct task *owner);

// Reap windows whose owners already exited after a close request.
void wm_reap_closed_windows(void);

// Terminal window tracking (optional helper for singleton terminal).
wm_window_t *wm_terminal_window(void);
void wm_set_terminal_window(wm_window_t *w);

// Window query
int wm_window_client_w(const wm_window_t *w);
int wm_window_client_h(const wm_window_t *w);
uint32_t *wm_window_client_pixels(const wm_window_t *w);
uint32_t wm_window_client_pitch_bytes(const wm_window_t *w);

// Event queue (per-window)
int wm_window_poll_event(wm_window_t *w, mljos_ui_event_t *out);
void wm_window_post_expose(wm_window_t *w);

// Launcher request from Start menu
int wm_consume_launch_request(char *out_name, int out_size);

#endif

#ifndef GUI_H
#define GUI_H

// Minimal Windows 95-style UI (taskbar + single terminal window).
// Управление делается из shell через горячие клавиши.

#include "common.h"

void gui_init(void);
void gui_tick(void);

void gui_toggle_dos_mode(void);
void gui_toggle_minimize_terminal(void);
void gui_restore_terminal(void);
void gui_move_terminal(int dx_chars, int dy_chars);

// Внутренний протокол мыши: shell читает сырые байты PS/2 и вызывает эту функцию.
void gui_mouse_push_byte(uint8_t byte);

// UI API hooks (used by apps via mljos_ui_api_t implementation).
uint32_t gui_ui_screen_w(void);
uint32_t gui_ui_screen_h(void);
void gui_ui_fill_rect(int x, int y, int w, int h, uint32_t rgb);
void gui_ui_draw_text(const char *s, int x, int y, uint32_t rgb);
void gui_ui_begin_app(const char *title);
void gui_ui_end_app(void);

// Mouse helpers for UI event generation.
int gui_ui_mouse_x(void);
int gui_ui_mouse_y(void);
int gui_ui_consume_left_pressed(void);
int gui_ui_consume_left_released(void);
int gui_ui_consume_expose(void);

// Force full redraw (recovery hotkey).
void gui_force_redraw(void);

#endif


#include "clipboard.h"

static char g_clipboard_text[4096];
static int g_clipboard_len = 0;

int clipboard_set_text(const char *text) {
    int i = 0;
    if (!text) {
        g_clipboard_text[0] = '\0';
        g_clipboard_len = 0;
        return 0;
    }
    while (text[i] && i < (int)sizeof(g_clipboard_text) - 1) {
        g_clipboard_text[i] = text[i];
        i++;
    }
    g_clipboard_text[i] = '\0';
    g_clipboard_len = i;
    return i;
}

int clipboard_get_text(char *out, int maxlen) {
    int i = 0;
    if (!out || maxlen <= 0) return 0;
    while (g_clipboard_text[i] && i < maxlen - 1) {
        out[i] = g_clipboard_text[i];
        i++;
    }
    out[i] = '\0';
    return i;
}

int clipboard_has_text(void) {
    return g_clipboard_len > 0;
}

int clipboard_text_len(void) {
    return g_clipboard_len;
}

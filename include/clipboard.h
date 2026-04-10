#ifndef CLIPBOARD_H
#define CLIPBOARD_H

int clipboard_set_text(const char *text);
int clipboard_get_text(char *out, int maxlen);
int clipboard_has_text(void);
int clipboard_text_len(void);

#endif

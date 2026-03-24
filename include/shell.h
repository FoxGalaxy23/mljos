#ifndef SHELL_H
#define SHELL_H

#include "common.h"
#include "sdk/mljos_api.h"

extern mljos_api_t os_api;
void shell_run(void);
int read_line(char *buf, int maxlen);
int read_secret_line(char *buf, int maxlen);

#endif

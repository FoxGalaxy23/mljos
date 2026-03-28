#ifndef SHELL_H
#define SHELL_H

#include "common.h"
#include "sdk/mljos_api.h"

extern mljos_api_t os_api;
void shell_boot(void);
void shell_run(void);
struct task;
void shell_init_task_api(struct task *t);
// Launch bundled app by command name (e.g. "calc").
// Exposed for GUI start menu.
void shell_exec_app_command(const char *name);
void shell_set_launch_flags(uint32_t flags);
int read_line(char *buf, int maxlen);
int read_secret_line(char *buf, int maxlen);

#endif

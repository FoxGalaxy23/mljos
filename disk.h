#ifndef DISK_H
#define DISK_H

#include "common.h"

void cmd_disk_format(void);
void cmd_disk_ls(const char *path);
void cmd_disk_cd(const char *path);
void cmd_disk_pwd(void);
void cmd_disk_mkdir(const char *path);
void cmd_disk_write(const char *path, const char *text);
void cmd_disk_cat(const char *path);
void cmd_disk_rm(const char *path);

#endif

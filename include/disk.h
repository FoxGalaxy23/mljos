#ifndef DISK_H
#define DISK_H

#include "common.h"

void cmd_disk_format(void);
void cmd_disk_devices(void);
int disk_select_device(int index);
int disk_get_active_device(void);
int disk_get_device_count(void);
int disk_get_system_device(void);
void disk_set_system_device(int index);
void disk_probe_devices_reset(void);
void cmd_disk_ls(const char *path);
void cmd_disk_cd(const char *path);
void cmd_disk_pwd(void);
void cmd_disk_mkdir(const char *path);
void cmd_disk_write(const char *path, const char *text);
void cmd_disk_cat(const char *path);
void cmd_disk_rm(const char *path);
const char *disk_get_cwd_path(void);
void disk_prepare_session(void);
void cmd_disk_install(void);
void cmd_disk_exec(const char *path);
int disk_can_exec_path(const char *path);
int disk_list_dir_file_names(const char *path, char *out, int out_size);
int disk_load_user_config(char *out, int maxlen);
int disk_save_user_config(const char *text);
int disk_ensure_directory(const char *path);
int disk_write_file(const char *path, const char *data, uint32_t size);
int disk_read_file(const char *path, char *out, int maxlen, uint32_t *size_out);
// Reads up to `maxlen` bytes from a file (binary-safe). Unlike disk_read_file,
// does not require the file to fit in the buffer.
int disk_read_file_prefix(const char *path, char *out, int maxlen, uint32_t *bytes_read_out);
int disk_touch_file(const char *path);
int disk_copy_file(const char *src_path, const char *dst_path);

#endif

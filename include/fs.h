#ifndef FS_H
#define FS_H

#include "common.h"

#define FS_FILE 0
#define FS_DIR  1
#define FS_APP_DIR "/apps"

#define FS_PERM_READ  4
#define FS_PERM_WRITE 2
#define FS_PERM_EXEC  1

typedef struct fs_node {
    char name[32];
    uint8_t flags;
    uint16_t owner_uid;
    uint16_t group_gid;
    uint16_t mode;
    uint32_t size;
    struct fs_node *parent;
    struct fs_node *child;
    struct fs_node *sibling;
    char *content;
} fs_node_t;

extern fs_node_t *fs_root;
extern fs_node_t *current_dir;

void fs_init(void);
void fs_enter_home(void);
void fs_print_prompt_path(void);
void fs_get_cwd_path(char *out, int out_size);
void fs_ensure_dir(const char *path, uint16_t uid, uint16_t gid, uint16_t mode);
int fs_sync_to_disk(void);
uint16_t fs_get_umask(void);
void fs_set_umask(uint16_t mask);
void cmd_ls(void);
void cmd_ls_path(const char *path);
void cmd_cd(const char *path);
void cmd_pwd(void);
void cmd_mkdir(const char *path);
void cmd_mkdir_p(const char *path);
void cmd_rmdir(const char *path);
void cmd_touch(const char *path);
void cmd_rm(const char *path);
void cmd_cat(const char *path);
void cmd_write(const char *path, const char *text);
void cmd_cp(const char *src_path, const char *dst_path);
void cmd_chmod(const char *mode_text, const char *path);
void cmd_chown(const char *owner_name, const char *path);
void cmd_ram_exec(const char *path);
int fs_resolve_app_command(const char *name, char *out, int out_size);
int fs_can_exec_path(const char *path);
int fs_list_dir_file_names(const char *path, char *out, int out_size);
int fs_read_file(const char *path, char *out, int maxlen, uint32_t *size_out);
int fs_write_file(const char *path, const char *data, uint32_t size);

#endif

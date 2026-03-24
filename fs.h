#ifndef FS_H
#define FS_H

#include "common.h"

#define FS_FILE 0
#define FS_DIR  1

typedef struct fs_node {
    char name[32];
    uint8_t flags;
    uint32_t size;
    struct fs_node *parent;
    struct fs_node *child;
    struct fs_node *sibling;
    char *content;
} fs_node_t;

extern fs_node_t *fs_root;
extern fs_node_t *current_dir;

void fs_init(void);
void cmd_ls(void);
void cmd_ls_path(const char *path);
void cmd_cd(const char *path);
void cmd_pwd(void);
void cmd_mkdir(const char *name);
void cmd_touch(const char *name);
void cmd_rm(const char *name);
void cmd_cat(const char *name);
void cmd_write(const char *name, const char *text);
void cmd_cp(const char *src_name, const char *dst_name);
void cmd_ram_exec(const char *name);

#endif

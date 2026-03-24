#include "fs.h"
#include "console.h"
#include "kstring.h"
#include "shell.h"
#include "apps/calc_app.h"

typedef void (*app_entry_t)(mljos_api_t*);

static int fs_is_elf_image(const char *data, uint32_t size) {
    return size >= 4
        && (uint8_t)data[0] == 0x7F
        && data[1] == 'E'
        && data[2] == 'L'
        && data[3] == 'F';
}

#define MAX_FS_NODES 256
#define FS_POOL_SIZE (64 * 1024)

static fs_node_t fs_nodes[MAX_FS_NODES];
static int fs_node_count = 0;
static char fs_data_pool[FS_POOL_SIZE];
static int fs_data_offset = 0;

fs_node_t *fs_root = NULL;
fs_node_t *current_dir = NULL;

static void fs_print_path(fs_node_t *node) {
    if (!node || !fs_root) return;
    if (node == fs_root) {
        putchar('/');
        return;
    }

    fs_print_path(node->parent);
    if (node->parent != fs_root) putchar('/');
    puts(node->name);
}

static fs_node_t *fs_find_child(fs_node_t *dir, const char *name) {
    if (!dir || dir->flags != FS_DIR) return NULL;
    if (strcmp(name, ".") == 0) return dir;
    if (strcmp(name, "..") == 0) return dir->parent;

    fs_node_t *curr = dir->child;
    while (curr) {
        if (strcmp(curr->name, name) == 0) return curr;
        curr = curr->sibling;
    }
    return NULL;
}

static fs_node_t *fs_create_node(fs_node_t *dir, const char *name, uint8_t flags) {
    if (fs_node_count >= MAX_FS_NODES) return NULL;
    if (fs_find_child(dir, name)) return NULL;

    fs_node_t *new_node = &fs_nodes[fs_node_count++];
    strncpy(new_node->name, name, 31);
    new_node->name[31] = '\0';
    new_node->flags = flags;
    new_node->size = 0;
    new_node->parent = dir;
    new_node->child = NULL;
    new_node->sibling = dir->child;
    new_node->content = NULL;
    dir->child = new_node;
    return new_node;
}

static void fs_delete_node(fs_node_t *dir, const char *name) {
    if (!dir || dir->flags != FS_DIR) return;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return;

    fs_node_t *prev = NULL;
    fs_node_t *curr = dir->child;
    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            if (prev) prev->sibling = curr->sibling;
            else dir->child = curr->sibling;
            return;
        }
        prev = curr;
        curr = curr->sibling;
    }
}

static fs_node_t *fs_resolve_dir(fs_node_t *base, const char *path) {
    if (!base || base->flags != FS_DIR || !path || !path[0]) return NULL;
    if (strcmp(path, "/") == 0) return fs_root;
    return fs_find_child(base, path);
}

void fs_init(void) {
    fs_root = &fs_nodes[fs_node_count++];
    strcpy(fs_root->name, "/");
    fs_root->flags = FS_DIR;
    fs_root->size = 0;
    fs_root->parent = fs_root;
    fs_root->child = NULL;
    fs_root->sibling = NULL;
    fs_root->content = NULL;
    current_dir = fs_root;
    
    fs_node_t *calc = fs_create_node(fs_root, "calc.app", FS_FILE);
    if (calc) {
        calc->size = calc_app_size;
        calc->content = (char*)calc_app_data;
    }
}

void cmd_ls_path(const char *path) {
    fs_node_t *dir = current_dir;
    if (path && path[0]) {
        dir = fs_resolve_dir(current_dir, path);
        if (!dir) {
            puts("ls: no such file or directory\n");
            return;
        }
        if (dir->flags != FS_DIR) {
            puts("ls: not a directory\n");
            return;
        }
    }

    fs_node_t *curr = dir->child;
    while (curr) {
        uint8_t old_color = COLOR;
        COLOR = (curr->flags == FS_DIR) ? 0x09 : COLOR_DEFAULT;
        puts(curr->name);
        puts("  ");
        COLOR = old_color;
        curr = curr->sibling;
    }
    putchar('\n');
}

void cmd_ls(void) {
    cmd_ls_path(NULL);
}

void cmd_cd(const char *path) {
    if (!path || !path[0]) return;
    fs_node_t *target = fs_resolve_dir(current_dir, path);
    if (!target) puts("cd: no such file or directory\n");
    else if (target->flags != FS_DIR) puts("cd: not a directory\n");
    else current_dir = target;
}

void cmd_pwd(void) {
    fs_print_path(current_dir);
    putchar('\n');
}

void cmd_mkdir(const char *name) {
    if (!fs_create_node(current_dir, name, FS_DIR)) puts("mkdir: cannot create directory\n");
}

void cmd_touch(const char *name) {
    if (!fs_create_node(current_dir, name, FS_FILE)) puts("touch: cannot create file\n");
}

void cmd_rm(const char *name) {
    fs_delete_node(current_dir, name);
}

void cmd_cat(const char *name) {
    fs_node_t *file = fs_find_child(current_dir, name);
    if (!file) puts("cat: no such file\n");
    else if (file->flags != FS_FILE) puts("cat: is a directory\n");
    else {
        if (file->size > 0 && file->content) {
            for (uint32_t i = 0; i < file->size; i++) putchar(file->content[i]);
        }
        putchar('\n');
    }
}

void cmd_write(const char *name, const char *text) {
    fs_node_t *file = fs_find_child(current_dir, name);
    if (!file) {
        file = fs_create_node(current_dir, name, FS_FILE);
        if (!file) {
            puts("write: cannot create file\n");
            return;
        }
    }

    if (file->flags != FS_FILE) {
        puts("write: is a directory\n");
        return;
    }

    unsigned int len = strlen(text);
    if (fs_data_offset + (int)len > FS_POOL_SIZE) {
        puts("write: out of space\n");
        return;
    }

    file->content = &fs_data_pool[fs_data_offset];
    for (unsigned int i = 0; i < len; i++) file->content[i] = text[i];
    file->size = len;
    fs_data_offset += (int)len;
}

void cmd_cp(const char *src_name, const char *dst_name) {
    fs_node_t *src = fs_find_child(current_dir, src_name);
    if (!src) {
        puts("cp: no such file\n");
        return;
    }

    if (src->flags == FS_DIR) {
        puts("cp: omitting directory\n");
        return;
    }

    fs_node_t *dst = fs_create_node(current_dir, dst_name, FS_FILE);
    if (!dst) {
        puts("cp: cannot create destination\n");
        return;
    }

    if (src->size > 0 && src->content) {
        if (fs_data_offset + (int)src->size > FS_POOL_SIZE) {
            puts("cp: out of space\n");
            return;
        }
        dst->content = &fs_data_pool[fs_data_offset];
        for (uint32_t i = 0; i < src->size; i++) dst->content[i] = src->content[i];
        dst->size = src->size;
        fs_data_offset += (int)src->size;
    }
}

void cmd_ram_exec(const char *name) {
    fs_node_t *file = fs_find_child(current_dir, name);
    if (!file) {
        puts("exec: no such file\n");
        return;
    }
    if (file->flags != FS_FILE) {
        puts("exec: is a directory\n");
        return;
    }
    if (file->size == 0 || !file->content) {
        puts("exec: file is empty\n");
        return;
    }
    if (fs_is_elf_image(file->content, file->size)) {
        puts("exec: unsupported ELF app format\n");
        return;
    }
    
    char *app_start = (char *)0x800000;
    for (uint32_t i = 0; i < file->size; i++) {
        app_start[i] = file->content[i];
    }
    
    app_entry_t app = (app_entry_t)app_start;
    app(&os_api);
}

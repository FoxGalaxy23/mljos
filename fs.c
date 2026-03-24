#include "fs.h"
#include "console.h"
#include "disk.h"
#include "kstring.h"
#include "shell.h"
#include "users.h"
#include "apps/calc_app.h"

typedef void (*app_entry_t)(mljos_api_t*);

#define MAX_FS_NODES 256
#define FS_POOL_SIZE (64 * 1024)

static fs_node_t fs_nodes[MAX_FS_NODES];
static int fs_node_count = 0;
static char fs_data_pool[FS_POOL_SIZE];
static int fs_data_offset = 0;
static uint16_t g_fs_umask = 0022;

fs_node_t *fs_root = NULL;
fs_node_t *current_dir = NULL;

static int fs_is_elf_image(const char *data, uint32_t size) {
    return size >= 4
        && (uint8_t)data[0] == 0x7F
        && data[1] == 'E'
        && data[2] == 'L'
        && data[3] == 'F';
}

static void print_uint(uint32_t value) {
    char buf[16];
    int pos = 0;

    if (value == 0) {
        putchar('0');
        return;
    }

    while (value > 0 && pos < 15) {
        buf[pos++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (pos > 0) putchar(buf[--pos]);
}

static void copy_limited(char *dst, const char *src, int maxlen) {
    int i = 0;
    while (src[i] && i < maxlen - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int path_has_more(const char *path, int index) {
    while (path[index] == '/') index++;
    return path[index] != '\0';
}

static int next_component(const char *path, int *index, char *out, int out_size) {
    int pos = 0;

    while (path[*index] == '/') (*index)++;
    if (!path[*index]) return 0;

    while (path[*index] && path[*index] != '/') {
        if (pos < out_size - 1) out[pos++] = path[*index];
        (*index)++;
    }
    out[pos] = '\0';
    return 1;
}

static void build_node_path(fs_node_t *node, char *out, int out_size) {
    char stack[16][32];
    int depth = 0;
    int pos = 0;

    if (!node || !out || out_size < 2) return;
    if (node == fs_root) {
        out[0] = '/';
        out[1] = '\0';
        return;
    }

    while (node && node != fs_root && depth < 16) {
        copy_limited(stack[depth++], node->name, 32);
        node = node->parent;
    }

    out[pos++] = '/';
    for (int i = depth - 1; i >= 0 && pos < out_size - 1; i--) {
        int j = 0;
        while (stack[i][j] && pos < out_size - 1) out[pos++] = stack[i][j++];
        if (i > 0 && pos < out_size - 1) out[pos++] = '/';
    }
    out[pos] = '\0';
}

static int path_starts_with(const char *path, const char *prefix) {
    int i = 0;
    while (prefix[i]) {
        if (path[i] != prefix[i]) return 0;
        i++;
    }
    return path[i] == '\0' || path[i] == '/';
}

static void expand_path(const char *path, char *out, int out_size) {
    const user_account_t *user = users_current();
    int pos = 0;

    if (!path || !path[0] || strcmp(path, "~") == 0) {
        copy_limited(out, user ? user->home : "/", out_size);
        return;
    }

    if (path[0] == '~' && path[1] == '/') {
        if (user) {
            while (user->home[pos] && pos < out_size - 1) {
                out[pos] = user->home[pos];
                pos++;
            }
        }
        for (int i = 1; path[i] && pos < out_size - 1; i++) out[pos++] = path[i];
        out[pos] = '\0';
        return;
    }

    copy_limited(out, path, out_size);
}

static fs_node_t *fs_find_child(fs_node_t *dir, const char *name) {
    fs_node_t *curr;

    if (!dir || dir->flags != FS_DIR) return NULL;
    if (strcmp(name, ".") == 0) return dir;
    if (strcmp(name, "..") == 0) return dir->parent;

    curr = dir->child;
    while (curr) {
        if (strcmp(curr->name, name) == 0) return curr;
        curr = curr->sibling;
    }
    return NULL;
}

static uint8_t permission_bits(fs_node_t *node) {
    const user_account_t *user = users_effective();
    if (!node || !user) return 0;
    if (users_effective_is_root()) return FS_PERM_READ | FS_PERM_WRITE | FS_PERM_EXEC;
    if (user->uid == node->owner_uid) return (uint8_t)((node->mode >> 6) & 0x7);
    if (user->gid == node->group_gid) return (uint8_t)((node->mode >> 3) & 0x7);
    return (uint8_t)(node->mode & 0x7);
}

static int fs_has_perm(fs_node_t *node, uint8_t perm) {
    return (permission_bits(node) & perm) == perm;
}

static fs_node_t *fs_create_node(fs_node_t *dir, const char *name, uint8_t flags, uint16_t uid, uint16_t gid, uint16_t mode) {
    fs_node_t *new_node;

    if (!dir || dir->flags != FS_DIR || fs_node_count >= MAX_FS_NODES) return NULL;
    if (!name || !name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return NULL;
    if (fs_find_child(dir, name)) return NULL;

    new_node = &fs_nodes[fs_node_count++];
    copy_limited(new_node->name, name, 32);
    new_node->flags = flags;
    new_node->owner_uid = uid;
    new_node->group_gid = gid;
    new_node->mode = mode;
    new_node->size = 0;
    new_node->parent = dir;
    new_node->child = NULL;
    new_node->sibling = dir->child;
    new_node->content = NULL;
    dir->child = new_node;
    return new_node;
}

static fs_node_t *fs_resolve_node(fs_node_t *base, const char *path) {
    fs_node_t *curr;
    char expanded[128];
    char component[32];
    int index = 0;

    if (!base || !path) return NULL;
    expand_path(path, expanded, sizeof(expanded));

    curr = expanded[0] == '/' ? fs_root : base;
    if (strcmp(expanded, "/") == 0) return fs_root;

    while (next_component(expanded, &index, component, sizeof(component))) {
        if (strcmp(component, ".") == 0) continue;
        if (strcmp(component, "..") == 0) {
            curr = curr->parent ? curr->parent : curr;
            continue;
        }

        curr = fs_find_child(curr, component);
        if (!curr) return NULL;
        if (curr->flags == FS_DIR && !fs_has_perm(curr, FS_PERM_EXEC)) return NULL;
    }

    return curr;
}

static fs_node_t *fs_resolve_parent(fs_node_t *base, const char *path, char *leaf, int leaf_size) {
    fs_node_t *curr;
    char expanded[128];
    char component[32];
    int index = 0;

    if (!base || !path || !leaf || leaf_size < 2) return NULL;
    expand_path(path, expanded, sizeof(expanded));
    curr = expanded[0] == '/' ? fs_root : base;

    while (next_component(expanded, &index, component, sizeof(component))) {
        if (!path_has_more(expanded, index)) {
            copy_limited(leaf, component, leaf_size);
            return curr;
        }

        if (strcmp(component, ".") == 0) continue;
        if (strcmp(component, "..") == 0) {
            curr = curr->parent ? curr->parent : curr;
            continue;
        }

        curr = fs_find_child(curr, component);
        if (!curr || curr->flags != FS_DIR || !fs_has_perm(curr, FS_PERM_EXEC)) return NULL;
    }

    return NULL;
}

static void unlink_child(fs_node_t *dir, fs_node_t *target) {
    fs_node_t *curr = dir ? dir->child : NULL;
    fs_node_t *prev = NULL;

    while (curr) {
        if (curr == target) {
            if (prev) prev->sibling = curr->sibling;
            else dir->child = curr->sibling;
            return;
        }
        prev = curr;
        curr = curr->sibling;
    }
}

static void print_mode(fs_node_t *node) {
    const char marks[3] = {'r', 'w', 'x'};
    putchar(node->flags == FS_DIR ? 'd' : '-');
    for (int shift = 6; shift >= 0; shift -= 3) {
        uint8_t bits = (uint8_t)((node->mode >> shift) & 0x7);
        for (int i = 0; i < 3; i++) putchar((bits & (1 << (2 - i))) ? marks[i] : '-');
    }
}

static int parse_mode(const char *text, uint16_t *mode_out) {
    uint16_t mode = 0;
    int len = 0;

    if (!text || !text[0]) return 0;
    while (text[len]) len++;
    if (len != 3) return 0;

    for (int i = 0; i < 3; i++) {
        if (text[i] < '0' || text[i] > '7') return 0;
        mode = (uint16_t)((mode << 3) | (text[i] - '0'));
    }

    *mode_out = mode;
    return 1;
}

static uint16_t apply_symbolic_clause(uint16_t mode, const char *clause) {
    int pos = 0;
    int who = 0;
    char op;
    int perm_bits = 0;
    int shifts[3] = {6, 3, 0};

    while (clause[pos] == 'u' || clause[pos] == 'g' || clause[pos] == 'o' || clause[pos] == 'a') {
        if (clause[pos] == 'u') who |= 1;
        else if (clause[pos] == 'g') who |= 2;
        else if (clause[pos] == 'o') who |= 4;
        else who |= 7;
        pos++;
    }
    if (who == 0) who = 7;

    op = clause[pos++];
    if (op != '+' && op != '-' && op != '=') return mode;

    while (clause[pos]) {
        if (clause[pos] == 'r') perm_bits |= FS_PERM_READ;
        else if (clause[pos] == 'w') perm_bits |= FS_PERM_WRITE;
        else if (clause[pos] == 'x') perm_bits |= FS_PERM_EXEC;
        else return mode;
        pos++;
    }

    for (int i = 0; i < 3; i++) {
        int mask = 1 << i;
        uint16_t shifted = (uint16_t)(perm_bits << shifts[i]);
        uint16_t clear_mask = (uint16_t)(0x7 << shifts[i]);
        if (!(who & mask)) continue;
        if (op == '+') mode |= shifted;
        else if (op == '-') mode &= (uint16_t)~shifted;
        else {
            mode &= (uint16_t)~clear_mask;
            mode |= shifted;
        }
    }

    return mode;
}

static int parse_mode_spec(const char *text, uint16_t current_mode, uint16_t *mode_out) {
    char clause[16];
    int clause_pos = 0;
    int pos = 0;
    uint16_t mode;

    if (parse_mode(text, mode_out)) return 1;
    if (!text || !text[0]) return 0;

    mode = current_mode;
    while (1) {
        char c = text[pos++];
        if (c == ',' || c == '\0') {
            clause[clause_pos] = '\0';
            if (clause_pos == 0) return 0;
            mode = apply_symbolic_clause(mode, clause);
            clause_pos = 0;
            if (c == '\0') break;
            continue;
        }
        if (clause_pos >= (int)sizeof(clause) - 1) return 0;
        clause[clause_pos++] = c;
    }

    *mode_out = mode;
    return 1;
}

static uint16_t fs_default_dir_mode(void) {
    return (uint16_t)(0777 & (uint16_t)~g_fs_umask);
}

static uint16_t fs_default_file_mode(void) {
    return (uint16_t)(0666 & (uint16_t)~g_fs_umask);
}

static int fs_sync_node_to_disk(fs_node_t *node) {
    char path[128];

    if (!node) return 0;
    if (node != fs_root) {
        build_node_path(node, path, sizeof(path));
        if (node->flags == FS_DIR) {
            if (!disk_ensure_directory(path)) return 0;
        } else {
            if (!disk_write_file(path, node->content, node->size)) return 0;
        }
    }

    if (node->flags == FS_DIR) {
        fs_node_t *child = node->child;
        while (child) {
            if (!fs_sync_node_to_disk(child)) return 0;
            child = child->sibling;
        }
    }

    return 1;
}

uint16_t fs_get_umask(void) {
    return g_fs_umask;
}

void fs_set_umask(uint16_t mask) {
    g_fs_umask = (uint16_t)(mask & 0777);
}

int fs_sync_to_disk(void) {
    return fs_sync_node_to_disk(fs_root);
}

void fs_init(void) {
    fs_node_t *calc;

    fs_node_count = 0;
    fs_data_offset = 0;
    g_fs_umask = 0022;

    fs_root = &fs_nodes[fs_node_count++];
    copy_limited(fs_root->name, "/", 32);
    fs_root->flags = FS_DIR;
    fs_root->owner_uid = 0;
    fs_root->group_gid = 0;
    fs_root->mode = 0755;
    fs_root->size = 0;
    fs_root->parent = fs_root;
    fs_root->child = NULL;
    fs_root->sibling = NULL;
    fs_root->content = NULL;
    current_dir = fs_root;

    calc = fs_create_node(fs_root, "calc.app", FS_FILE, 0, 0, 0755);
    if (calc) {
        calc->size = calc_app_size;
        calc->content = (char*)calc_app_data;
    }
}

void fs_ensure_dir(const char *path, uint16_t uid, uint16_t gid, uint16_t mode) {
    fs_node_t *curr = fs_root;
    char expanded[128];
    char component[32];
    int index = 0;
    fs_node_t *next;

    if (!path || !path[0]) return;
    copy_limited(expanded, path, sizeof(expanded));
    if (strcmp(expanded, "/") == 0) return;

    while (next_component(expanded, &index, component, sizeof(component))) {
        int final_component = !path_has_more(expanded, index);
        next = fs_find_child(curr, component);
        if (!next) {
            next = fs_create_node(curr, component, FS_DIR,
                final_component ? uid : 0,
                final_component ? gid : 0,
                final_component ? mode : 0755);
            if (!next) return;
        }
        curr = next;
    }

    curr->owner_uid = uid;
    curr->group_gid = gid;
    curr->mode = mode;
}

void fs_enter_home(void) {
    const user_account_t *user = users_current();
    fs_node_t *home = user ? fs_resolve_node(fs_root, user->home) : fs_root;
    current_dir = home ? home : fs_root;
}

void fs_print_prompt_path(void) {
    char full[128];
    const user_account_t *user = users_current();

    build_node_path(current_dir, full, sizeof(full));
    if (user && path_starts_with(full, user->home)) {
        putchar('~');
        if (strlen(full) > strlen(user->home)) puts(full + strlen(user->home));
        return;
    }
    puts(full);
}

void cmd_ls_path(const char *path) {
    fs_node_t *dir = path && path[0] ? fs_resolve_node(current_dir, path) : current_dir;
    fs_node_t *curr;

    if (!dir) {
        puts("ls: no such file or directory\n");
        return;
    }
    if (dir->flags != FS_DIR) {
        puts("ls: not a directory\n");
        return;
    }
    if (!fs_has_perm(dir, FS_PERM_READ)) {
        puts("ls: permission denied\n");
        return;
    }

    curr = dir->child;
    while (curr) {
        const user_account_t *owner = users_find_by_uid(curr->owner_uid);
        print_mode(curr);
        putchar(' ');
        if (owner) puts(owner->username);
        else print_uint(curr->owner_uid);
        putchar(' ');
        print_uint(curr->size);
        putchar(' ');
        puts(curr->name);
        putchar('\n');
        curr = curr->sibling;
    }
}

void cmd_ls(void) {
    cmd_ls_path(NULL);
}

void cmd_cd(const char *path) {
    fs_node_t *target;

    if (!path || !path[0]) {
        fs_enter_home();
        return;
    }

    target = fs_resolve_node(current_dir, path);
    if (!target) puts("cd: no such file or directory\n");
    else if (target->flags != FS_DIR) puts("cd: not a directory\n");
    else if (!fs_has_perm(target, FS_PERM_EXEC)) puts("cd: permission denied\n");
    else current_dir = target;
}

void cmd_pwd(void) {
    char full[128];
    build_node_path(current_dir, full, sizeof(full));
    puts(full);
    putchar('\n');
}

void cmd_mkdir(const char *path) {
    char leaf[32];
    fs_node_t *parent = fs_resolve_parent(current_dir, path, leaf, sizeof(leaf));
    const user_account_t *user = users_effective();

    if (!parent || !leaf[0]) {
        puts("mkdir: invalid path\n");
        return;
    }
    if (!fs_has_perm(parent, FS_PERM_WRITE | FS_PERM_EXEC)) {
        puts("mkdir: permission denied\n");
        return;
    }
    if (!fs_create_node(parent, leaf, FS_DIR, user->uid, user->gid, fs_default_dir_mode())) puts("mkdir: cannot create directory\n");
}

void cmd_mkdir_p(const char *path) {
    char expanded[128];
    char component[32];
    fs_node_t *curr;
    const user_account_t *user = users_effective();
    int index = 0;

    if (!path || !path[0]) {
        puts("mkdir: missing operand\n");
        return;
    }

    expand_path(path, expanded, sizeof(expanded));
    curr = expanded[0] == '/' ? fs_root : current_dir;

    while (next_component(expanded, &index, component, sizeof(component))) {
        fs_node_t *next;

        if (strcmp(component, ".") == 0) continue;
        if (strcmp(component, "..") == 0) {
            curr = curr->parent ? curr->parent : curr;
            continue;
        }

        next = fs_find_child(curr, component);
        if (!next) {
            if (!fs_has_perm(curr, FS_PERM_WRITE | FS_PERM_EXEC)) {
                puts("mkdir: permission denied\n");
                return;
            }
            next = fs_create_node(curr, component, FS_DIR, user->uid, user->gid, fs_default_dir_mode());
            if (!next) {
                puts("mkdir: cannot create directory\n");
                return;
            }
        } else if (next->flags != FS_DIR) {
            puts("mkdir: path component is not a directory\n");
            return;
        }
        curr = next;
    }
}

void cmd_rmdir(const char *path) {
    char leaf[32];
    fs_node_t *parent = fs_resolve_parent(current_dir, path, leaf, sizeof(leaf));
    fs_node_t *target;

    if (!parent || !leaf[0]) {
        puts("rmdir: invalid path\n");
        return;
    }
    if (!fs_has_perm(parent, FS_PERM_WRITE | FS_PERM_EXEC)) {
        puts("rmdir: permission denied\n");
        return;
    }

    target = fs_find_child(parent, leaf);
    if (!target) {
        puts("rmdir: no such directory\n");
        return;
    }
    if (target->flags != FS_DIR) {
        puts("rmdir: not a directory\n");
        return;
    }
    if (target == fs_root || target == current_dir) {
        puts("rmdir: resource busy\n");
        return;
    }
    if (target->child) {
        puts("rmdir: directory not empty\n");
        return;
    }

    unlink_child(parent, target);
}

void cmd_touch(const char *path) {
    char leaf[32];
    fs_node_t *parent = fs_resolve_parent(current_dir, path, leaf, sizeof(leaf));
    const user_account_t *user = users_effective();

    if (!parent || !leaf[0]) {
        puts("touch: invalid path\n");
        return;
    }
    if (!fs_has_perm(parent, FS_PERM_WRITE | FS_PERM_EXEC)) {
        puts("touch: permission denied\n");
        return;
    }
    if (fs_find_child(parent, leaf)) return;
    if (!fs_create_node(parent, leaf, FS_FILE, user->uid, user->gid, fs_default_file_mode())) puts("touch: cannot create file\n");
}

void cmd_rm(const char *path) {
    char leaf[32];
    fs_node_t *parent = fs_resolve_parent(current_dir, path, leaf, sizeof(leaf));
    fs_node_t *target;

    if (!parent || !leaf[0]) {
        puts("rm: invalid path\n");
        return;
    }
    if (!fs_has_perm(parent, FS_PERM_WRITE | FS_PERM_EXEC)) {
        puts("rm: permission denied\n");
        return;
    }

    target = fs_find_child(parent, leaf);
    if (!target) {
        puts("rm: no such file or directory\n");
        return;
    }
    if (target == fs_root || target == current_dir) {
        puts("rm: resource busy\n");
        return;
    }
    if (target->flags == FS_DIR && target->child) {
        puts("rm: directory not empty\n");
        return;
    }

    unlink_child(parent, target);
}

void cmd_cat(const char *path) {
    fs_node_t *file = fs_resolve_node(current_dir, path);

    if (!file) puts("cat: no such file\n");
    else if (file->flags != FS_FILE) puts("cat: is a directory\n");
    else if (!fs_has_perm(file, FS_PERM_READ)) puts("cat: permission denied\n");
    else {
        if (file->size > 0 && file->content) {
            for (uint32_t i = 0; i < file->size; i++) putchar(file->content[i]);
        }
        putchar('\n');
    }
}

void cmd_write(const char *path, const char *text) {
    char leaf[32];
    fs_node_t *parent = fs_resolve_parent(current_dir, path, leaf, sizeof(leaf));
    fs_node_t *file;
    const user_account_t *user = users_effective();
    unsigned int len;

    if (!parent || !leaf[0]) {
        puts("write: invalid path\n");
        return;
    }

    file = fs_find_child(parent, leaf);
    if (!file) {
        if (!fs_has_perm(parent, FS_PERM_WRITE | FS_PERM_EXEC)) {
            puts("write: permission denied\n");
            return;
        }
        file = fs_create_node(parent, leaf, FS_FILE, user->uid, user->gid, fs_default_file_mode());
        if (!file) {
            puts("write: cannot create file\n");
            return;
        }
    }

    if (file->flags != FS_FILE) {
        puts("write: is a directory\n");
        return;
    }
    if (!fs_has_perm(file, FS_PERM_WRITE)) {
        puts("write: permission denied\n");
        return;
    }

    len = strlen(text);
    if (fs_data_offset + (int)len > FS_POOL_SIZE) {
        puts("write: out of space\n");
        return;
    }

    file->content = &fs_data_pool[fs_data_offset];
    for (unsigned int i = 0; i < len; i++) file->content[i] = text[i];
    file->size = len;
    fs_data_offset += (int)len;
}

void cmd_cp(const char *src_path, const char *dst_path) {
    fs_node_t *src = fs_resolve_node(current_dir, src_path);
    char leaf[32];
    fs_node_t *parent = fs_resolve_parent(current_dir, dst_path, leaf, sizeof(leaf));
    fs_node_t *dst;
    const user_account_t *user = users_effective();

    if (!src) {
        puts("cp: no such source\n");
        return;
    }
    if (src->flags == FS_DIR) {
        puts("cp: omitting directory\n");
        return;
    }
    if (!fs_has_perm(src, FS_PERM_READ)) {
        puts("cp: permission denied\n");
        return;
    }
    if (!parent || !leaf[0]) {
        puts("cp: invalid destination\n");
        return;
    }
    if (!fs_has_perm(parent, FS_PERM_WRITE | FS_PERM_EXEC)) {
        puts("cp: permission denied\n");
        return;
    }
    if (fs_find_child(parent, leaf)) {
        puts("cp: destination exists\n");
        return;
    }
    if (fs_data_offset + (int)src->size > FS_POOL_SIZE) {
        puts("cp: out of space\n");
        return;
    }

    dst = fs_create_node(parent, leaf, FS_FILE, user->uid, user->gid, src->mode);
    if (!dst) {
        puts("cp: cannot create destination\n");
        return;
    }

    if (src->size > 0 && src->content) {
        dst->content = &fs_data_pool[fs_data_offset];
        for (uint32_t i = 0; i < src->size; i++) dst->content[i] = src->content[i];
        dst->size = src->size;
        fs_data_offset += (int)src->size;
    }
}

void cmd_chmod(const char *mode_text, const char *path) {
    fs_node_t *node = fs_resolve_node(current_dir, path);
    uint16_t mode;
    const user_account_t *user = users_effective();

    if (!node) {
        puts("chmod: no such file or directory\n");
        return;
    }
    if (!parse_mode_spec(mode_text, node->mode, &mode)) {
        puts("chmod: use 755 or symbolic mode like u+rwx,g-w\n");
        return;
    }
    if (!users_effective_is_root() && (!user || user->uid != node->owner_uid)) {
        puts("chmod: permission denied\n");
        return;
    }
    node->mode = mode;
}

void cmd_chown(const char *owner_name, const char *path) {
    fs_node_t *node = fs_resolve_node(current_dir, path);
    const user_account_t *owner = users_find(owner_name);

    if (!node) {
        puts("chown: no such file or directory\n");
        return;
    }
    if (!users_effective_is_root()) {
        puts("chown: only root can change owner\n");
        return;
    }
    if (!owner) {
        puts("chown: no such user\n");
        return;
    }
    node->owner_uid = owner->uid;
    node->group_gid = owner->gid;
}

void cmd_ram_exec(const char *path) {
    fs_node_t *file = fs_resolve_node(current_dir, path);

    if (!file) {
        puts("exec: no such file\n");
        return;
    }
    if (file->flags != FS_FILE) {
        puts("exec: is a directory\n");
        return;
    }
    if (!fs_has_perm(file, FS_PERM_READ | FS_PERM_EXEC)) {
        puts("exec: permission denied\n");
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

    {
        char *app_start = (char *)0x800000;
        for (uint32_t i = 0; i < file->size; i++) app_start[i] = file->content[i];
        ((app_entry_t)app_start)(&os_api);
    }
}

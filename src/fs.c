#include "fs.h"
#include "app_layout.h"
#include "console.h"
#include "disk.h"
#include "kmem.h"
#include "kstring.h"
#include "sdk/mljos_app.h"
#include "shell.h"
#include "task.h"
#include "users.h"
#include "apps/calc_app.h"
#include "apps/edit_app.h"
#include "apps/microcoder_app.h"
#include "apps/mcrunner_app.h"
#include "apps/ls_app.h"
#include "apps/cat_app.h"
#include "apps/echo_app.h"
#include "apps/pwd_app.h"
#include "apps/mkdir_app.h"
#include "apps/rm_app.h"
#include "apps/touch_app.h"
#include "apps/clear_app.h"
#include "apps/time_app.h"
#include "apps/date_app.h"
#include "apps/terminal_app.h"
#include "boot/limine_bootx64_efi.h"
#include "common.h"

extern uint8_t _kernel_start[];
extern uint8_t _kernel_end[];

typedef void (*app_entry_t)(mljos_api_t*);

#define MAX_FS_NODES 256
#define FS_POOL_SIZE (64 * 1024)

static fs_node_t fs_nodes[MAX_FS_NODES];
static int fs_node_count = 0;
static char fs_data_pool[FS_POOL_SIZE];
static int fs_data_offset = 0;
static uint16_t g_fs_umask = 0022;

fs_node_t *fs_root = NULL;
static fs_node_t *g_kernel_current_dir = NULL;

fs_node_t *fs_current_dir(void) {
    task_t *t = task_current();
    if (t && t->fs_cwd) return t->fs_cwd;
    return g_kernel_current_dir ? g_kernel_current_dir : fs_root;
}

void fs_set_current_dir(fs_node_t *dir) {
    if (!dir) dir = fs_root;
    task_t *t = task_current();
    if (t) t->fs_cwd = dir;
    else g_kernel_current_dir = dir;
}

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

static int ends_with(const char *text, const char *suffix) {
    int text_len;
    int suffix_len;

    if (!text || !suffix) return 0;
    text_len = strlen(text);
    suffix_len = strlen(suffix);
    if (suffix_len > text_len) return 0;
    return strcmp(text + text_len - suffix_len, suffix) == 0;
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

void fs_get_cwd_path(char *out, int out_size) {
    build_node_path(fs_current_dir(), out, out_size);
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
    fs_node_t *apps_dir;
    fs_node_t *calc;
    fs_node_t *edit;

    fs_node_count = 0;
    fs_data_offset = 0;
    g_fs_umask = 0022;

    fs_root = &fs_nodes[fs_node_count++];
    copy_limited(fs_root->name, "/", 32);
    fs_root->flags = FS_DIR;
    fs_root->owner_uid = 0;
    fs_root->group_gid = 0;
    fs_root->mode = 0777; // Relaxed root permissions for live environment testing
    fs_root->size = 0;
    fs_root->parent = fs_root;
    fs_root->child = NULL;
    fs_root->sibling = NULL;
    fs_root->content = NULL;
    fs_set_current_dir(fs_root);

    apps_dir = fs_create_node(fs_root, "apps", FS_DIR, 0, 0, 0755);

    calc = fs_create_node(apps_dir ? apps_dir : fs_root, "calc.app", FS_FILE, 0, 0, 0755);
    if (calc) {
        calc->size = calc_app_size;
        calc->content = (char*)calc_app_data;
    }

    edit = fs_create_node(apps_dir ? apps_dir : fs_root, "edit.app", FS_FILE, 0, 0, 0755);
    if (edit) {
        edit->size = edit_app_size;
        edit->content = (char*)edit_app_data;
    }

    fs_node_t *microcoder = fs_create_node(apps_dir ? apps_dir : fs_root, "microcoder.app", FS_FILE, 0, 0, 0755);
    if (microcoder) {
        microcoder->size = microcoder_app_size;
        microcoder->content = (char*)microcoder_app_data;
    }

    fs_node_t *mcrunner = fs_create_node(apps_dir ? apps_dir : fs_root, "mcrunner.app", FS_FILE, 0, 0, 0755);
    if (mcrunner) {
        mcrunner->size = mcrunner_app_size;
        mcrunner->content = (char*)mcrunner_app_data;
    }

    fs_node_t *ls = fs_create_node(apps_dir ? apps_dir : fs_root, "ls.app", FS_FILE, 0, 0, 0755);
    if (ls) { ls->size = ls_app_size; ls->content = (char*)ls_app_data; }

    fs_node_t *cat = fs_create_node(apps_dir ? apps_dir : fs_root, "cat.app", FS_FILE, 0, 0, 0755);
    if (cat) { cat->size = cat_app_size; cat->content = (char*)cat_app_data; }

    fs_node_t *echo = fs_create_node(apps_dir ? apps_dir : fs_root, "echo.app", FS_FILE, 0, 0, 0755);
    if (echo) { echo->size = echo_app_size; echo->content = (char*)echo_app_data; }

    fs_node_t *pwd = fs_create_node(apps_dir ? apps_dir : fs_root, "pwd.app", FS_FILE, 0, 0, 0755);
    if (pwd) { pwd->size = pwd_app_size; pwd->content = (char*)pwd_app_data; }

    fs_node_t *mkdir_node = fs_create_node(apps_dir ? apps_dir : fs_root, "mkdir.app", FS_FILE, 0, 0, 0755);
    if (mkdir_node) { mkdir_node->size = mkdir_app_size; mkdir_node->content = (char*)mkdir_app_data; }

    fs_node_t *rm_node = fs_create_node(apps_dir ? apps_dir : fs_root, "rm.app", FS_FILE, 0, 0, 0755);
    if (rm_node) { rm_node->size = rm_app_size; rm_node->content = (char*)rm_app_data; }

    fs_node_t *touch = fs_create_node(apps_dir ? apps_dir : fs_root, "touch.app", FS_FILE, 0, 0, 0755);
    if (touch) { touch->size = touch_app_size; touch->content = (char*)touch_app_data; }

    fs_node_t *clear = fs_create_node(apps_dir ? apps_dir : fs_root, "clear.app", FS_FILE, 0, 0, 0755);
    if (clear) { clear->size = clear_app_size; clear->content = (char*)clear_app_data; }

    fs_node_t *time = fs_create_node(apps_dir ? apps_dir : fs_root, "time.app", FS_FILE, 0, 0, 0755);
    if (time) { time->size = time_app_size; time->content = (char*)time_app_data; }

    fs_node_t *date = fs_create_node(apps_dir ? apps_dir : fs_root, "date.app", FS_FILE, 0, 0, 0755);
    if (date) { date->size = date_app_size; date->content = (char*)date_app_data; }

    fs_node_t *terminal = fs_create_node(apps_dir ? apps_dir : fs_root, "terminal.app", FS_FILE, 0, 0, 0755);
    if (terminal) { terminal->size = terminal_app_size; terminal->content = (char*)terminal_app_data; }

    // UEFI shell fallback: auto-launch default bootloader path.
    fs_node_t *startup = fs_create_node(fs_root, "startup.nsh", FS_FILE, 0, 0, 0644);
    if (startup) {
        static const char *startup_script = "fs0:\\EFI\\BOOT\\BOOTX64.EFI\n";
        startup->size = (uint32_t)strlen(startup_script);
        startup->content = (char*)startup_script;
    }

    // UEFI/Bootloader entries
    fs_ensure_dir("/boot", 0, 0, 0755);
    fs_node_t *boot_dir = fs_resolve_node(fs_root, "/boot");
    if (boot_dir) {
        fs_node_t *kbin = fs_create_node(boot_dir, "mljos.bin", FS_FILE, 0, 0, 0644);
        if (kbin) {
            kbin->size = (uint32_t)((uintptr_t)_kernel_end - (uintptr_t)_kernel_start);
            kbin->content = (char*)_kernel_start;
        }
        
        static const char *cfg = "TIMEOUT=0\n\n:mljOS\n    PROTOCOL=limine\n    KERNEL_PATH=boot:///boot/mljos.bin\n";
        fs_node_t *lcfg = fs_create_node(boot_dir, "limine.cfg", FS_FILE, 0, 0, 0644);
        if (lcfg) { lcfg->size = (uint32_t)strlen(cfg); lcfg->content = (char*)cfg; }
        fs_node_t *lconf = fs_create_node(boot_dir, "limine.conf", FS_FILE, 0, 0, 0644);
        if (lconf) { lconf->size = (uint32_t)strlen(cfg); lconf->content = (char*)cfg; }
    }

    fs_ensure_dir("/EFI/BOOT", 0, 0, 0755);
    fs_node_t *efi_dir = fs_resolve_node(fs_root, "/EFI/BOOT");
    if (efi_dir) {
        fs_node_t *ebin = fs_create_node(efi_dir, "BOOTX64.EFI", FS_FILE, 0, 0, 0644);
        if (ebin && limine_bootx64_efi_size > 0) {
            ebin->size = limine_bootx64_efi_size;
            ebin->content = (char*)limine_bootx64_efi_data;
        }
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
    fs_set_current_dir(home ? home : fs_root);
}

void fs_print_prompt_path(void) {
    char full[128];
    const user_account_t *user = users_current();

    build_node_path(fs_current_dir(), full, sizeof(full));
    if (user && path_starts_with(full, user->home)) {
        putchar('~');
        if (strlen(full) > strlen(user->home)) puts(full + strlen(user->home));
        return;
    }
    puts(full);
}

void cmd_ls_path(const char *path) {
    fs_node_t *cwd = fs_current_dir();
    fs_node_t *dir = path && path[0] ? fs_resolve_node(cwd, path) : cwd;
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

    target = fs_resolve_node(fs_current_dir(), path);
    if (!target) puts("cd: no such file or directory\n");
    else if (target->flags != FS_DIR) puts("cd: not a directory\n");
    else if (!fs_has_perm(target, FS_PERM_EXEC)) puts("cd: permission denied\n");
    else fs_set_current_dir(target);
}

void cmd_pwd(void) {
    char full[128];
    build_node_path(fs_current_dir(), full, sizeof(full));
    puts(full);
    putchar('\n');
}

void cmd_mkdir(const char *path) {
    char leaf[32];
    fs_node_t *parent = fs_resolve_parent(fs_current_dir(), path, leaf, sizeof(leaf));
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
    curr = expanded[0] == '/' ? fs_root : fs_current_dir();

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
    fs_node_t *parent = fs_resolve_parent(fs_current_dir(), path, leaf, sizeof(leaf));
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
    if (target == fs_root || target == fs_current_dir()) {
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
    fs_node_t *parent = fs_resolve_parent(fs_current_dir(), path, leaf, sizeof(leaf));
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
    fs_node_t *parent = fs_resolve_parent(fs_current_dir(), path, leaf, sizeof(leaf));
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
    if (target == fs_root || target == fs_current_dir()) {
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
    fs_node_t *file = fs_resolve_node(fs_current_dir(), path);

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
    if (!fs_write_file(path, text, text ? strlen(text) : 0)) puts("write: unable to write file\n");
}

int fs_write_file(const char *path, const char *data, uint32_t size) {
    char leaf[32];
    fs_node_t *parent = fs_resolve_parent(fs_current_dir(), path, leaf, sizeof(leaf));
    fs_node_t *file;
    const user_account_t *user = users_effective();

    if (!parent || !leaf[0]) return 0;

    file = fs_find_child(parent, leaf);
    if (!file) {
        if (!fs_has_perm(parent, FS_PERM_WRITE | FS_PERM_EXEC)) return 0;
        file = fs_create_node(parent, leaf, FS_FILE, user->uid, user->gid, fs_default_file_mode());
        if (!file) return 0;
    }

    if (file->flags != FS_FILE) return 0;
    if (!fs_has_perm(file, FS_PERM_WRITE)) return 0;

    // Prevent memory leaks on continuous edits by reusing the old allocation if possible
    if (file->size >= size && file->content != NULL) {
        for (uint32_t i = 0; i < size; i++) file->content[i] = data[i];
        file->size = size;
        return 1;
    }

    if (fs_data_offset + (int)size > FS_POOL_SIZE) return 0;

    file->content = &fs_data_pool[fs_data_offset];
    for (uint32_t i = 0; i < size; i++) file->content[i] = data[i];
    file->size = size;
    fs_data_offset += (int)size;
    return 1;
}

int fs_read_file(const char *path, char *out, int maxlen, uint32_t *size_out) {
    fs_node_t *file = fs_resolve_node(fs_current_dir(), path);

    if (!file || !out || maxlen <= 0) return 0;
    if (file->flags != FS_FILE) return 0;
    if (!fs_has_perm(file, FS_PERM_READ)) return 0;
    if ((int)file->size >= maxlen) return 0;

    for (uint32_t i = 0; i < file->size; i++) out[i] = file->content ? file->content[i] : '\0';
    out[file->size] = '\0';
    if (size_out) *size_out = file->size;
    return 1;
}

int fs_read_file_prefix(const char *path, char *out, int maxlen, uint32_t *bytes_read_out) {
    fs_node_t *file = fs_resolve_node(fs_current_dir(), path);

    if (bytes_read_out) *bytes_read_out = 0;
    if (!file || !out || maxlen <= 0) return 0;
    if (file->flags != FS_FILE) return 0;
    if (!fs_has_perm(file, FS_PERM_READ)) return 0;

    uint32_t to_copy = file->size;
    if (to_copy > (uint32_t)maxlen) to_copy = (uint32_t)maxlen;
    for (uint32_t i = 0; i < to_copy; i++) out[i] = file->content ? file->content[i] : '\0';
    if (bytes_read_out) *bytes_read_out = to_copy;
    return 1;
}

void cmd_cp(const char *src_path, const char *dst_path) {
    fs_node_t *src = fs_resolve_node(fs_current_dir(), src_path);
    char leaf[32];
    fs_node_t *parent = fs_resolve_parent(fs_current_dir(), dst_path, leaf, sizeof(leaf));
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
    fs_node_t *node = fs_resolve_node(fs_current_dir(), path);
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
    fs_node_t *node = fs_resolve_node(fs_current_dir(), path);
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

int fs_resolve_app_command(const char *name, char *out, int out_size) {
    int pos = 0;
    int name_has_path = 0;

    if (!name || !name[0] || !out || out_size < 2) return 0;

    for (int i = 0; name[i]; i++) {
        if (name[i] == '/') {
            name_has_path = 1;
            break;
        }
    }

    if (name_has_path) {
        copy_limited(out, name, out_size);
        return 1;
    }

    while (FS_APP_DIR[pos] && pos < out_size - 1) {
        out[pos] = FS_APP_DIR[pos];
        pos++;
    }
    if (pos >= out_size - 1) return 0;
    out[pos++] = '/';

    for (int i = 0; name[i] && pos < out_size - 1; i++) out[pos++] = name[i];
    if (!ends_with(name, ".app")) {
        const char *suffix = ".app";
        for (int i = 0; suffix[i] && pos < out_size - 1; i++) out[pos++] = suffix[i];
    }
    out[pos] = '\0';
    return 1;
}

int fs_can_exec_path(const char *path) {
    fs_node_t *file = fs_resolve_node(fs_current_dir(), path);

    if (!file) return 0;
    if (file->flags != FS_FILE) return 0;
    if (!fs_has_perm(file, FS_PERM_READ | FS_PERM_EXEC)) return 0;
    if (file->size == 0 || !file->content) return 0;
    return 1;
}

int fs_list_dir_file_names(const char *path, char *out, int out_size) {
    fs_node_t *dir;
    fs_node_t *child;
    int pos = 0;
    int first = 1;

    if (!out || out_size <= 0) return 0;
    out[0] = '\0';
    fs_node_t *cwd = fs_current_dir();
    if (!path || !path[0]) dir = cwd;
    else dir = fs_resolve_node(cwd, path);

    if (!dir || dir->flags != FS_DIR) return 0;
    if (!fs_has_perm(dir, FS_PERM_READ)) return 0;

    child = dir->child;
    while (child && pos < out_size - 1) {
        if (child->flags == FS_FILE || child->flags == FS_DIR) {
            // Use null terminator as separator for standalone apps
            for (int i = 0; child->name[i] && pos < out_size - 1; i++) out[pos++] = child->name[i];
            if (pos < out_size - 1) out[pos++] = '\0';
        }
        child = child->sibling;
    }

    out[pos] = '\0';
    return 1;
}

void cmd_ram_exec(const char *path) {
    fs_node_t *file = fs_resolve_node(fs_current_dir(), path);

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
        char *app_start = (char *)(uintptr_t)MLJOS_APP_VADDR;
        if (file->size > (uint32_t)MLJOS_APP_REGION_SIZE) {
            puts("exec: app image too large\n");
            return;
        }
        kmem_memset(app_start, 0, (uint64_t)MLJOS_APP_REGION_SIZE);
        for (uint32_t i = 0; i < file->size; i++) app_start[i] = file->content[i];
        mljos_api_t *api = task_current_api();
        if (!api || !api->puts) api = &os_api;
        uint32_t off = mljos_app_entry_offset_from_image(app_start, file->size);
        ((app_entry_t)(app_start + off))(api);
    }
}

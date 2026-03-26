#include "users.h"
#include "console.h"
#include "disk.h"
#include "fs.h"
#include "kstring.h"
#include "rtc.h"

#define USER_DB_BUFFER_SIZE 1280
#define USER_MUTATION_OK 1
#define USER_MUTATION_FAIL 0

static user_account_t g_users[MAX_USERS];
static int g_user_count = 0;
static uint16_t g_next_uid = 1000;
static user_account_t *g_current_user = NULL;
static user_account_t *g_effective_user = NULL;
static int g_persist_enabled = 1;
static int g_autologin_enabled = 1;
static char g_autologin_user[USERNAME_MAX_LEN] = "guest";
static uint32_t g_salt_counter = 1;
static int g_system_installed = 0;

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

static int string_to_uint16(const char *text, uint16_t *value_out) {
    uint32_t value = 0;
    int i = 0;

    if (!text || !text[0]) return 0;
    while (text[i]) {
        if (text[i] < '0' || text[i] > '9') return 0;
        value = value * 10U + (uint32_t)(text[i] - '0');
        if (value > 65535U) return 0;
        i++;
    }
    *value_out = (uint16_t)value;
    return 1;
}

static int hex_char_to_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static int is_valid_hex_string(const char *text, int expected_len) {
    int i = 0;
    if (!text) return 0;
    while (text[i]) {
        if (hex_char_to_value(text[i]) < 0) return 0;
        i++;
    }
    return i == expected_len - 1;
}

static void hex_from_u32(uint32_t value, char out[9]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) {
        out[i] = hex[value & 0x0F];
        value >>= 4;
    }
    out[8] = '\0';
}

static uint32_t fnv1a_mix(uint32_t hash, uint8_t value) {
    hash ^= value;
    hash *= 16777619U;
    return hash;
}

static uint32_t compute_password_hash_u32(const char *username, const char *password, const char *salt_hex) {
    uint32_t hash = 2166136261U;
    int round;

    for (round = 0; round < 8; round++) {
        for (int i = 0; username && username[i]; i++) hash = fnv1a_mix(hash, (uint8_t)username[i]);
        hash = fnv1a_mix(hash, (uint8_t)(':'));
        for (int i = 0; password && password[i]; i++) hash = fnv1a_mix(hash, (uint8_t)password[i]);
        hash = fnv1a_mix(hash, (uint8_t)(':'));
        for (int i = 0; salt_hex && salt_hex[i]; i++) hash = fnv1a_mix(hash, (uint8_t)salt_hex[i]);
        hash = fnv1a_mix(hash, (uint8_t)round);
    }

    return hash;
}

static void build_password_hash(const char *username, const char *password, const char *salt_hex, char out_hash[PASSWORD_HASH_LEN]) {
    hex_from_u32(compute_password_hash_u32(username, password, salt_hex), out_hash);
}

static void generate_password_salt(char out_salt[PASSWORD_SALT_LEN]) {
    uint8_t day, month, hour, minute, second;
    uint16_t year;
    uint32_t value;

    get_rtc_time(&hour, &minute, &second);
    get_rtc_date(&day, &month, &year);
    value = ((uint32_t)year << 16)
        ^ ((uint32_t)month << 12)
        ^ ((uint32_t)day << 8)
        ^ ((uint32_t)hour << 4)
        ^ ((uint32_t)minute << 1)
        ^ (uint32_t)second
        ^ (g_salt_counter++ * 2654435761U);
    hex_from_u32(value, out_salt);
}

static int is_valid_username(const char *username) {
    int i = 0;

    if (!username || !username[0]) return 0;
    while (username[i]) {
        char c = username[i];
        int ok = (c >= 'a' && c <= 'z')
            || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9')
            || c == '_'
            || c == '-';
        if (!ok || i >= USERNAME_MAX_LEN - 1) return 0;
        i++;
    }
    return 1;
}

static int is_valid_password_input(const char *password) {
    return password && password[0] && strlen(password) < PASSWORD_INPUT_MAX_LEN;
}

static int verify_password(const user_account_t *user, const char *password) {
    char expected[PASSWORD_HASH_LEN];

    if (!user || !is_valid_password_input(password)) return 0;
    build_password_hash(user->username, password, user->password_salt, expected);
    return strcmp(expected, user->password_hash) == 0;
}

static void next_uid_from_table(void) {
    uint16_t next_uid = 1000;

    for (int i = 0; i < g_user_count; i++) {
        if (g_users[i].active && g_users[i].uid >= next_uid) next_uid = (uint16_t)(g_users[i].uid + 1);
    }
    g_next_uid = next_uid;
}

static user_account_t *users_add_internal_hashed(const char *username, const char *salt_hex, const char *hash_hex, uint16_t uid, uint16_t gid, uint8_t role, int can_sudo, const char *home) {
    user_account_t *user;

    if (g_user_count >= MAX_USERS) return NULL;

    user = &g_users[g_user_count++];
    copy_limited(user->username, username, USERNAME_MAX_LEN);
    copy_limited(user->password_salt, salt_hex, PASSWORD_SALT_LEN);
    copy_limited(user->password_hash, hash_hex, PASSWORD_HASH_LEN);
    copy_limited(user->home, home, USER_HOME_MAX_LEN);
    user->uid = uid;
    user->gid = gid;
    user->role = role;
    user->flags = can_sudo ? USER_FLAG_CAN_SUDO : 0;
    user->active = 1;
    return user;
}

static user_account_t *users_add_internal_plain(const char *username, const char *password, uint16_t uid, uint16_t gid, uint8_t role, int can_sudo, const char *home) {
    char salt_hex[PASSWORD_SALT_LEN];
    char hash_hex[PASSWORD_HASH_LEN];

    generate_password_salt(salt_hex);
    build_password_hash(username, password, salt_hex, hash_hex);
    return users_add_internal_hashed(username, salt_hex, hash_hex, uid, gid, role, can_sudo, home);
}

static void users_reset_defaults(void) {
    g_user_count = 0;
    g_next_uid = 1000;
    g_autologin_enabled = 1;
    copy_limited(g_autologin_user, "guest", USERNAME_MAX_LEN);
    g_system_installed = 0;

    users_add_internal_plain("root", "root", 0, 0, USER_ROLE_ADMIN, 1, "/root");
    users_add_internal_plain("guest", "guest", g_next_uid, g_next_uid, USER_ROLE_USER, 0, "/home/guest");
    g_next_uid++;

    g_current_user = &g_users[0];
    g_effective_user = g_current_user;
}

static int user_should_persist(void) {
    return g_persist_enabled;
}

static int append_uint(char *dst, int pos, int maxlen, uint32_t value) {
    char buf[16];
    int len = 0;

    if (value == 0) {
        if (pos < maxlen - 1) dst[pos++] = '0';
        return pos;
    }

    while (value > 0 && len < 15) {
        buf[len++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (len > 0 && pos < maxlen - 1) dst[pos++] = buf[--len];
    return pos;
}

static int append_text(char *dst, int pos, int maxlen, const char *text) {
    int i = 0;
    while (text[i] && pos < maxlen - 1) dst[pos++] = text[i++];
    return pos;
}

void users_set_autologin(int enabled, const char *username) {
    g_autologin_enabled = enabled ? 1 : 0;
    if (username && username[0]) copy_limited(g_autologin_user, username, USERNAME_MAX_LEN);
}

int users_autologin_enabled(void) {
    return g_autologin_enabled;
}

const char *users_autologin_username(void) {
    return g_autologin_user;
}

int users_system_is_installed(void) {
    return g_system_installed;
}

static void users_sync_to_disk(void) {
    char buffer[USER_DB_BUFFER_SIZE];
    int pos = 0;

    if (!user_should_persist()) return;

    pos = append_text(buffer, pos, USER_DB_BUFFER_SIZE, "CONFIG|AUTOLOGIN|");
    pos = append_uint(buffer, pos, USER_DB_BUFFER_SIZE, g_autologin_enabled ? 1U : 0U);
    if (pos < USER_DB_BUFFER_SIZE - 1) buffer[pos++] = '|';
    pos = append_text(buffer, pos, USER_DB_BUFFER_SIZE, g_autologin_user);
    if (pos < USER_DB_BUFFER_SIZE - 1) buffer[pos++] = '\n';

    for (int i = 0; i < g_user_count && pos < USER_DB_BUFFER_SIZE - 1; i++) {
        const user_account_t *user = &g_users[i];
        if (!user->active) continue;
        pos = append_text(buffer, pos, USER_DB_BUFFER_SIZE, "USER|");
        pos = append_text(buffer, pos, USER_DB_BUFFER_SIZE, user->username);
        if (pos < USER_DB_BUFFER_SIZE - 1) buffer[pos++] = '|';
        pos = append_text(buffer, pos, USER_DB_BUFFER_SIZE, user->password_salt);
        if (pos < USER_DB_BUFFER_SIZE - 1) buffer[pos++] = '|';
        pos = append_text(buffer, pos, USER_DB_BUFFER_SIZE, user->password_hash);
        if (pos < USER_DB_BUFFER_SIZE - 1) buffer[pos++] = '|';
        pos = append_uint(buffer, pos, USER_DB_BUFFER_SIZE, user->uid);
        if (pos < USER_DB_BUFFER_SIZE - 1) buffer[pos++] = '|';
        pos = append_uint(buffer, pos, USER_DB_BUFFER_SIZE, user->gid);
        if (pos < USER_DB_BUFFER_SIZE - 1) buffer[pos++] = '|';
        pos = append_uint(buffer, pos, USER_DB_BUFFER_SIZE, user->role);
        if (pos < USER_DB_BUFFER_SIZE - 1) buffer[pos++] = '|';
        pos = append_uint(buffer, pos, USER_DB_BUFFER_SIZE, (user->flags & USER_FLAG_CAN_SUDO) ? 1U : 0U);
        if (pos < USER_DB_BUFFER_SIZE - 1) buffer[pos++] = '|';
        pos = append_text(buffer, pos, USER_DB_BUFFER_SIZE, user->home);
        if (pos < USER_DB_BUFFER_SIZE - 1) buffer[pos++] = '\n';
    }

    buffer[pos] = '\0';
    (void)disk_save_user_config(buffer);
}

static int split_record(char *line, char **fields, int max_fields) {
    int count = 0;
    char *p = line;

    while (count < max_fields) {
        fields[count++] = p;
        while (*p && *p != '|' && *p != '\n' && *p != '\r') p++;
        if (!*p || *p == '\n' || *p == '\r') {
            *p = '\0';
            break;
        }
        *p = '\0';
        p++;
    }
    return count;
}

static int users_parse_config(char *line) {
    char *fields[4];

    if (split_record(line + 7, fields, 3) != 3) return 0;
    if (strcmp(fields[0], "AUTOLOGIN") != 0) return 0;
    g_autologin_enabled = fields[1][0] == '1';
    if (fields[2][0]) copy_limited(g_autologin_user, fields[2], USERNAME_MAX_LEN);
    return 1;
}

static int users_parse_record(char *line) {
    char *fields[8];
    uint16_t uid;
    uint16_t gid;
    uint16_t role_value;
    uint16_t sudo_value;

    if (split_record(line + 5, fields, 8) != 8) return 0;
    if (!is_valid_username(fields[0])) return 0;
    if (!is_valid_hex_string(fields[1], PASSWORD_SALT_LEN)) return 0;
    if (!is_valid_hex_string(fields[2], PASSWORD_HASH_LEN)) return 0;
    if (!string_to_uint16(fields[3], &uid) || !string_to_uint16(fields[4], &gid)) return 0;
    if (!string_to_uint16(fields[5], &role_value) || !string_to_uint16(fields[6], &sudo_value)) return 0;
    if (users_find(fields[0])) return 0;
    return users_add_internal_hashed(fields[0], fields[1], fields[2], uid, gid, (uint8_t)role_value, sudo_value != 0, fields[7]) != NULL;
}

void users_init(void) {
    users_reset_defaults();
}

int users_load_from_disk(void) {
    char buffer[USER_DB_BUFFER_SIZE];
    char line[192];
    int source = 0;
    int loaded_any = 0;
    int line_pos = 0;

    if (!disk_load_user_config(buffer, sizeof(buffer))) return 0;

    g_persist_enabled = 0;
    g_user_count = 0;
    g_next_uid = 1000;
    g_current_user = NULL;
    g_effective_user = NULL;
    g_autologin_enabled = 0;
    g_autologin_user[0] = '\0';

    while (1) {
        char c = buffer[source++];
        if (c == '\0' || c == '\n') {
            line[line_pos] = '\0';
            if (line_pos > 0) {
                if (line[0] == 'C' && line[1] == 'O' && line[2] == 'N' && line[3] == 'F' && line[4] == 'I' && line[5] == 'G' && line[6] == '|') {
                    (void)users_parse_config(line);
                } else if (line[0] == 'U' && line[1] == 'S' && line[2] == 'E' && line[3] == 'R' && line[4] == '|') {
                    if (users_parse_record(line)) loaded_any = 1;
                }
            }
            line_pos = 0;
            if (c == '\0') break;
        } else if (c != '\r' && line_pos < (int)sizeof(line) - 1) {
            line[line_pos++] = c;
        }
    }

    if (!loaded_any || !users_find("root")) {
        users_reset_defaults();
        g_persist_enabled = 1;
        return 0;
    }

    g_system_installed = 1;
    next_uid_from_table();
    for (int i = 0; i < g_user_count; i++) {
        if (g_users[i].uid == 0) {
            g_current_user = &g_users[i];
            g_effective_user = &g_users[i];
            break;
        }
    }
    if (!g_current_user) users_reset_defaults();
    g_persist_enabled = 1;
    return 1;
}

int users_add(const char *username, const char *password, uint8_t role, int can_sudo) {
    char home[USER_HOME_MAX_LEN];
    int pos = 0;
    uint16_t uid;

    if (!is_valid_username(username) || !is_valid_password_input(password)) return USER_MUTATION_FAIL;
    if (users_find(username)) return USER_MUTATION_FAIL;
    if (g_user_count >= MAX_USERS) return USER_MUTATION_FAIL;

    uid = g_next_uid++;
    if (strcmp(username, "root") == 0) copy_limited(home, "/root", USER_HOME_MAX_LEN);
    else {
        const char *prefix = "/home/";
        while (prefix[pos] && pos < USER_HOME_MAX_LEN - 1) {
            home[pos] = prefix[pos];
            pos++;
        }
        for (int i = 0; username[i] && pos < USER_HOME_MAX_LEN - 1; i++) home[pos++] = username[i];
        home[pos] = '\0';
    }

    if (!users_add_internal_plain(username, password, uid, uid, role, can_sudo, home)) return USER_MUTATION_FAIL;
    fs_ensure_dir(home, uid, uid, 0750);
    if (g_system_installed) (void)disk_ensure_directory(home);
    users_sync_to_disk();
    return USER_MUTATION_OK;
}

int users_remove(const char *username) {
    if (!username || strcmp(username, "root") == 0) return USER_MUTATION_FAIL;

    for (int i = 0; i < g_user_count; i++) {
        if (!g_users[i].active || strcmp(g_users[i].username, username) != 0) continue;
        if (&g_users[i] == g_current_user) return USER_MUTATION_FAIL;
        g_users[i].active = 0;
        if (strcmp(g_autologin_user, username) == 0) {
            g_autologin_enabled = 0;
            g_autologin_user[0] = '\0';
        }
        users_sync_to_disk();
        return USER_MUTATION_OK;
    }
    return USER_MUTATION_FAIL;
}

int users_set_password(const char *username, const char *password) {
    user_account_t *user = (user_account_t*)users_find(username);

    if (!user || !is_valid_password_input(password)) return USER_MUTATION_FAIL;
    generate_password_salt(user->password_salt);
    build_password_hash(user->username, password, user->password_salt, user->password_hash);
    users_sync_to_disk();
    return USER_MUTATION_OK;
}

int users_setup_install_owner(const char *username, const char *user_password, const char *root_password, int enable_autologin) {
    user_account_t *owner;

    if (!is_valid_username(username) || !is_valid_password_input(user_password) || !is_valid_password_input(root_password)) return USER_MUTATION_FAIL;

    if (!users_set_password("root", root_password)) return USER_MUTATION_FAIL;
    (void)users_remove("guest");

    owner = (user_account_t*)users_find(username);
    if (!owner) {
        if (!users_add(username, user_password, USER_ROLE_ADMIN, 1)) return USER_MUTATION_FAIL;
        owner = (user_account_t*)users_find(username);
    } else {
        if (!users_set_password(username, user_password)) return USER_MUTATION_FAIL;
        owner->role = USER_ROLE_ADMIN;
        owner->flags |= USER_FLAG_CAN_SUDO;
        if (owner->uid != 0) fs_ensure_dir(owner->home, owner->uid, owner->gid, 0750);
        if (owner->uid != 0) (void)disk_ensure_directory(owner->home);
        users_sync_to_disk();
    }

    g_system_installed = 1;
    users_set_autologin(enable_autologin, username);
    users_sync_to_disk();
    return owner != NULL;
}

const user_account_t *users_find(const char *username) {
    for (int i = 0; i < g_user_count; i++) {
        if (g_users[i].active && strcmp(g_users[i].username, username) == 0) return &g_users[i];
    }
    return NULL;
}

const user_account_t *users_find_by_uid(uint16_t uid) {
    for (int i = 0; i < g_user_count; i++) {
        if (g_users[i].active && g_users[i].uid == uid) return &g_users[i];
    }
    return NULL;
}

const user_account_t *users_current(void) {
    return g_current_user;
}

const user_account_t *users_effective(void) {
    return g_effective_user;
}

int users_login(const char *username, const char *password) {
    user_account_t *user = (user_account_t*)users_find(username);
    if (!user || !verify_password(user, password)) return 0;
    g_current_user = user;
    g_effective_user = user;
    return 1;
}

int users_try_autologin(void) {
    const user_account_t *user;

    if (!g_autologin_enabled || !g_autologin_user[0]) return 0;
    user = users_find(g_autologin_user);
    if (!user) return 0;
    g_current_user = (user_account_t*)user;
    g_effective_user = (user_account_t*)user;
    return 1;
}

int users_su(const char *username, const char *password) {
    return users_login(username, password);
}

int users_begin_sudo(const char *password) {
    if (!g_current_user) return 0;
    if (g_current_user->uid == 0) {
        g_effective_user = g_current_user;
        return 1;
    }
    if (!(g_current_user->flags & USER_FLAG_CAN_SUDO)) return 0;
    if (!verify_password(g_current_user, password)) return 0;
    g_effective_user = (user_account_t*)users_find("root");
    return g_effective_user != NULL;
}

void users_end_sudo(void) {
    g_effective_user = g_current_user;
}

int users_is_root(void) {
    return g_current_user && g_current_user->uid == 0;
}

int users_effective_is_root(void) {
    return g_effective_user && g_effective_user->uid == 0;
}

int users_can_manage_accounts(void) {
    return users_effective_is_root();
}

int users_current_can_sudo(void) {
    return g_current_user && ((g_current_user->flags & USER_FLAG_CAN_SUDO) != 0);
}

void users_list(void) {
    for (int i = 0; i < g_user_count; i++) {
        const user_account_t *user = &g_users[i];
        if (!user->active) continue;
        puts(user->username);
        puts(" uid=");
        print_uint(user->uid);
        puts(" gid=");
        print_uint(user->gid);
        puts(" role=");
        puts(user->role == USER_ROLE_ADMIN ? "admin" : "user");
        puts(" sudo=");
        puts((user->flags & USER_FLAG_CAN_SUDO) ? "yes" : "no");
        puts(" home=");
        puts(user->home);
        putchar('\n');
    }
}

void users_print_id(const char *username) {
    const user_account_t *user = username ? users_find(username) : g_current_user;
    if (!user) {
        puts("id: no such user\n");
        return;
    }

    puts("uid=");
    print_uint(user->uid);
    putchar('(');
    puts(user->username);
    putchar(')');
    puts(" gid=");
    print_uint(user->gid);
    putchar('(');
    puts(user->username);
    putchar(')');
    puts(" role=");
    puts(user->role == USER_ROLE_ADMIN ? "admin" : "user");
    puts(" sudo=");
    puts((user->flags & USER_FLAG_CAN_SUDO) ? "yes" : "no");
    putchar('\n');
}

void users_print_groups(const char *username) {
    const user_account_t *user = username ? users_find(username) : g_current_user;
    if (!user) {
        puts("groups: no such user\n");
        return;
    }

    puts(user->username);
    puts(" : ");
    puts(user->username);
    if (user->flags & USER_FLAG_CAN_SUDO) puts(" sudo");
    if (user->role == USER_ROLE_ADMIN) puts(" admin");
    putchar('\n');
}

void users_persist(void) {
    users_sync_to_disk();
}

void users_bootstrap_fs(void) {
    fs_ensure_dir("/home", 0, 0, 0755);
    fs_ensure_dir("/root", 0, 0, 0700);
    fs_ensure_dir("/tmp", 0, 0, 0777);
    fs_ensure_dir("/system", 0, 0, 0755);
    fs_ensure_dir("/system/autorun", 0, 0, 0755);

    for (int i = 0; i < g_user_count; i++) {
        const user_account_t *user = &g_users[i];
        if (!user->active || user->uid == 0) continue;
        fs_ensure_dir(user->home, user->uid, user->gid, 0750);
    }
}

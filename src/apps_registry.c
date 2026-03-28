#include "apps_registry.h"
#include "disk.h"
#include "fs.h"
#include "kstring.h"
#include "sdk/mljos_app.h"
#include "users.h"

#define APP_CACHE_MAX 64
#define APP_LIST_BUF_SIZE 4096

static mljos_app_descriptor_t g_cache[APP_CACHE_MAX];
static uint32_t g_cache_flags[APP_CACHE_MAX];
static char g_cache_names[APP_CACHE_MAX][32];
static char g_cache_titles[APP_CACHE_MAX][64];
static int g_cache_count = 0;
static int g_cache_valid = 0;

static int ends_with(const char *s, const char *suffix) {
    if (!s || !suffix) return 0;
    unsigned int sl = strlen(s);
    unsigned int su = strlen(suffix);
    if (su > sl) return 0;
    return strncmp(s + (sl - su), suffix, su) == 0;
}

static void copy_name_without_app_suffix(char out[32], const char *filename) {
    if (!out) return;
    out[0] = '\0';
    if (!filename || !filename[0]) return;

    unsigned int n = strlen(filename);
    if (n >= 4 && ends_with(filename, ".app")) n -= 4;
    if (n > 31) n = 31;
    for (unsigned int i = 0; i < n; ++i) out[i] = filename[i];
    out[n] = '\0';
}

static void copy_title_or_fallback(char out[64], const char *title, const char *fallback_name) {
    if (!out) return;
    out[0] = '\0';
    if (title && title[0]) {
        // Copy up to 63 chars, always NUL-terminate.
        for (int i = 0; i < 63 && title[i]; ++i) out[i] = title[i];
        out[63] = '\0';
        return;
    }
    if (fallback_name && fallback_name[0]) {
        for (int i = 0; i < 63 && fallback_name[i]; ++i) out[i] = fallback_name[i];
        out[63] = '\0';
    }
}

static int read_app_header_prefix(const char *app_path, mljos_app_header_v1_t *out_hdr) {
    if (!app_path || !out_hdr) return 0;

    uint8_t buf[4096];
    uint32_t got = 0;
    int ok = 0;

    if (users_system_is_installed()) {
        ok = disk_read_file_prefix(app_path, (char *)buf, (int)sizeof(buf), &got);
        if (!ok) ok = fs_read_file_prefix(app_path, (char *)buf, (int)sizeof(buf), &got);
    } else {
        ok = fs_read_file_prefix(app_path, (char *)buf, (int)sizeof(buf), &got);
        if (!ok) ok = disk_read_file_prefix(app_path, (char *)buf, (int)sizeof(buf), &got);
    }

    if (!ok || got < (uint32_t)sizeof(mljos_app_header_v1_t)) return 0;
    *out_hdr = *(const mljos_app_header_v1_t *)buf;
    return 1;
}

void apps_registry_refresh(void) {
    char names_buf[APP_LIST_BUF_SIZE];
    int ok = 0;

    g_cache_count = 0;

    if (users_system_is_installed()) {
        ok = disk_list_dir_file_names(FS_APP_DIR, names_buf, (int)sizeof(names_buf));
        if (!ok) ok = fs_list_dir_file_names(FS_APP_DIR, names_buf, (int)sizeof(names_buf));
    } else {
        ok = fs_list_dir_file_names(FS_APP_DIR, names_buf, (int)sizeof(names_buf));
        if (!ok) ok = disk_list_dir_file_names(FS_APP_DIR, names_buf, (int)sizeof(names_buf));
    }

    if (!ok) {
        g_cache_valid = 1;
        return;
    }

    int p = 0;
    while (p < (int)sizeof(names_buf) && names_buf[p] && g_cache_count < APP_CACHE_MAX) {
        const char *filename = &names_buf[p];
        int flen = (int)strlen(filename);
        p += flen + 1;

        if (!ends_with(filename, ".app")) continue;

        g_cache_flags[g_cache_count] = MLJOS_APP_FLAG_TUI;

        copy_name_without_app_suffix(g_cache_names[g_cache_count], filename);
        if (!g_cache_names[g_cache_count][0]) continue;

        char app_path[128];
        // "/apps/<filename>"
        int pos = 0;
        const char *prefix = FS_APP_DIR;
        for (int i = 0; prefix[i] && pos < (int)sizeof(app_path) - 1; ++i) app_path[pos++] = prefix[i];
        if (pos < (int)sizeof(app_path) - 1) app_path[pos++] = '/';
        for (int i = 0; filename[i] && pos < (int)sizeof(app_path) - 1; ++i) app_path[pos++] = filename[i];
        app_path[pos] = '\0';

        mljos_app_header_v1_t hdr;
        if (read_app_header_prefix(app_path, &hdr) && mljos_app_header_v1_valid(&hdr)) {
            g_cache_flags[g_cache_count] = hdr.flags;
            // Ensure NUL-termination defensively.
            hdr.title[63] = '\0';
            copy_title_or_fallback(g_cache_titles[g_cache_count], hdr.title, g_cache_names[g_cache_count]);
        } else {
            copy_title_or_fallback(g_cache_titles[g_cache_count], NULL, g_cache_names[g_cache_count]);
        }

        g_cache[g_cache_count].name = g_cache_names[g_cache_count];
        g_cache[g_cache_count].title = g_cache_titles[g_cache_count];
        g_cache[g_cache_count].supports_ui = (g_cache_flags[g_cache_count] & MLJOS_APP_FLAG_GUI) ? 1 : 0;

        g_cache_count++;
    }

    g_cache_valid = 1;
}

const mljos_app_descriptor_t *apps_registry_list(int *count_out) {
    if (!g_cache_valid) apps_registry_refresh();
    if (count_out) *count_out = g_cache_count;
    return g_cache;
}

int apps_registry_supports_ui(const char *name) {
    if (!name || !name[0]) return 0;
    uint32_t flags = apps_registry_app_flags(name);
    return (flags & MLJOS_APP_FLAG_GUI) ? 1 : 0;
}

uint32_t apps_registry_app_flags(const char *name) {
    if (!name || !name[0]) return MLJOS_APP_FLAG_TUI;
    if (!g_cache_valid) apps_registry_refresh();

    // Strip ".app" if passed.
    char key[32];
    copy_name_without_app_suffix(key, name);
    const char *needle = key[0] ? key : name;

    for (int i = 0; i < g_cache_count; ++i) {
        if (g_cache[i].name && strcmp(g_cache[i].name, needle) == 0) return g_cache_flags[i];
    }
    return MLJOS_APP_FLAG_TUI;
}

#include "apps_registry.h"
#include "kstring.h"

static const mljos_app_descriptor_t g_apps[] = {
    { "calc", "Calculator", 1 },
    { "time", "Time", 1 },

    // Text-only apps (not shown in Start menu)
    { "edit", "Editor", 0 },
    { "microcoder", "Microcoder", 0 },
    { "mcrunner", "MC Runner", 0 },
    { "ls", "LS", 0 },
    { "cat", "Cat", 0 },
    { "echo", "Echo", 0 },
    { "pwd", "PWD", 0 },
    { "mkdir", "MKDIR", 0 },
    { "rm", "RM", 0 },
    { "touch", "Touch", 0 },
    { "clear", "Clear", 0 },
    { "date", "Date", 0 },
};

const mljos_app_descriptor_t *apps_registry_list(int *count_out) {
    int count = (int)(sizeof(g_apps) / sizeof(g_apps[0]));
    if (count_out) *count_out = count;
    return g_apps;
}

int apps_registry_supports_ui(const char *name) {
    if (!name || !name[0]) return 0;
    int count = 0;
    const mljos_app_descriptor_t *apps = apps_registry_list(&count);
    for (int i = 0; i < count; ++i) {
        if (apps[i].name && strcmp(apps[i].name, name) == 0) return apps[i].supports_ui ? 1 : 0;
    }
    return 0;
}


#ifndef APPS_REGISTRY_H
#define APPS_REGISTRY_H

#include "common.h"

typedef struct mljos_app_descriptor {
    const char *name;        // command name (e.g. "calc")
    const char *title;       // display name for GUI
    uint8_t supports_ui;     // 1 if app has GUI mode
} mljos_app_descriptor_t;

// Refreshes the internal cache by scanning /apps and reading app headers.
// Safe to call multiple times.
void apps_registry_refresh(void);

// Returns pointer to a static array of descriptors.
// `count_out` can be NULL.
const mljos_app_descriptor_t *apps_registry_list(int *count_out);

// Returns 1 if app name exists in registry and supports UI, else 0.
int apps_registry_supports_ui(const char *name);

// Returns the app capability flags (MLJOS_APP_FLAG_*). Defaults to TUI-only if
// app is not found or does not have a header.
uint32_t apps_registry_app_flags(const char *name);

#endif

#include "sdk/mljos_api.h"
#include "sdk/mljos_app.h"

MLJOS_APP_DEFINE("Clear", MLJOS_APP_FLAG_TUI);

MLJOS_APP_ENTRY void _start(mljos_api_t *api) {
    api->clear_screen();
}

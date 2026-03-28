#include "sdk/mljos_api.h"
#include "sdk/mljos_app.h"

MLJOS_APP_DEFINE("Terminal", MLJOS_APP_FLAG_TUI | MLJOS_APP_FLAG_GUI);

MLJOS_APP_ENTRY void _start(mljos_api_t *api) {
    if (api && api->run_shell) api->run_shell();
}

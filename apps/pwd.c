#include "sdk/mljos_api.h"
#include "sdk/mljos_app.h"

MLJOS_APP_DEFINE("PWD", MLJOS_APP_FLAG_TUI);

MLJOS_APP_ENTRY void _start(mljos_api_t *api) {
    char buf[128];
    if (api->get_cwd(buf, sizeof(buf))) {
        api->puts(buf);
        api->putchar('\n');
    } else {
        api->puts("pwd: error getting current directory\n");
    }
}

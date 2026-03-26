#include "sdk/mljos_api.h"

void _start(mljos_api_t *api) {
    char buf[128];
    if (api->get_cwd(buf, sizeof(buf))) {
        api->puts(buf);
        api->putchar('\n');
    } else {
        api->puts("pwd: error getting current directory\n");
    }
}

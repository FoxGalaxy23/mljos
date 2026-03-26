#include "sdk/mljos_api.h"

void _start(mljos_api_t *api) {
    if (!api->open_path || !api->open_path[0]) {
        api->puts("mkdir: missing operand\n");
        return;
    }
    
    if (!api->mkdir(api->open_path)) {
        api->puts("mkdir: cannot create directory '");
        api->puts(api->open_path);
        api->puts("'\n");
    }
}

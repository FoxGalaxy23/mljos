#include "sdk/mljos_api.h"

void _start(mljos_api_t *api) {
    if (!api->open_path || !api->open_path[0]) {
        api->puts("touch: missing file operand\n");
        return;
    }
    
    if (!api->write_file(api->open_path, "", 0)) {
        api->puts("touch: cannot touch '");
        api->puts(api->open_path);
        api->puts("'\n");
    }
}

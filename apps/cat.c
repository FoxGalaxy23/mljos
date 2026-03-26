#include "sdk/mljos_api.h"

void _start(mljos_api_t *api) {
    if (!api->open_path || !api->open_path[0]) {
        api->puts("cat: missing file operand\n");
        return;
    }
    
    char buf[4096];
    unsigned int size = 0;
    if (api->read_file(api->open_path, buf, sizeof(buf), &size)) {
        for (unsigned int i = 0; i < size; i++) {
            api->putchar(buf[i]);
        }
        api->putchar('\n');
    } else {
        api->puts("cat: error reading file '");
        api->puts(api->open_path);
        api->puts("'\n");
    }
}

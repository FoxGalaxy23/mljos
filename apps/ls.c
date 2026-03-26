#include "sdk/mljos_api.h"

void _start(mljos_api_t *api) {
    char buf[1024];
    const char *path = (api->open_path && api->open_path[0]) ? api->open_path : ".";
    
    if (api->list_dir(path, buf, sizeof(buf))) {
        char *p = buf;
        while (*p) {
            api->puts(p);
            api->putchar('\n');
            while (*p) p++;
            p++;
        }
    } else {
        api->puts("ls: error listing directory '");
        api->puts(path);
        api->puts("'\n");
    }
}

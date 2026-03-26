#include "sdk/mljos_api.h"

void _start(mljos_api_t *api) {
    unsigned char h, m, s;
    api->get_time(&h, &m, &s);
    
    api->putchar('0' + (h / 10));
    api->putchar('0' + (h % 10));
    api->putchar(':');
    api->putchar('0' + (m / 10));
    api->putchar('0' + (m % 10));
    api->putchar(':');
    api->putchar('0' + (s / 10));
    api->putchar('0' + (s % 10));
    api->putchar('\n');
}

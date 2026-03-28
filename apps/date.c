#include "sdk/mljos_api.h"
#include "sdk/mljos_app.h"

MLJOS_APP_DEFINE("Date", MLJOS_APP_FLAG_TUI);

MLJOS_APP_ENTRY void _start(mljos_api_t *api) {
    unsigned char d, mo;
    unsigned short y;
    api->get_date(&d, &mo, &y);
    
    api->putchar('0' + (d / 10));
    api->putchar('0' + (d % 10));
    api->putchar('.');
    api->putchar('0' + (mo / 10));
    api->putchar('0' + (mo % 10));
    api->putchar('.');
    
    char ybuf[5];
    ybuf[4] = '\0';
    ybuf[3] = '0' + (y % 10); y /= 10;
    ybuf[2] = '0' + (y % 10); y /= 10;
    ybuf[1] = '0' + (y % 10); y /= 10;
    ybuf[0] = '0' + (y % 10);
    api->puts(ybuf);
    api->putchar('\n');
}

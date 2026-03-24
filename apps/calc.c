#include "../sdk/mljos_api.h"

void _start(mljos_api_t *api) {
    char buf[128];
    api->puts("Welcome to mljOS Calculator!\n");
    api->puts("Type 'quit' to exit.\n");
    
    while(1) {
        api->puts("calc> ");
        int len = api->read_line(buf, sizeof(buf));
        if (len == 0) continue;
        
        if (buf[0] == 'q' && buf[1] == 'u' && buf[2] == 'i' && buf[3] == 't') {
            break;
        }
        
        int a = 0, b = 0;
        char op = 0;
        int i = 0;
        
        while(buf[i] && buf[i] == ' ') i++;
        while(buf[i] >= '0' && buf[i] <= '9') {
            a = a * 10 + (buf[i] - '0');
            i++;
        }
        while(buf[i] && buf[i] == ' ') i++;
        if (buf[i]) {
            op = buf[i];
            i++;
        }
        while(buf[i] && buf[i] == ' ') i++;
        while(buf[i] >= '0' && buf[i] <= '9') {
            b = b * 10 + (buf[i] - '0');
            i++;
        }
        
        if (op == 0) continue;
        
        int res = 0;
        if (op == '+') res = a + b;
        else if (op == '-') res = a - b;
        else if (op == '*') res = a * b;
        else if (op == '/') {
            if (b != 0) res = a / b;
            else {
                api->puts("Error: Division by zero\n");
                continue;
            }
        } else {
            api->puts("Error: Unknown operator\n");
            continue;
        }
        
        char out[16];
        int out_idx = 0;
        if (res == 0) {
            out[out_idx++] = '0';
        } else {
            int temp = res;
            if (res < 0) {
                api->putchar('-');
                temp = -res;
            }
            char tmp_buf[16];
            int tmp_idx = 0;
            while(temp > 0) {
                tmp_buf[tmp_idx++] = '0' + (temp % 10);
                temp /= 10;
            }
            while(tmp_idx > 0) out[out_idx++] = tmp_buf[--tmp_idx];
        }
        out[out_idx++] = '\n';
        out[out_idx] = '\0';
        api->puts(out);
    }
}

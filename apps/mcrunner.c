/*
 * Tiny-C mcrunner (mljOS app)
 *
 * Minimal Tiny-C VM that executes bytecode appended to its own binary.
 */

#include "sdk/mljos_api.h"
#include "sdk/mljos_app.h"

MLJOS_APP_DEFINE("MC Runner", MLJOS_APP_FLAG_TUI);

MLJOS_APP_ENTRY void _start(mljos_api_t *api);

#ifndef NULL
#define NULL ((void*)0)
#endif

#define MAX_CODE 1000
#define MAX_STACK 1000

enum {
    IFETCH, ISTORE, IPUSH, IPOP, IADD, ISUB, IMUL, IDIV, IMOD,
    ILT, IGT, IGE, ILE, IEQ, INE, JZ, JNZ, JMP, IPRINT, HALT
};

typedef signed char code;

MLJOS_APP_ENTRY void _start(mljos_api_t *api) {
    // The runner logic:
    // 1. In mljOS, apps are loaded at 0x800000.
    // 2. This runner binary has a certain size (RUNNER_SIZE).
    // 3. Bytecode is expected to be at 0x800000 + RUNNER_SIZE.
    
    // We'll use a placeholder for RUNNER_SIZE that microcoder will patch,
    // or just use a magic constant if we know the size after build.
    // Let's use a magic marker to find the start of bytecode.
    
    unsigned char *base = (unsigned char *)0x800000;
    unsigned char *p = base;
    
    // Search for the magic marker: "MCBYTE"
    int found = 0;
    // Scan a larger window to survive bigger runner images.
    for (int i = 0; i < 65536; i++) {
        if (p[i] == 'M' && p[i+1] == 'C' && p[i+2] == 'B' && p[i+3] == 'Y' && p[i+4] == 'T' && p[i+5] == 'E') {
            p = &p[i+6];
            found = 1;
            break;
        }
    }
    
    if (!found) {
        api->puts("mcrunner error: bytecode not found\n");
        return;
    }

    // VM state
   static int vm_stack[1024] = {0};
static int globals[26] = {0};
    int sp = 0;

    code *pc = (code *)p;

    while (1) {
        code instr = *pc++;
        switch (instr) {
            case IFETCH: {
                int reg = (int)(*pc++);
                vm_stack[sp++] = globals[reg];
                break;
            }
            case ISTORE: {
                int reg = (int)(*pc++);
                globals[reg] = vm_stack[--sp];
                break;
            }
            case IPUSH: {
                int v = (pc[0] & 0xFF) | ((pc[1] & 0xFF) << 8) | ((pc[2] & 0xFF) << 16) | ((pc[3] & 0xFF) << 24);
                pc += 4;
                vm_stack[sp++] = v;
                break;
            }
            case IPOP:
                sp--;
                break;
            case IADD: {
                int b = vm_stack[--sp];
                int a = vm_stack[--sp];
                vm_stack[sp++] = a + b;
                break;
            }
            case ISUB: {
                int b = vm_stack[--sp];
                int a = vm_stack[--sp];
                vm_stack[sp++] = a - b;
                break;
            }
            case IMUL: {
                int b = vm_stack[--sp];
                int a = vm_stack[--sp];
                vm_stack[sp++] = a * b;
                break;
            }
            case IDIV: {
                int b = vm_stack[--sp];
                int a = vm_stack[--sp];
                vm_stack[sp++] = (b != 0) ? a / b : 0;
                break;
            }
            case IMOD: {
                int b = vm_stack[--sp];
                int a = vm_stack[--sp];
                vm_stack[sp++] = (b != 0) ? a % b : 0;
                break;
            }
            case ILT: {
                int b = vm_stack[--sp];
                int a = vm_stack[--sp];
                vm_stack[sp++] = (a < b);
                break;
            }
            case IGT: {
                int b = vm_stack[--sp];
                int a = vm_stack[--sp];
                vm_stack[sp++] = (a > b);
                break;
            }
            case IGE: {
                int b = vm_stack[--sp];
                int a = vm_stack[--sp];
                vm_stack[sp++] = (a >= b);
                break;
            }
            case ILE: {
                int b = vm_stack[--sp];
                int a = vm_stack[--sp];
                vm_stack[sp++] = (a <= b);
                break;
            }
            case IEQ: {
                int b = vm_stack[--sp];
                int a = vm_stack[--sp];
                vm_stack[sp++] = (a == b);
                break;
            }
            case INE: {
                int b = vm_stack[--sp];
                int a = vm_stack[--sp];
                vm_stack[sp++] = (a != b);
                break;
            }
            case JZ: {
                int off = (int)(*pc++);
                if (vm_stack[--sp] == 0) pc += off;
                break;
            }
            case JNZ: {
                int off = (int)(*pc++);
                if (vm_stack[--sp] != 0) pc += off;
                break;
            }
            case JMP: {
                int off = (int)(*pc++);
                pc += off;
                break;
            }
            case IPRINT: {
                int v = vm_stack[--sp];
                
                // Minimal itoa-like print
                if (v < 0) { api->putchar('-'); v = -v; }
                if (v == 0) { api->putchar('0'); }
                else {
                    char buf[12];
                    int i = 0;
                    while (v > 0) { buf[i++] = (char)((v % 10) + '0'); v /= 10; }
                    while (i > 0) api->putchar(buf[--i]);
                }
                api->putchar('\n');
                break;
            }
            case HALT:
                return;
            default:
                api->puts("mcrunner error: unknown instruction\n");
                return;
        }
    }
}

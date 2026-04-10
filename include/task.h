#ifndef TASK_H
#define TASK_H

#include "common.h"
#include "sdk/mljos_api.h"

typedef struct console console_t;

typedef enum {
    TASK_UNUSED = 0,
    TASK_RUNNABLE = 1,
    TASK_PAUSED = 2,
    TASK_DEAD = 3,
} task_state_t;

typedef struct task_context {
    uint64_t rsp;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rip;
    uint64_t cr3;
} task_context_t;

struct wm_window;
struct fs_node;
typedef struct fs_node fs_node_t;

typedef void (*task_entry_t)(void *arg);

typedef struct task {
    task_state_t state;
    const char *name;
    task_context_t ctx;
    task_entry_t entry;
    void *arg;
    struct wm_window *window; // owned UI window (may be NULL)
    console_t *console;       // optional per-task console (Terminal)
    fs_node_t *fs_cwd;        // per-task current directory (RAM FS)
    int shell_active_storage;
    int shell_location;
    uint32_t shell_launch_flags;
    char shell_open_path[128];
    void *shell_jmp_env[8];
    int shell_jmp_ready;
    char shell_history[16][128];
    int shell_history_count;
    int shell_history_pos;
    mljos_api_t api;
    uint8_t killed;
} task_t;

void task_init(void);

task_t *task_current(void);
mljos_api_t *task_current_api(void);

// Creates a kernel-mode task with its own stack and address space (separate CR3),
// but not loaded from an app image.
task_t *task_create_kernel(const char *name, task_entry_t entry, void *arg);

// Creates a task backed by a raw .app image that must be linked for MLJOS_APP_VADDR.
task_t *task_create_app(const char *name, const void *image, uint32_t image_size);

void task_attach_window(task_t *t, struct wm_window *w);
void task_attach_console(task_t *t, console_t *c);
void task_set_paused(task_t *t, int paused);
int task_is_alive(const task_t *t);

// Called by tasks to cooperatively yield back to the kernel scheduler.
void task_yield(void);

// Marks current task dead and yields (never returns).
__attribute__((noreturn)) void task_exit(void);

// Run one scheduling quantum (switches into one runnable task).
void task_schedule_once(void);

// Ask a task to exit when it next yields.
void task_kill(task_t *t);

#endif

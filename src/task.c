#include "task.h"

#include "app_layout.h"
#include "kmem.h"
#include "sdk/mljos_app.h"

#define MAX_TASKS 16
#define TASK_STACK_SIZE (64 * 1024)
#define APP_REGION_SIZE (2 * 1024 * 1024ULL)

static task_t g_tasks[MAX_TASKS];
static task_t *g_current = NULL;
static task_context_t g_kernel_ctx;
static uint64_t g_kernel_cr3 = 0;
static int g_rr_pos = 0;

static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void write_cr3(uint64_t v) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(v) : "memory");
}

__attribute__((naked)) static void ctx_switch(task_context_t *old_ctx, task_context_t *new_ctx) {
    __asm__ volatile (
        ".intel_syntax noprefix\n"
        // rdi = old, rsi = new
        "mov [rdi + 0], rsp\n"
        "mov [rdi + 8], rbx\n"
        "mov [rdi + 16], rbp\n"
        "mov [rdi + 24], r12\n"
        "mov [rdi + 32], r13\n"
        "mov [rdi + 40], r14\n"
        "mov [rdi + 48], r15\n"
        "lea rax, [rip + 0f]\n"
        "mov [rdi + 56], rax\n"
        "mov rax, cr3\n"
        "mov [rdi + 64], rax\n"

        "mov rax, [rsi + 64]\n"
        "mov cr3, rax\n"
        "mov rsp, [rsi + 0]\n"
        "mov rbx, [rsi + 8]\n"
        "mov rbp, [rsi + 16]\n"
        "mov r12, [rsi + 24]\n"
        "mov r13, [rsi + 32]\n"
        "mov r14, [rsi + 40]\n"
        "mov r15, [rsi + 48]\n"
        "jmp qword ptr [rsi + 56]\n"
        "0:\n"
        "ret\n"
        ".att_syntax prefix\n"
    );
}

static void task_trampoline(void) __attribute__((noreturn));
static void task_trampoline(void) {
    task_t *t = g_current;
    if (t && t->entry) t->entry(t->arg);
    task_exit();
}

static void app_trampoline(void) __attribute__((noreturn));
static void app_trampoline(void) {
    task_t *t = g_current;
    if (!t) task_exit();
    uint32_t off = mljos_app_entry_offset_from_image((const void *)(uintptr_t)MLJOS_APP_VADDR, 0);
    void (*app_entry)(mljos_api_t *) = (void (*)(mljos_api_t *))(uintptr_t)(MLJOS_APP_VADDR + (uint64_t)off);
    app_entry(&t->api);
    task_exit();
}

static task_t *task_alloc_slot(void) {
    for (int i = 0; i < MAX_TASKS; ++i) {
        if (g_tasks[i].state == TASK_UNUSED) return &g_tasks[i];
    }
    return NULL;
}

static void clone_page_tables(uint64_t *out_cr3, uint64_t app_phys_2mib) {
    // Kernel sets up 6 consecutive 4K pages: PML4, PDPT, PD0..PD3.
    uint64_t kernel_cr3 = g_kernel_cr3;
    uint8_t *src = (uint8_t *)(uintptr_t)kernel_cr3;
    uint8_t *dst = (uint8_t *)kmem_alloc(6 * 4096, 4096);
    kmem_memcpy(dst, src, 6 * 4096);

    // IMPORTANT: patch the copied pointers to point to the copied lower-level tables.
    // Otherwise CR3 would still reference the kernel's original PDPT/PD pages.
    uint64_t *pml4 = (uint64_t *)dst;
    uint64_t *pdpt = (uint64_t *)(dst + 1 * 4096);

    pml4[0] = ((uint64_t)(uintptr_t)pdpt) | 0x03ULL;
    for (int i = 0; i < 4; ++i) {
        uint64_t *pd = (uint64_t *)(dst + (2 + i) * 4096);
        pdpt[i] = ((uint64_t)(uintptr_t)pd) | 0x03ULL;
    }

    // Map the app into one 2MiB page at MLJOS_APP_VADDR.
    uint64_t pdpt_i = (MLJOS_APP_VADDR >> 30) & 0x1FFULL;
    uint64_t pd_i = (MLJOS_APP_VADDR >> 21) & 0x1FFULL;
    if (pdpt_i < 4) {
        uint64_t *pd = (uint64_t *)(dst + (2 + pdpt_i) * 4096);
        // 2MiB page entry: present|rw|ps
        pd[pd_i] = (app_phys_2mib & 0x000FFFFFFFFFF000ULL) | 0x83ULL;
    }

    *out_cr3 = (uint64_t)(uintptr_t)dst;
}

static uint64_t alloc_app_region_2mib(void) {
    void *p = kmem_alloc(APP_REGION_SIZE, APP_REGION_SIZE);
    kmem_memset(p, 0, APP_REGION_SIZE);
    return (uint64_t)(uintptr_t)p;
}

static void init_task_common(task_t *t, const char *name) {
    t->state = TASK_RUNNABLE;
    t->name = name;
    t->entry = NULL;
    t->arg = NULL;
    t->window = NULL;
    t->console = NULL;
    t->fs_cwd = NULL;
    t->shell_active_storage = 0;
    t->shell_location = 1;
    t->shell_launch_flags = 0;
    t->shell_open_path[0] = '\0';
    kmem_memset(t->shell_jmp_env, 0, sizeof(t->shell_jmp_env));
    t->shell_jmp_ready = 0;
    kmem_memset(t->shell_history, 0, sizeof(t->shell_history));
    t->shell_history_count = 0;
    t->shell_history_pos = -1;
    t->killed = 0;
    kmem_memset(&t->ctx, 0, sizeof(t->ctx));
    kmem_memset(&t->api, 0, sizeof(t->api));
}

static void init_task_stack(task_t *t, void (*start_rip)(void)) {
    uint8_t *stack = (uint8_t *)kmem_alloc(TASK_STACK_SIZE, 16);
    uintptr_t top = (uintptr_t)stack + TASK_STACK_SIZE;
    // Fake return address so that RSP%16==8 at function entry.
    top -= 8;
    *(uint64_t *)top = 0;
    t->ctx.rsp = (uint64_t)top;
    t->ctx.rip = (uint64_t)(uintptr_t)start_rip;
}

void task_init(void) {
    kmem_init();
    g_kernel_cr3 = read_cr3();
    kmem_memset(&g_kernel_ctx, 0, sizeof(g_kernel_ctx));
    for (int i = 0; i < MAX_TASKS; ++i) {
        g_tasks[i].state = TASK_UNUSED;
    }
    g_current = NULL;
    g_rr_pos = 0;
}

task_t *task_current(void) {
    return g_current;
}

mljos_api_t *task_current_api(void) {
    if (!g_current) return NULL;
    return &g_current->api;
}

task_t *task_create_kernel(const char *name, task_entry_t entry, void *arg) {
    task_t *t = task_alloc_slot();
    if (!t) return NULL;
    init_task_common(t, name);

    uint64_t region = alloc_app_region_2mib();
    clone_page_tables(&t->ctx.cr3, region);

    t->entry = entry;
    t->arg = arg;
    init_task_stack(t, task_trampoline);
    return t;
}

task_t *task_create_app(const char *name, const void *image, uint32_t image_size) {
    if (!image || image_size == 0) return NULL;
    if (image_size > (uint32_t)APP_REGION_SIZE) return NULL;

    task_t *t = task_alloc_slot();
    if (!t) return NULL;
    init_task_common(t, name);

    uint64_t region = alloc_app_region_2mib();
    kmem_memcpy((void *)(uintptr_t)region, image, image_size);
    clone_page_tables(&t->ctx.cr3, region);

    init_task_stack(t, app_trampoline);
    return t;
}

void task_attach_window(task_t *t, struct wm_window *w) {
    if (!t) return;
    t->window = w;
}

void task_attach_console(task_t *t, console_t *c) {
    if (!t) return;
    t->console = c;
}

void task_set_paused(task_t *t, int paused) {
    if (!t) return;
    if (t->state == TASK_DEAD || t->state == TASK_UNUSED) return;
    t->state = paused ? TASK_PAUSED : TASK_RUNNABLE;
}

int task_is_alive(const task_t *t) {
    if (!t) return 0;
    return t->state == TASK_RUNNABLE || t->state == TASK_PAUSED;
}

void task_kill(task_t *t) {
    if (!t) return;
    t->killed = 1;
    if (t->state == TASK_PAUSED) t->state = TASK_RUNNABLE;
}

void task_yield(void) {
    if (!g_current) return;
    ctx_switch(&g_current->ctx, &g_kernel_ctx);
    // On resume, continue.
}

__attribute__((noreturn)) void task_exit(void) {
    if (g_current) g_current->state = TASK_DEAD;
    // Switch back to kernel.
    ctx_switch(&g_current->ctx, &g_kernel_ctx);
    for (;;) { }
}

void task_schedule_once(void) {
    // Round-robin runnable tasks.
    for (int n = 0; n < MAX_TASKS; ++n) {
        g_rr_pos = (g_rr_pos + 1) % MAX_TASKS;
        task_t *t = &g_tasks[g_rr_pos];
        if (t->state != TASK_RUNNABLE) continue;
        g_current = t;
        ctx_switch(&g_kernel_ctx, &t->ctx);
        g_current = NULL;
        // Restore kernel CR3 in case task switched away (ctx_switch restores it).
        write_cr3(g_kernel_cr3);
        return;
    }
}

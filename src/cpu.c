#include "cpu.h"
#include "console.h"
#include "task.h"
#include "sound.h"

// GDT
static gdt_entry_t g_gdt[5];
static gdt_ptr_t g_gdt_ptr;

static void gdt_set_entry(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    g_gdt[i].base_low = (base & 0xFFFF);
    g_gdt[i].base_middle = (base >> 16) & 0xFF;
    g_gdt[i].base_high = (base >> 24) & 0xFF;
    g_gdt[i].limit_low = (limit & 0xFFFF);
    g_gdt[i].granularity = (limit >> 16) & 0x0F;
    g_gdt[i].granularity |= gran & 0xF0;
    g_gdt[i].access = access;
}

static void gdt_init(void) {
    gdt_set_entry(0, 0, 0, 0, 0);                // Null
    gdt_set_entry(1, 0, 0xFFFFFFFF, 0x9A, 0xAF); // Kernel Code (64-bit)
    gdt_set_entry(2, 0, 0xFFFFFFFF, 0x92, 0xAF); // Kernel Data (64-bit)
    gdt_set_entry(3, 0, 0xFFFFFFFF, 0xFA, 0xAF); // User Code (64-bit)
    gdt_set_entry(4, 0, 0xFFFFFFFF, 0xF2, 0xAF); // User Data (64-bit)

    g_gdt_ptr.limit = sizeof(g_gdt) - 1;
    g_gdt_ptr.base = (uint64_t)&g_gdt;

    __asm__ volatile (
        "lgdt %0\n"
        "push $0x08\n"
        "lea 1f(%%rip), %%rax\n"
        "push %%rax\n"
        "lretq\n"
        "1:\n"
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        : : "m"(g_gdt_ptr) : "rax", "memory"
    );
}

// IDT
static idt_entry_t g_idt[256];
static idt_ptr_t g_idt_ptr;

extern void isr0();
extern void isr1();
extern void isr2();
extern void isr3();
extern void isr4();
extern void isr5();
extern void isr6();
extern void isr7();
extern void isr8();
extern void isr9();
extern void isr10();
extern void isr11();
extern void isr12();
extern void isr13();
extern void isr14();
extern void isr15();
extern void isr16();
extern void isr17();
extern void isr18();
extern void isr19();
extern void isr20();
extern void isr30();

static void idt_set_gate(int i, void (*handler)(void), uint8_t ist, uint8_t type_attr) {
    uint64_t addr = (uint64_t)handler;
    g_idt[i].offset_low = addr & 0xFFFF;
    g_idt[i].selector = 0x08; // Kernel Code
    g_idt[i].ist = ist;
    g_idt[i].type_attr = type_attr;
    g_idt[i].offset_middle = (addr >> 16) & 0xFFFF;
    g_idt[i].offset_high = (addr >> 32) & 0xFFFFFFFF;
    g_idt[i].reserved = 0;
}

void exception_handler(interrupt_frame_t *frame) {
    const char *exception_names[] = {
        "Division By Zero", "Debug", "Non Maskable Interrupt", "Breakpoint",
        "Into Detected Overlow", "Out of Bounds", "Invalid Opcode", "No Coprocessor",
        "Double Fault", "Coprocessor Segment Overrun", "Bad TSS", "Segment Not Present",
        "Stack Fault", "General Protection Fault", "Page Fault", "Unknown Interrupt",
        "Coprocessor Fault", "Alignment Check", "Machine Check", "SIMD Floating Point",
        "Virtualization", "Control Protection"
    };

    const char *name = (frame->int_no < 22) ? exception_names[frame->int_no] : "Unknown Exception";

    puts("\n[KERNEL PANIC] CPU Exception: ");
    puts(name);
    puts("\n");

    task_t *t = task_current();
    if (t) {
        puts("Faulting Task: ");
        puts(t->name);
        puts("\nRIP: ");
        // (Hex print would be nice, but puts is basic)
        puts("... killing task.\n");
        task_exit();
    } else {
        puts("CRITICAL: Exception in Kernel Mode! System halted.\n");
        // Error sound: low frequency continuous tone
        sound_play(220);
        for (;;) { __asm__ volatile ("hlt"); }
    }
}

// Assembly stubs
__asm__ (
    ".macro ISR_NOERR i\n"
    ".global isr\\i\n"
    "isr\\i:\n"
    "pushq $0\n"
    "pushq $\\i\n"
    "jmp isr_common\n"
    ".endm\n"

    ".macro ISR_ERR i\n"
    ".global isr\\i\n"
    "isr\\i:\n"
    "pushq $\\i\n"
    "jmp isr_common\n"
    ".endm\n"

    "ISR_NOERR 0\n"
    "ISR_NOERR 1\n"
    "ISR_NOERR 2\n"
    "ISR_NOERR 3\n"
    "ISR_NOERR 4\n"
    "ISR_NOERR 5\n"
    "ISR_NOERR 6\n"
    "ISR_NOERR 7\n"
    "ISR_ERR   8\n"
    "ISR_NOERR 9\n"
    "ISR_ERR   10\n"
    "ISR_ERR   11\n"
    "ISR_ERR   12\n"
    "ISR_ERR   13\n"
    "ISR_ERR   14\n"
    "ISR_NOERR 15\n"
    "ISR_NOERR 16\n"
    "ISR_ERR   17\n"
    "ISR_NOERR 18\n"
    "ISR_NOERR 19\n"
    "ISR_NOERR 20\n"
    "ISR_ERR   30\n"

    "isr_common:\n"
    "pushq %rax\n"
    "pushq %rbx\n"
    "pushq %rcx\n"
    "pushq %rdx\n"
    "pushq %rsi\n"
    "pushq %rdi\n"
    "pushq %rbp\n"
    "pushq %r8\n"
    "pushq %r9\n"
    "pushq %r10\n"
    "pushq %r11\n"
    "pushq %r12\n"
    "pushq %r13\n"
    "pushq %r14\n"
    "pushq %r15\n"
    "movq %rsp, %rdi\n"
    "cld\n"
    "call exception_handler\n"
    "popq %r15\n"
    "popq %r14\n"
    "popq %r13\n"
    "popq %r12\n"
    "popq %r11\n"
    "popq %r10\n"
    "popq %r9\n"
    "popq %r8\n"
    "popq %rbp\n"
    "popq %rdi\n"
    "popq %rsi\n"
    "popq %rdx\n"
    "popq %rcx\n"
    "popq %rbx\n"
    "popq %rax\n"
    "addq $16, %rsp\n"
    "iretq\n"
);

void cpu_init(void) {
    gdt_init();

    for (int i = 0; i < 256; ++i) {
        idt_set_gate(i, isr0, 0, 0x8E); // Default to isr0 for now
    }

    idt_set_gate(0, isr0, 0, 0x8E);
    idt_set_gate(1, isr1, 0, 0x8E);
    idt_set_gate(2, isr2, 0, 0x8E);
    idt_set_gate(3, isr3, 0, 0x8E);
    idt_set_gate(4, isr4, 0, 0x8E);
    idt_set_gate(5, isr5, 0, 0x8E);
    idt_set_gate(6, isr6, 0, 0x8E);
    idt_set_gate(7, isr7, 0, 0x8E);
    idt_set_gate(8, isr8, 0, 0x8E);
    idt_set_gate(9, isr9, 0, 0x8E);
    idt_set_gate(10, isr10, 0, 0x8E);
    idt_set_gate(11, isr11, 0, 0x8E);
    idt_set_gate(12, isr12, 0, 0x8E);
    idt_set_gate(13, isr13, 0, 0x8E);
    idt_set_gate(14, isr14, 0, 0x8E);
    idt_set_gate(15, isr15, 0, 0x8E);
    idt_set_gate(16, isr16, 0, 0x8E);
    idt_set_gate(17, isr17, 0, 0x8E);
    idt_set_gate(18, isr18, 0, 0x8E);
    idt_set_gate(19, isr19, 0, 0x8E);
    idt_set_gate(20, isr20, 0, 0x8E);
    idt_set_gate(30, isr30, 0, 0x8E);

    g_idt_ptr.limit = sizeof(g_idt) - 1;
    g_idt_ptr.base = (uint64_t)&g_idt;

    __asm__ volatile ("lidt %0" : : "m"(g_idt_ptr));
}

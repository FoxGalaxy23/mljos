section .multiboot
    align 8
    MB2_MAGIC       equ 0xE85250D6
    MB2_ARCH_I386   equ 0
    MB2_HEADER_LEN  equ mb2_header_end - mb2_header
    MB2_CHECKSUM    equ -(MB2_MAGIC + MB2_ARCH_I386 + MB2_HEADER_LEN)

mb2_header:
    dd MB2_MAGIC
    dd MB2_ARCH_I386
    dd MB2_HEADER_LEN
    dd MB2_CHECKSUM
    dw 0
    dw 0
    dd 8
mb2_header_end:

section .text align=8
    bits 32
    global start
    extern kernel_main

start:
    cli
    mov esp, stack_top

    ; Clear paging structures before enabling long mode.
    mov edi, pml4_table
    mov ecx, (4096 * 3) / 4
    xor eax, eax
    rep stosd

    mov eax, pdpt_table
    or eax, 0x03
    mov [pml4_table], eax

    mov eax, pd_table
    or eax, 0x03
    mov [pdpt_table], eax

    xor ecx, ecx
.map_pd:
    mov eax, ecx
    shl eax, 21
    or eax, 0x83
    mov [pd_table + ecx * 8], eax
    mov dword [pd_table + ecx * 8 + 4], 0
    inc ecx
    cmp ecx, 512
    jne .map_pd

    lgdt [gdt64_desc]

    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    mov eax, pml4_table
    mov cr3, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    mov eax, cr0
    or eax, 0x80000001
    mov cr0, eax

    jmp 0x08:long_mode_start

    bits 64
long_mode_start:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, stack_top
    xor rbp, rbp

    call kernel_main

.hlt_loop:
    hlt
    jmp .hlt_loop

section .rodata
    align 8
gdt64:
    dq 0
    dq 0x00AF9A000000FFFF
    dq 0x00AF92000000FFFF

gdt64_desc:
    dw gdt64_desc - gdt64 - 1
    dq gdt64

section .bss
    align 4096
pml4_table:
    resb 4096
pdpt_table:
    resb 4096
pd_table:
    resb 4096

    align 16
    resb 16384
stack_top:

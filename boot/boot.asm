section .multiboot2
    align 8
    header_start:
        dd 0xe85250d6                ; Magic
        dd 0                         ; Architecture (i386)
        dd header_end - header_start ; Header length
        dd 0x100000000 - (0xe85250d6 + 0 + (header_end - header_start)) ; Checksum

        ; Framebuffer tag
        align 8
        dw 5 ; type
        dw 0 ; flags
        dd 20 ; size
        dd 1024 ; width
        dd 768 ; height
        dd 32 ; depth
        
        ; End tag
        align 8
        dw 0
        dw 0
        dd 8
    header_end:

section .text
    bits 32
    global start
    extern kernel_main

start:
    cli
    mov esp, stack_top
    
    ; EBX contains the address of the multiboot info structure.
    ; We need to preserve it across the switch.
    mov esi, ebx

    ; Clear paging structures
    mov edi, pml4_table
    mov ecx, (4096 * 6) / 4
    xor eax, eax
    rep stosd

    ; Setup identity mapping for the first 4GB
    mov eax, pdpt_table
    or eax, 0x03
    mov [pml4_table], eax

    mov eax, pd_table_0
    or eax, 0x03
    mov [pdpt_table], eax
    mov eax, pd_table_1
    or eax, 0x03
    mov [pdpt_table + 8], eax
    mov eax, pd_table_2
    or eax, 0x03
    mov [pdpt_table + 16], eax
    mov eax, pd_table_3
    or eax, 0x03
    mov [pdpt_table + 24], eax

    xor ecx, ecx
.map_pd:
    mov eax, ecx
    shl eax, 21
    or eax, 0x83
    ; Map 4GB = 2048 * 2MB
    mov [pd_table_0 + ecx * 8], eax
    inc ecx
    cmp ecx, 2048
    jne .map_pd

    lgdt [gdt64_desc]

    ; Enable PAE
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; Load PML4
    mov eax, pml4_table
    mov cr3, eax

    ; Enable Long Mode in EFER MSR
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    ; Enable Paging
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

    ; ESI was EBX, so we pass it in RDI (SysV ABI)
    mov rdi, rsi
    call kernel_main

.hlt_loop:
    hlt
    jmp .hlt_loop

section .rodata
    align 8
gdt64:
    dq 0 ; Null
    dq 0x00AF9A000000FFFF ; Code
    dq 0x00AF92000000FFFF ; Data

gdt64_desc:
    dw gdt64_desc - gdt64 - 1
    dq gdt64

section .bss
    align 4096
pml4_table:
    resb 4096
pdpt_table:
    resb 4096
pd_table_0:
    resb 4096
pd_table_1:
    resb 4096
pd_table_2:
    resb 4096
pd_table_3:
    resb 4096

    align 16
    resb 16384
stack_top:

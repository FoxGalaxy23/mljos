[bits 16]
[org 0x7c00]

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov sp, 0x7c00

    ; Enable A20 (Fast A20)
    in al, 0x92
    or al, 2
    out 0x92, al

    ; Load GDT
    lgdt [gdt_desc]

    ; Enable PE
    mov eax, cr0
    or al, 1
    mov cr0, eax

    jmp 0x08:start32

[bits 32]
start32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    mov edi, 0x100000 ; destination
    mov ebx, 1        ; LBA start
    ; ecx = NUMBER OF SECTORS (will be patched by install command)
    ; We patch this exact offset with the number of sectors
    db 0xB9 ; opcode for mov ecx, imm32
patch_sectors:
    dd 0x00000000

read_loop:
    ; Wait for BSY to clear
wait_bsy:
    mov dx, 0x1F7
    in al, dx
    test al, 0x80
    jnz wait_bsy

    ; Send parameters
    mov dx, 0x1F6
    mov eax, ebx
    shr eax, 24
    and al, 0x0F
    or al, 0xE0
    out dx, al

    mov dx, 0x1F2
    mov al, 1
    out dx, al

    mov dx, 0x1F3
    mov al, bl
    out dx, al

    mov dx, 0x1F4
    mov eax, ebx
    shr eax, 8
    out dx, al

    mov dx, 0x1F5
    mov eax, ebx
    shr eax, 16
    out dx, al

    mov dx, 0x1F7
    mov al, 0x20 ; READ SECTORS
    out dx, al

wait_drq:
    mov dx, 0x1F7
    in al, dx
    test al, 0x80
    jnz wait_drq
    test al, 0x08
    jz wait_drq

    ; Read data
    mov dx, 0x1F0
    push ecx
    mov ecx, 256
    rep insw
    pop ecx

    inc ebx
    loop read_loop

    ; Jump to kernel
    jmp 0x10000C ; start symbol is right after 12 bytes of multiboot header

    hlt

align 8
gdt:
    dq 0
    ; Code
    dw 0xFFFF
    dw 0
    db 0
    db 0x9A
    db 0xCF
    db 0
    ; Data
    dw 0xFFFF
    dw 0
    db 0
    db 0x92
    db 0xCF
    db 0

gdt_desc:
    dw gdt_desc - gdt - 1
    dd gdt

times 446-($-$$) db 0
; We stop exactly at 446 bytes to not overwrite the partition table

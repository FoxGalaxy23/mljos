; boot.asm
; Компилировать с: nasm -f elf32 boot.asm -o boot.o

section .text
    ; Обязательный заголовок Multiboot
    MBOOT_PAGE_ALIGN    equ 1<<0    ; Выравнивание модулей ядра по границе страницы (4 КБ)
    MBOOT_MEM_INFO      equ 1<<1    ; GRUB должен предоставить информацию о памяти
    MBOOT_HEADER_MAGIC  equ 0x1BADB002 ; Магическое число для Multiboot
    MBOOT_HEADER_FLAGS  equ MBOOT_PAGE_ALIGN | MBOOT_MEM_INFO
    MBOOT_CHECKSUM      equ -(MBOOT_HEADER_MAGIC + MBOOT_HEADER_FLAGS)

    ; Выравнивание по 4 байта
    align 4
    mboot:
        dd MBOOT_HEADER_MAGIC
        dd MBOOT_HEADER_FLAGS
        dd MBOOT_CHECKSUM

    ; Точка входа в ядро (ядро начнет выполняться отсюда)
    global start
    extern kernel_main  ; Ссылка на функцию C

start:
    ; Запрещаем прерывания (для безопасности)
    cli

    ; Настраиваем стек
    ; Мы используем верхний адрес раздела BSS как начало стека
    mov esp, stack_top

    ; Вызываем основную функцию ядра на C
    call kernel_main

    ; Если kernel_main вернет управление, просто зацикливаемся
    hlt_loop:
        hlt
        jmp hlt_loop

section .bss
    ; Резервируем место для стека (4 килобайта)
    resb 4096
    stack_top:

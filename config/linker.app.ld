OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)
SECTIONS
{
    . = 0x40000000;

    .mljos_hdr : ALIGN(4K)
    {
        KEEP(*(.mljos_hdr))
        . = ALIGN(4K);
    }
    
    .text : ALIGN(4K)
    {
        *(.text.mljos_entry)
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)
    }
    
    .data : ALIGN(4K)
    {
        *(.data)
    }
    
    .bss : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }

    /DISCARD/ :
    {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.gcc_except_table*)
        *(.note*)
        *(.comment)
    }
}

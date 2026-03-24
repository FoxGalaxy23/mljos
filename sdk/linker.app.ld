ENTRY(_start)
SECTIONS
{
    . = 0x800000;
    
    .text : ALIGN(4K)
    {
        *(.text)
        *(.rodata)
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
}

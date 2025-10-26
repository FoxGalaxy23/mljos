/* linker.ld */
OUTPUT_FORMAT(elf32-i386)
ENTRY(start)

SECTIONS
{
  /* The multiboot header must be the first thing in the kernel image */
  . = 0x100000; /* Kernel loading address is 1MB */

  .text :
  {
    *(.mboot)
    *(.text)
  }

  .data :
  {
    *(.data)
  }

  .rodata :
  {
    *(.rodata)
  }

  .bss :
  {
    *(.bss)
  }
}

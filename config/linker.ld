/* linker.ld */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(start)

PHDRS
{
  text PT_LOAD FLAGS(5);
  rodata PT_LOAD FLAGS(4);
  data PT_LOAD FLAGS(6);
}

SECTIONS
{
  . = 0x100000;
  _kernel_start = .;

  .multiboot ALIGN(8) : ALIGN(8)
  {
    KEEP(*(.multiboot))
  } :text

  .text ALIGN(8) : ALIGN(8)
  {
    *(.text)
  } :text

  . = ALIGN(0x1000);

  .rodata ALIGN(16) : ALIGN(16)
  {
    *(.rodata)
    *(.eh_frame)
  } :rodata

  . = ALIGN(0x1000);

  .data ALIGN(16) : ALIGN(16)
  {
    *(.data)
  } :data

  .bss ALIGN(16) (NOLOAD) : ALIGN(16)
  {
    *(.bss)
  } :data

  _kernel_end = .;
}

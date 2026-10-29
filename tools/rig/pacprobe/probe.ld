ENTRY(_head)
SECTIONS {
  . = 0;
  .text : { *(.text.head) *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss : { *(.bss*) *(COMMON) }
}

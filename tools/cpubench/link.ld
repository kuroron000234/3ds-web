/* Azahar の ELF ローダーは各セグメントがページ境界（4KB）から始まることを要求する */
ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); rodata PT_LOAD FLAGS(4); data PT_LOAD FLAGS(6); }
SECTIONS {
    . = 0x00100000;
    .text : { *(.text*) } :text
    . = ALIGN(0x1000);
    .rodata : { *(.rodata*) *(.ARM.*) } :rodata
    . = ALIGN(0x1000);
    .data : { *(.data*) } :data
    .bss : { *(.bss*) *(COMMON) } :data
    . = ALIGN(0x1000);
    /DISCARD/ : { *(.comment) *(.note*) }
}

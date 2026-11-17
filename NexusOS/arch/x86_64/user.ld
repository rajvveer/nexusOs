OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)
PHDRS { code PT_LOAD FLAGS(5); ro PT_LOAD FLAGS(4); data PT_LOAD FLAGS(6); }
SECTIONS {
    . = 0x100400000;
    .text : { *(.text.start) *(.text .text.* .ltext .ltext.*) } :code
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.* .lrodata .lrodata.*) } :ro
    . = ALIGN(4096);
    .data : { *(.data .data.* .ldata .ldata.*) } :data
    .bss : { *(.bss .bss.* .lbss .lbss.*) *(COMMON) } :data
    /DISCARD/ : { *(.eh_frame*) *(.note*) *(.comment*) }
}

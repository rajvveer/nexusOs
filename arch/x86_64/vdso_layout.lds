/* Linux-style vDSO layout: one read+execute PT_LOAD at address 0 (file offset == address)
 * and a read-only PT_DYNAMIC, so the C library can use the image without relocating it. */
PHDRS
{
    text PT_LOAD FLAGS(5) FILEHDR PHDRS;
    dynamic PT_DYNAMIC FLAGS(4);
}
SECTIONS
{
    . = SIZEOF_HEADERS;
    .hash : { *(.hash) } :text
    .gnu.hash : { *(.gnu.hash) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .gnu.version : { *(.gnu.version) }
    .gnu.version_d : { *(.gnu.version_d) }
    .gnu.version_r : { *(.gnu.version_r) }
    .dynamic : { *(.dynamic) } :text :dynamic
    .rodata : { *(.rodata .rodata.*) } :text
    .data : { *(.data .data.* .bss .bss.*) }
    . = ALIGN(16);
    .text : { *(.text .text.*) }
    /DISCARD/ : { *(.note.GNU-stack) *(.eh_frame) *(.eh_frame_hdr) *(.comment) *(.note.*) }
}

/* NexusOS userland link script (Phase 57). Static i386 ET_EXEC loaded by
 * uproc_spawn_from(): every PT_LOAD must sit inside the per-process user window
 * [0x08000000, 0xC0000000). The heap (sbrk) starts at 0x10000000, the stack
 * top is 0x0C000000, so the image lives at the classic 0x08048000. */
ENTRY(_start)
SECTIONS
{
    . = 0x08048000;
    .text : { *(.text .text.* .gnu.linkonce.t.*) }
    .rodata : { *(.rodata .rodata.* .gnu.linkonce.r.*) }
    . = ALIGN(4096);
    .data : { *(.data .data.* .gnu.linkonce.d.*) }
    .bss : { *(COMMON) *(.bss .bss.*) }
    _end = .;
    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) }
}

; The /proc/kallsyms table (gen_kallsyms.py), placed in the loadable .kallsyms section (linker.ld), after .data.
; Only linked into the final kernel pass; procext.c reads it through the weak kallsyms_blob/kallsyms_blob_end symbols.
bits 64
section .kallsyms
global kallsyms_blob, kallsyms_blob_end
kallsyms_blob:
    incbin "kallsyms.bin"
kallsyms_blob_end:

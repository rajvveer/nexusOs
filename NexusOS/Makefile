# ============================================================================
# NexusOS — Build System (Makefile)
# ============================================================================
# Requires: nasm, i686-elf-gcc, i686-elf-ld, qemu-system-i386
#
# Targets:
#   make          — Build nexus.img
#   make run      — Build and run in QEMU
#   make clean    — Remove build artifacts
# ============================================================================

# --- Toolchain ---
# Try cross-compiler first, fall back to system gcc with -m32
CC      = i686-elf-gcc
LD      = i686-elf-ld
ASM     = nasm
QEMU    = qemu-system-i386

# --- Flags ---
CFLAGS  = -ffreestanding -m32 -nostdlib -nostdinc -fno-builtin \
          -fno-stack-protector -nostartfiles -nodefaultlibs \
          -Wall -Wextra -c
LDFLAGS = -T linker.ld --oformat binary -nostdlib
ASFLAGS_BIN = -f bin
ASFLAGS_ELF = -f elf32

# --- Source Files ---
C_SOURCES   = $(wildcard kernel/*.c)
ASM_SOURCES = kernel/kernel_entry.asm kernel/gdt_asm.asm kernel/idt_asm.asm \
              kernel/isr_asm.asm kernel/irq_asm.asm kernel/switch.asm

# --- Object Files ---
C_OBJECTS   = $(C_SOURCES:.c=.o)
ASM_OBJECTS = $(ASM_SOURCES:.asm=.o)

# --- Output ---
KERNEL_BIN  = kernel.bin
OS_IMAGE    = nexus.img

# ============================================================================
# Targets
# ============================================================================

all: $(OS_IMAGE)

# --- Build the final OS image ---
# Layout: [boot.bin (512B)] [boot2.bin (2048B)] [kernel.bin ...]
$(OS_IMAGE): boot/boot.bin boot/boot2.bin $(KERNEL_BIN)
	cat boot/boot.bin boot/boot2.bin $(KERNEL_BIN) > $(OS_IMAGE)
	# Pad to 1.44MB floppy size
	truncate -s 1474560 $(OS_IMAGE)
	@echo ""
	@echo "========================================"
	@echo " NexusOS image built: $(OS_IMAGE)"
	@echo " Size: $$(wc -c < $(OS_IMAGE)) bytes"
	@echo "========================================"

# --- Compile Stage 1 bootloader ---
boot/boot.bin: boot/boot.asm
	$(ASM) $(ASFLAGS_BIN) $< -o $@

# --- Compile Stage 2 bootloader ---
boot/boot2.bin: boot/boot2.asm
	$(ASM) $(ASFLAGS_BIN) $< -o $@

# --- Link kernel ---
$(KERNEL_BIN): $(ASM_OBJECTS) $(C_OBJECTS)
	$(LD) $(LDFLAGS) -Map kernel.map -o $@ $^

# --- Compile C source files ---
kernel/%.o: kernel/%.c
	$(CC) $(CFLAGS) $< -o $@

# --- Compile ASM source files (ELF format for linking) ---
kernel/%.o: kernel/%.asm
	$(ASM) $(ASFLAGS_ELF) $< -o $@

# --- Run in QEMU ---
run: $(OS_IMAGE)
	$(QEMU) -fda $(OS_IMAGE)

# --- Run with debug output ---
debug: $(OS_IMAGE)
	$(QEMU) -fda $(OS_IMAGE) -d int -no-reboot -no-shutdown

# --- Clean build artifacts ---
clean:
	rm -f kernel/*.o boot/*.bin $(KERNEL_BIN) $(OS_IMAGE)

.PHONY: all run debug clean

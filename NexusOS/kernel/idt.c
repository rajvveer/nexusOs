/* ============================================================================
 * NexusOS — IDT (Implementation)
 * ============================================================================ */

#include "idt.h"
#include "vga.h"
#include "string.h"
#include "port.h"

#define IDT_ENTRIES 256

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr   idtp;

/* Interrupt handler function pointers */
static isr_handler_t interrupt_handlers[IDT_ENTRIES];

/* External ISR stubs (defined in isr_asm.asm) */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

/* External IRQ stubs (defined in irq_asm.asm) */
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

/* -------------------------------------------------------------------------- */
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low  = base & 0xFFFF;
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].selector  = sel;
    idt[num].always0   = 0;
    idt[num].flags     = flags;
}

/* -------------------------------------------------------------------------- */
void register_interrupt_handler(uint8_t n, isr_handler_t handler) {
    interrupt_handlers[n] = handler;
}

/* --------------------------------------------------------------------------
 * idt_init: Install all ISR and IRQ gates, then load IDT
 * -------------------------------------------------------------------------- */
void idt_init(void) {
    idtp.limit = (sizeof(struct idt_entry) * IDT_ENTRIES) - 1;
    idtp.base  = (uint32_t)&idt;

    memset(&idt, 0, sizeof(struct idt_entry) * IDT_ENTRIES);
    memset(&interrupt_handlers, 0, sizeof(isr_handler_t) * IDT_ENTRIES);

    /* CPU Exception ISRs (0-31) */
    idt_set_gate(0,  (uint32_t)isr0,  0x08, 0x8E);
    idt_set_gate(1,  (uint32_t)isr1,  0x08, 0x8E);
    idt_set_gate(2,  (uint32_t)isr2,  0x08, 0x8E);
    idt_set_gate(3,  (uint32_t)isr3,  0x08, 0x8E);
    idt_set_gate(4,  (uint32_t)isr4,  0x08, 0x8E);
    idt_set_gate(5,  (uint32_t)isr5,  0x08, 0x8E);
    idt_set_gate(6,  (uint32_t)isr6,  0x08, 0x8E);
    idt_set_gate(7,  (uint32_t)isr7,  0x08, 0x8E);
    idt_set_gate(8,  (uint32_t)isr8,  0x08, 0x8E);
    idt_set_gate(9,  (uint32_t)isr9,  0x08, 0x8E);
    idt_set_gate(10, (uint32_t)isr10, 0x08, 0x8E);
    idt_set_gate(11, (uint32_t)isr11, 0x08, 0x8E);
    idt_set_gate(12, (uint32_t)isr12, 0x08, 0x8E);
    idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E);
    idt_set_gate(14, (uint32_t)isr14, 0x08, 0x8E);
    idt_set_gate(15, (uint32_t)isr15, 0x08, 0x8E);
    idt_set_gate(16, (uint32_t)isr16, 0x08, 0x8E);
    idt_set_gate(17, (uint32_t)isr17, 0x08, 0x8E);
    idt_set_gate(18, (uint32_t)isr18, 0x08, 0x8E);
    idt_set_gate(19, (uint32_t)isr19, 0x08, 0x8E);
    idt_set_gate(20, (uint32_t)isr20, 0x08, 0x8E);
    idt_set_gate(21, (uint32_t)isr21, 0x08, 0x8E);
    idt_set_gate(22, (uint32_t)isr22, 0x08, 0x8E);
    idt_set_gate(23, (uint32_t)isr23, 0x08, 0x8E);
    idt_set_gate(24, (uint32_t)isr24, 0x08, 0x8E);
    idt_set_gate(25, (uint32_t)isr25, 0x08, 0x8E);
    idt_set_gate(26, (uint32_t)isr26, 0x08, 0x8E);
    idt_set_gate(27, (uint32_t)isr27, 0x08, 0x8E);
    idt_set_gate(28, (uint32_t)isr28, 0x08, 0x8E);
    idt_set_gate(29, (uint32_t)isr29, 0x08, 0x8E);
    idt_set_gate(30, (uint32_t)isr30, 0x08, 0x8E);
    idt_set_gate(31, (uint32_t)isr31, 0x08, 0x8E);

    /* Hardware IRQs (32-47) */
    idt_set_gate(32, (uint32_t)irq0,  0x08, 0x8E);
    idt_set_gate(33, (uint32_t)irq1,  0x08, 0x8E);
    idt_set_gate(34, (uint32_t)irq2,  0x08, 0x8E);
    idt_set_gate(35, (uint32_t)irq3,  0x08, 0x8E);
    idt_set_gate(36, (uint32_t)irq4,  0x08, 0x8E);
    idt_set_gate(37, (uint32_t)irq5,  0x08, 0x8E);
    idt_set_gate(38, (uint32_t)irq6,  0x08, 0x8E);
    idt_set_gate(39, (uint32_t)irq7,  0x08, 0x8E);
    idt_set_gate(40, (uint32_t)irq8,  0x08, 0x8E);
    idt_set_gate(41, (uint32_t)irq9,  0x08, 0x8E);
    idt_set_gate(42, (uint32_t)irq10, 0x08, 0x8E);
    idt_set_gate(43, (uint32_t)irq11, 0x08, 0x8E);
    idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E);
    idt_set_gate(45, (uint32_t)irq13, 0x08, 0x8E);
    idt_set_gate(46, (uint32_t)irq14, 0x08, 0x8E);
    idt_set_gate(47, (uint32_t)irq15, 0x08, 0x8E);

    /* Load the IDT */
    idt_load((uint32_t)&idtp);

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("IDT initialized (256 entries)\n");
}

/* --------------------------------------------------------------------------
 * isr_handler: Called from assembly for CPU exceptions (ISR 0-31)
 * -------------------------------------------------------------------------- */
static const char* exception_messages[] = {
    "Division By Zero", "Debug", "Non Maskable Interrupt", "Breakpoint",
    "Overflow", "Bound Range Exceeded", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS", "Segment Not Present",
    "Stack-Segment Fault", "General Protection Fault", "Page Fault", "Reserved",
    "x87 Floating-Point", "Alignment Check", "Machine Check", "SIMD Floating-Point",
    "Virtualization", "Control Protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection", "VMM Communication", "Security Exception", "Reserved"
};

void isr_handler(struct registers* regs) {
    if (interrupt_handlers[regs->int_no] != NULL) {
        interrupt_handlers[regs->int_no](regs);
    } else if (regs->int_no < 32) {
        /* ---- Serial debug output to COM1 ---- */
        char hex[16] = "0123456789ABCDEF";
        #define SERIAL_CHAR(c) do { while ((port_byte_in(0x3FD) & 0x20) == 0); port_byte_out(0x3F8, (c)); } while(0)
        #define SERIAL_HEX(v) do { for (int _i=28;_i>=0;_i-=4) SERIAL_CHAR(hex[((v)>>_i)&0xF]); } while(0)
        SERIAL_CHAR('!'); SERIAL_CHAR('E'); SERIAL_CHAR('X'); SERIAL_CHAR(':');
        SERIAL_HEX(regs->int_no); SERIAL_CHAR(' ');
        SERIAL_HEX(regs->err_code); SERIAL_CHAR(' ');
        SERIAL_HEX(regs->eip); SERIAL_CHAR('\n');

        /* ---- VESA Framebuffer BSOD ---- */
        /* Write directly to the hardware framebuffer at the stored address */
        uint32_t fb_addr = *(volatile uint32_t*)0x9000;   /* FB address from boot2 */
        uint8_t  vesa_on = *(volatile uint8_t*)0x900A;    /* VESA active flag */

        if (vesa_on && fb_addr) {
            volatile uint32_t* fb = (volatile uint32_t*)fb_addr;
            int pitch_px = 1024;  /* 1024 pixels wide */

            /* Fill a red bar at top of screen: 1024 x 40 pixels */
            for (int y = 0; y < 40; y++)
                for (int x = 0; x < 1024; x++)
                    fb[y * pitch_px + x] = 0x00CC0000; /* Dark red */

            /* Draw exception number as 2-digit hex in white at (10, 8) */
            /* Simple 5x7 pixel digit patterns */
            static const uint8_t font_hex[16][7] = {
                {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* 0 */
                {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
                {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}, /* 2 */
                {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}, /* 3 */
                {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 4 */
                {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, /* 5 */
                {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /* 6 */
                {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
                {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 8 */
                {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, /* 9 */
                {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /* A */
                {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* B */
                {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* C */
                {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, /* D */
                {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
                {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, /* F */
            };

            /* Helper: draw one hex digit at (px, py) scaled 3x */
            #define DRAW_HEX_DIGIT(digit, px, py) do { \
                int _d = (digit) & 0xF; \
                for (int _r = 0; _r < 7; _r++) \
                    for (int _c = 0; _c < 5; _c++) \
                        if (font_hex[_d][_r] & (0x10 >> _c)) \
                            for (int _sy = 0; _sy < 3; _sy++) \
                                for (int _sx = 0; _sx < 3; _sx++) \
                                    fb[((py)+_r*3+_sy) * pitch_px + (px)+_c*3+_sx] = 0x00FFFFFF; \
            } while(0)

            /* "CRASH INT:" at x=10 */
            /* Draw "INT xx ERR xxxxxxxx EIP xxxxxxxx" as hex digits */
            int xp = 10;
            /* INT: label - just draw the int number */
            DRAW_HEX_DIGIT(regs->int_no >> 4, xp, 6);
            xp += 18;
            DRAW_HEX_DIGIT(regs->int_no, xp, 6);
            xp += 30;

            /* EIP: 8 hex digits */
            for (int i = 7; i >= 0; i--) {
                DRAW_HEX_DIGIT((regs->eip >> (i*4)), xp, 6);
                xp += 18;
            }
            xp += 12;

            /* ERR: 8 hex digits */
            for (int i = 7; i >= 0; i--) {
                DRAW_HEX_DIGIT((regs->err_code >> (i*4)), xp, 6);
                xp += 18;
            }
            #undef DRAW_HEX_DIGIT
        }

        /* Halt on unhandled exception */
        __asm__ volatile("cli; hlt");
    }
}

/* --------------------------------------------------------------------------
 * irq_handler: Called from assembly for hardware IRQs (32-47)
 * -------------------------------------------------------------------------- */
void irq_handler(struct registers* regs) {
    /* Send EOI (End Of Interrupt) to PIC */
    if (regs->int_no >= 40) {
        /* Send EOI to slave PIC (PIC2) */
        port_byte_out(0xA0, 0x20);
    }
    /* Always send EOI to master PIC (PIC1) */
    port_byte_out(0x20, 0x20);

    /* Call registered handler if any */
    if (interrupt_handlers[regs->int_no] != NULL) {
        interrupt_handlers[regs->int_no](regs);
    }
}

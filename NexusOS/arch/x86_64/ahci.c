/* AHCI 1.x SATA driver: BIOS/OS handoff, HBA reset, port spin-up and link detection, command
 * list / received FIS / PRDT, IDENTIFY, READ/WRITE DMA (EXT), FLUSH CACHE (EXT), COMRESET error
 * recovery. One command slot per port: requests are synchronous under the big kernel lock.
 * Completion: MSI/MSI-X or PCI INTx (block_pci_irq) with polling as the fallback; BAR via block_pci_bar.
 * Bare-metal: link power management off (PxSCTL.IPM = 3) on every port before spin-up; links are given up to
 * 2 s and accepted once no port's PxSSTS.DET changed for 100 ms (libata's normal debounce) instead of a fixed
 * 20 ms (OOB on HDDs and some AMD/ASMedia ports takes longer); a SATA controller left in RAID mode (class
 * 01/04, e.g. AMD RAIDXpert) is reported with the fix. tests/test_ahci_sim.py simulates all of it. */
#include "native.h"
#include "power.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"
#include "../../kernel/pci.h"

#define HBA_CAP 0x00
#define HBA_GHC 0x04
#define HBA_IS 0x08
#define HBA_PI 0x0C
#define HBA_VS 0x10
#define HBA_CAP2 0x24
#define HBA_BOHC 0x28
#define GHC_HR 0x1u
#define GHC_IE 0x2u
#define GHC_AE 0x80000000u
#define CAP_SSS (1u << 27)
#define CAP_S64A (1u << 31)
#define CAP2_BOH 0x1u
#define BOHC_BOS 0x1u
#define BOHC_OOS 0x2u
#define BOHC_BB 0x10u
#define PX_CLB 0x00
#define PX_CLBU 0x04
#define PX_FB 0x08
#define PX_FBU 0x0C
#define PX_IS 0x10
#define PX_IE 0x14
#define PX_CMD 0x18
#define PX_TFD 0x20
#define PX_SIG 0x24
#define PX_SSTS 0x28
#define PX_SCTL 0x2C
#define PX_SERR 0x30
#define PX_CI 0x38
#define CMD_ST 0x1u
#define CMD_SUD 0x2u
#define CMD_POD 0x4u
#define CMD_FRE 0x10u
#define CMD_FR 0x4000u
#define CMD_CR 0x8000u
#define IS_FATAL ((1u << 30) | (1u << 29) | (1u << 28) | (1u << 27)) /* task file, host bus fatal/data, interface */
#define IE_MASK (0x2Fu | IS_FATAL)  /* D2H, PIO setup, DMA setup, set-device-bits, descriptor processed, errors */
#define TFD_ERR 0x01u
#define TFD_DRQ 0x08u
#define TFD_DF 0x20u
#define TFD_BSY 0x80u
#define SIG_ATA 0x00000101u
#define SIG_ATAPI 0xEB140101u
#define MAX_HBAS 4
#define MAX_PORTS 16
#define PRDT_MAX 8
#define PRD_BYTES (4u * 1024 * 1024)
#define COMMAND_BYTES (4u * 1024 * 1024)   /* largest transfer of one command */
#define RW_TIMEOUT_MS 30000
#define FLUSH_TIMEOUT_MS 60000

struct ahci_hba { volatile uint8_t* abar; uint32_t cap; unsigned index, irq; };
struct ahci_port {
    struct block_device dev;
    struct ahci_hba* hba; volatile uint8_t* regs; unsigned number;
    uint8_t* dma;               /* one page: command list (0), received FIS (1024), command table (2048) */
    volatile uint32_t irq_status;
    bool lba48, flush_ext, flush;
};
static struct ahci_hba hbas[MAX_HBAS];
static struct ahci_port* ports[MAX_PORTS];
static unsigned hba_total, port_total;

#ifndef AHCI_TEST_MMIO   /* tests/ahci_sim_test.c supplies rd/wr over a simulated HBA */
static inline uint32_t rd(volatile uint8_t* base, unsigned offset) { return *(volatile uint32_t*)(base + offset); }
static inline void wr(volatile uint8_t* base, unsigned offset, uint32_t value) { *(volatile uint32_t*)(base + offset) = value; }
#endif
static bool wait_reg(volatile uint8_t* base, unsigned offset, uint32_t mask, uint32_t value, unsigned ms) {
    uint64_t deadline = native_clock_ns() + ms * 1000000ULL;
    for (;;) {
        if ((rd(base, offset) & mask) == value) return true;
        if (native_clock_ns() > deadline) return (rd(base, offset) & mask) == value;
        __asm__ volatile("pause");
    }
}
static void delay_ms(unsigned ms) { uint64_t end = native_clock_ns() + ms * 1000000ULL; while (native_clock_ns() < end) __asm__ volatile("pause"); }
static void print_hex32(uint32_t value) { char text[11] = "0x"; for (int i = 0; i < 8; i++) text[2 + i] = "0123456789ABCDEF"[(value >> (28 - 4 * i)) & 15]; text[10] = 0; vga_print(text); }
static void port_message(const struct ahci_port* port, const char* text) { vga_print("[AHCI] "); vga_print(port->dev.name); vga_print(": "); vga_print(text); }

static bool port_stop(volatile uint8_t* p) {
    wr(p, PX_CMD, rd(p, PX_CMD) & ~CMD_ST);
    if (!wait_reg(p, PX_CMD, CMD_CR, 0, 500)) return false;
    wr(p, PX_CMD, rd(p, PX_CMD) & ~CMD_FRE);
    return wait_reg(p, PX_CMD, CMD_FR, 0, 500);
}
static bool port_start(volatile uint8_t* p) {
    if (!wait_reg(p, PX_CMD, CMD_CR, 0, 500)) return false;
    wr(p, PX_CMD, rd(p, PX_CMD) | CMD_FRE);
    wr(p, PX_CMD, rd(p, PX_CMD) | CMD_ST);
    return true;
}
/* COMRESET (PxSCTL.DET 1 -> 0) with partial/slumber disabled; the port must be stopped. */
static bool port_comreset(volatile uint8_t* p) {
    uint32_t control = rd(p, PX_SCTL) & ~0xFFFu;
    wr(p, PX_SCTL, control | 0x301);
    delay_ms(2);
    wr(p, PX_SCTL, control | 0x300);
    bool link = wait_reg(p, PX_SSTS, 0xF, 3, 1000);
    wr(p, PX_SERR, ~0u);
    return link;
}
static bool port_ready(volatile uint8_t* p, unsigned ms) { return wait_reg(p, PX_TFD, TFD_BSY | TFD_DRQ, 0, ms); }
/* Non-queued error recovery (AHCI 1.3 6.2.2): stop, reset the link, clear errors, restart. */
static bool port_recover(struct ahci_port* port) {
    volatile uint8_t* p = port->regs;
    port->dev.stats.resets++;
    if (!port_stop(p)) port_message(port, "command engine did not stop; forcing COMRESET\n");
    bool link = port_comreset(p);
    wr(p, PX_IS, ~0u); wr(port->hba->abar, HBA_IS, 1u << port->number); port->irq_status = 0;
    wr(p, PX_CMD, rd(p, PX_CMD) | CMD_FRE);
    bool ready = link && port_ready(p, 10000) && port_start(p);
    port_message(port, ready ? "port reset, link up\n" : "port reset failed\n");
    return ready;
}

/* Issue one command in slot 0 and wait for it. 0 ok, -1 failed (port needs recovery). */
static int port_exec(struct ahci_port* port, uint8_t command, uint64_t lba, uint32_t count, uint64_t address, uint32_t bytes, bool write, unsigned timeout_ms) {
    volatile uint8_t* p = port->regs;
    uint32_t* header = (uint32_t*)port->dma;
    uint8_t* table = port->dma + 2048;
    memset(table, 0, 0x80 + PRDT_MAX * 16);
    table[0] = 0x27; table[1] = 0x80; table[2] = command;
    bool addressed = command == 0x25 || command == 0x35 || command == 0xC8 || command == 0xCA;
    if (addressed) {
        table[4] = (uint8_t)lba; table[5] = (uint8_t)(lba >> 8); table[6] = (uint8_t)(lba >> 16);
        table[7] = port->lba48 ? 0x40 : (uint8_t)(0x40 | ((lba >> 24) & 15));
        if (port->lba48) { table[8] = (uint8_t)(lba >> 24); table[9] = (uint8_t)(lba >> 32); table[10] = (uint8_t)(lba >> 40); }
        table[12] = (uint8_t)count; table[13] = (uint8_t)(count >> 8);
    }
    unsigned prds = 0;
    for (uint32_t done = 0; done < bytes; prds++) {
        uint32_t n = bytes - done; if (n > PRD_BYTES) n = PRD_BYTES;
        if (prds == PRDT_MAX) return -1;
        uint32_t* prd = (uint32_t*)(table + 0x80 + prds * 16);
        uint64_t at = address + done;
        prd[0] = (uint32_t)at; prd[1] = (uint32_t)(at >> 32); prd[3] = n - 1;
        done += n;
    }
    header[0] = 5u | (write ? 1u << 6 : 0) | ((uint32_t)prds << 16);
    header[1] = 0;
    uint64_t table_physical = virt_to_phys(table);
    header[2] = (uint32_t)table_physical; header[3] = (uint32_t)(table_physical >> 32);
    header[4] = header[5] = header[6] = header[7] = 0;
    if (!port_ready(p, 1000)) { port_message(port, "device busy before command\n"); return -1; }
    port->irq_status = 0; wr(p, PX_IS, ~0u);
    __asm__ volatile("mfence" ::: "memory");
    uint64_t start = block_ticks(), deadline = native_clock_ns() + timeout_ms * 1000000ULL;
    wr(p, PX_CI, 1);
    int result = 0;
    for (;;) {
        uint32_t status = port->irq_status | rd(p, PX_IS);
        if (status & IS_FATAL) { result = -1; break; }
        if (!(rd(p, PX_CI) & 1)) break;
        if (native_clock_ns() > deadline) { result = -2; break; }
        block_wait(port->dev.irq);
    }
    port->dev.stats.busy_ticks += block_ticks() - start;
    uint32_t status = port->irq_status | rd(p, PX_IS), task = rd(p, PX_TFD);
    wr(p, PX_IS, status); wr(port->hba->abar, HBA_IS, 1u << port->number); port->irq_status = 0;
    if (!result && (task & (TFD_ERR | TFD_DF | TFD_BSY | TFD_DRQ))) result = -1;
    if (result) {
        port->dev.stats.errors++;
        port_message(port, result == -2 ? "command timeout cmd=" : "command error cmd="); print_hex32(command);
        vga_print(" is="); print_hex32(status); vga_print(" tfd="); print_hex32(task); vga_print(" serr="); print_hex32(rd(p, PX_SERR)); vga_print("\n");
        return -1;
    }
    __asm__ volatile("" ::: "memory");
    return 0;
}
static int port_command(struct ahci_port* port, uint8_t command, uint64_t lba, uint32_t count, uint64_t address, uint32_t bytes, bool write, unsigned timeout_ms) {
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        if (!port_exec(port, command, lba, count, address, bytes, write, timeout_ms)) return 0;
        if (!port_recover(port)) return -1;
    }
    return -1;
}

static int ahci_transfer(struct block_device* dev, uint64_t lba, uint32_t count, uint8_t* data, bool write) {
    struct ahci_port* port = dev->driver;
    uint32_t bs = dev->block_size;
    uint64_t bounce_bytes; uint8_t* bounce = block_bounce(&bounce_bytes);
    bool direct = block_dma_direct(data, (uint64_t)count * bs, port->hba->cap & CAP_S64A ? ~0ULL : 0x100000000ULL);
    uint32_t limit = dev->max_blocks;
    if (!direct && bounce_bytes / bs < limit) limit = (uint32_t)(bounce_bytes / bs);
    if (!port->lba48 && (lba > 0x0FFFFFFF || count > 0x10000000 - lba)) return -1;
    while (count) {
        uint32_t n = count < limit ? count : limit, bytes = n * bs;
        uint8_t* target = direct ? data : bounce;
        if (write && !direct) memcpy(bounce, data, bytes);
        uint8_t command = write ? (port->lba48 ? 0x35 : 0xCA) : (port->lba48 ? 0x25 : 0xC8);
        if (port_command(port, command, lba, n, virt_to_phys(target), bytes, write, RW_TIMEOUT_MS) < 0) return -1;
        if (!write && !direct) memcpy(data, bounce, bytes);
        if (write) { dev->stats.write_cmds++; dev->stats.write_sectors += bytes / 512; }
        else { dev->stats.read_cmds++; dev->stats.read_sectors += bytes / 512; }
        lba += n; count -= n; data += bytes;
    }
    return 0;
}
static int ahci_read(struct block_device* dev, uint64_t lba, uint32_t count, void* buffer) { return ahci_transfer(dev, lba, count, buffer, false); }
static int ahci_write(struct block_device* dev, uint64_t lba, uint32_t count, const void* buffer) { return ahci_transfer(dev, lba, count, (uint8_t*)buffer, true); }
static int ahci_flush(struct block_device* dev) {
    struct ahci_port* port = dev->driver;
    if (!port->flush_ext && !port->flush) return 0; /* no volatile cache to flush */
    dev->stats.flushes++;
    return port_command(port, port->flush_ext ? 0xEA : 0xE7, 0, 0, 0, 0, false, FLUSH_TIMEOUT_MS);
}
static const struct block_ops ahci_ops = {ahci_read, ahci_write, ahci_flush};

static void ahci_irq(void* context) {
    struct ahci_hba* hba = context;
    uint32_t pending = rd(hba->abar, HBA_IS);
    if (!pending || pending == ~0u) return;
    for (unsigned i = 0; i < port_total; i++) {
        struct ahci_port* port = ports[i];
        if (port->hba != hba || !(pending & (1u << port->number))) continue;
        uint32_t status = rd(port->regs, PX_IS);
        wr(port->regs, PX_IS, status);   /* port status first, then the HBA summary bit */
        port->irq_status |= status; port->dev.stats.irqs++;
    }
    wr(hba->abar, HBA_IS, pending);
}

static void identify_string(const uint16_t* words, unsigned first, unsigned count, char* out) {
    for (unsigned i = 0; i < count; i++) { out[2 * i] = (char)(words[first + i] >> 8); out[2 * i + 1] = (char)words[first + i]; }
    out[2 * count] = 0;
    for (unsigned i = 2 * count; i && (out[i - 1] == ' ' || !out[i - 1]); i--) out[i - 1] = 0;
}
static void port_probe(struct ahci_hba* hba, unsigned number) {
    volatile uint8_t* p = hba->abar + 0x100 + number * 0x80;
    /* Link: DET 3 = device present and PHY communication; 1 = present without link -> COMRESET. */
    uint32_t det = rd(p, PX_SSTS) & 0xF;
    if (det == 1 && port_comreset(p)) det = 3;
    if (det != 3) return;
    uint64_t pages = dma32_alloc(2 * 4096);
    if (!pages) { vga_print("[AHCI] no memory for port\n"); return; }
    uint8_t* dma = (uint8_t*)(uintptr_t)pages;
    struct ahci_port* port = (struct ahci_port*)(uintptr_t)(pages + 4096);
    port->hba = hba; port->regs = p; port->number = number; port->dma = dma;
    block_name_number(port->dev.name, "sata", port_total, NULL);
    uint64_t dma_physical = virt_to_phys(dma);
    wr(p, PX_CLB, (uint32_t)dma_physical); wr(p, PX_CLBU, (uint32_t)(dma_physical >> 32));
    wr(p, PX_FB, (uint32_t)(dma_physical + 1024)); wr(p, PX_FBU, (uint32_t)((dma_physical + 1024) >> 32));
    wr(p, PX_SERR, ~0u); wr(p, PX_IS, ~0u);
    wr(p, PX_CMD, rd(p, PX_CMD) | CMD_FRE);
    if (!port_ready(p, 10000)) {
        port_stop(p);
        if (!port_comreset(p)) goto release;
        wr(p, PX_CMD, rd(p, PX_CMD) | CMD_FRE);
        if (!port_ready(p, 10000)) goto release;
    }
    uint32_t signature = rd(p, PX_SIG);
    if (signature != SIG_ATA) {
        vga_print("[64] AHCI"); char digits[12]; uint_to_str(hba->index, digits); vga_print(digits); vga_print(" port ");
        uint_to_str(number, digits); vga_print(digits);
        vga_print(signature == SIG_ATAPI ? ": ATAPI device ignored\n" : ": non-disk device ignored sig="); if (signature != SIG_ATAPI) { print_hex32(signature); vga_print("\n"); }
        goto release;
    }
    if (!port_start(p)) goto release;
    uint64_t bounce_bytes; uint8_t* identify = block_bounce(&bounce_bytes);
    if (!identify || port_command(port, 0xEC, 0, 0, virt_to_phys(identify), 512, false, 5000) < 0) { port_message(port, "IDENTIFY failed\n"); goto release; }
    const uint16_t* w = (const uint16_t*)identify;
    bool valid83 = (w[83] & 0xC000) == 0x4000;
    port->lba48 = valid83 && (w[83] & (1u << 10));
    port->flush_ext = valid83 && port->lba48 && (w[83] & (1u << 13));
    port->flush = valid83 && (w[83] & (1u << 12));
    uint64_t sectors = port->lba48 ? (uint64_t)w[100] | (uint64_t)w[101] << 16 | (uint64_t)w[102] << 32 | (uint64_t)w[103] << 48
                                   : (uint64_t)w[60] | (uint64_t)w[61] << 16;
    uint32_t sector_size = 512;
    if ((w[106] & 0xC000) == 0x4000 && (w[106] & (1u << 12))) sector_size = ((uint32_t)w[117] | (uint32_t)w[118] << 16) * 2;
    identify_string(w, 27, 20, port->dev.model);
    if (!sectors || (sector_size != 512 && sector_size != 4096)) { port_message(port, "unsupported geometry\n"); goto release; }
    port->dev.kind = "AHCI"; port->dev.block_size = sector_size; port->dev.blocks = sectors;
    port->dev.max_blocks = port->lba48 ? COMMAND_BYTES / sector_size : 256;
    port->dev.ops = &ahci_ops; port->dev.driver = port;
    wr(p, PX_IE, IE_MASK);
    ports[port_total++] = port;
    return;
release:
    port_stop(p);
    wr(p, PX_CLB, 0); wr(p, PX_CLBU, 0); wr(p, PX_FB, 0); wr(p, PX_FBU, 0);
    arena_free(pages, 2 * 4096);
}
/* System sleep (power.h): the ports stop; after the power loss the HBA is in AHCI mode again and every port is
 * spun up and programmed as at probe (command list, received-FIS area, link, interrupt enables). */
static int ahci_pm_suspend(void* context, unsigned state) {
    struct ahci_hba* hba = context; (void)state;
    wr(hba->abar, HBA_GHC, rd(hba->abar, HBA_GHC) & ~GHC_IE);
    for (unsigned i = 0; i < port_total; i++) if (ports[i]->hba == hba) port_stop(ports[i]->regs);
    return 0;
}
static void ahci_pm_resume(void* context, unsigned state) {
    struct ahci_hba* hba = context; (void)state;
    if (rd(hba->abar, HBA_CAP2) & CAP2_BOH) { wr(hba->abar, HBA_BOHC, rd(hba->abar, HBA_BOHC) | BOHC_OOS); wait_reg(hba->abar, HBA_BOHC, BOHC_BOS, 0, 2000); }
    wr(hba->abar, HBA_GHC, GHC_AE);
    for (unsigned i = 0; i < port_total; i++) {
        struct ahci_port* port = ports[i]; volatile uint8_t* p = port->regs;
        if (port->hba != hba) continue;
        port_stop(p);
        wr(p, PX_SCTL, (rd(p, PX_SCTL) & ~0xF0Fu) | 0x300u);       /* IPM: no partial/slumber, as at probe (hw) */
        wr(p, PX_CMD, rd(p, PX_CMD) | CMD_SUD | CMD_POD);
        wait_reg(p, PX_SSTS, 0xF, 3, 20);
        uint64_t physical = virt_to_phys(port->dma);
        wr(p, PX_CLB, (uint32_t)physical); wr(p, PX_CLBU, (uint32_t)(physical >> 32));
        wr(p, PX_FB, (uint32_t)(physical + 1024)); wr(p, PX_FBU, (uint32_t)((physical + 1024) >> 32));
        wr(p, PX_SERR, ~0u); wr(p, PX_IS, ~0u); port->irq_status = 0;
        wr(p, PX_CMD, rd(p, PX_CMD) | CMD_FRE);
        if (!port_ready(p, 10000)) { port_stop(p); port_comreset(p); wr(p, PX_CMD, rd(p, PX_CMD) | CMD_FRE); port_ready(p, 10000); }
        if (!port_start(p)) port_message(port, "did not restart after resume\n");
        wr(p, PX_IE, IE_MASK);
    }
    wr(hba->abar, HBA_IS, ~0u);
    wr(hba->abar, HBA_GHC, GHC_AE | (hba->irq ? GHC_IE : 0));
}
static struct pm_ops ahci_pm[MAX_HBAS];
static void hba_probe(pci_device_t* pci) {
    if (hba_total == MAX_HBAS) return;
    char digits[12]; uint_to_str(hba_total, digits);
    volatile uint8_t* abar = block_pci_bar(pci, 5, 0x1100);
    if (!abar) { vga_print("[64] AHCI ABAR missing or cannot be mapped\n"); return; }
    struct ahci_hba* hba = &hbas[hba_total];
    hba->abar = abar; hba->index = hba_total;
    uint16_t command = pci_read16(pci->bus, pci->device, pci->function, PCI_COMMAND);
    /* Memory space + bus master, INTx enabled; the upper (status) half is write-1-to-clear. */
    pci_write32(pci->bus, pci->device, pci->function, PCI_COMMAND, (uint32_t)((command | 6u) & ~0x400u));
    if (rd(abar, HBA_CAP2) & CAP2_BOH) {
        wr(abar, HBA_BOHC, rd(abar, HBA_BOHC) | BOHC_OOS);
        if (!wait_reg(abar, HBA_BOHC, BOHC_BOS, 0, 25) && (rd(abar, HBA_BOHC) & BOHC_BB)) wait_reg(abar, HBA_BOHC, BOHC_BOS, 0, 2000);
        vga_print((rd(abar, HBA_BOHC) & BOHC_BOS) ? "[64] AHCI BIOS kept ownership; taking over\n" : "[64] AHCI BIOS/OS handoff complete\n");
    }
    wr(abar, HBA_GHC, rd(abar, HBA_GHC) | GHC_AE);
    wr(abar, HBA_GHC, rd(abar, HBA_GHC) | GHC_HR);
    if (!wait_reg(abar, HBA_GHC, GHC_HR, 0, 1000)) { vga_print("[64] AHCI HBA reset timeout\n"); return; }
    wr(abar, HBA_GHC, GHC_AE);
    hba->cap = rd(abar, HBA_CAP);
    uint32_t implemented = rd(abar, HBA_PI), version = rd(abar, HBA_VS);
    hba_total++;
    vga_print("[64] AHCI"); vga_print(digits); vga_print(" version="); print_hex32(version);
    vga_print(" ports="); print_hex32(implemented); vga_print(" slots="); uint_to_str(((hba->cap >> 8) & 31) + 1, digits); vga_print(digits);
    vga_print(hba->cap & (1u << 31) ? " 64-bit" : " 32-bit"); vga_print("\n");
    /* Stop every port, switch link power management off and spin it up; then wait for the links to settle. */
    uint32_t usable = 0;
    for (unsigned i = 0; i < 32; i++) {
        volatile uint8_t* p = abar + 0x100 + i * 0x80;
        if (!(implemented & (1u << i)) || !port_stop(p)) continue;
        wr(p, PX_SCTL, (rd(p, PX_SCTL) & ~0xF0Fu) | 0x300u);       /* IPM: no partial/slumber; DET 0, no reset */
        wr(p, PX_CMD, rd(p, PX_CMD) | CMD_SUD | CMD_POD); usable |= 1u << i;
    }
    /* Every port up (DET 3): done. Else up to 2 s, until no DET changed for 100 ms (empty ports stay 0). */
    uint8_t det[32] = {0};
    for (uint64_t start = native_clock_ns(), stable = start;;) {
        bool all = true; uint64_t now = native_clock_ns();
        for (unsigned i = 0; i < 32; i++) {
            if (!(usable & (1u << i))) continue;
            uint8_t d = (uint8_t)(rd(abar + 0x100 + i * 0x80, PX_SSTS) & 0xF);
            if (d != det[i]) { det[i] = d; stable = now; }
            if (d != 3) all = false;
        }
        if (all || now - stable >= 100000000ULL || now - start >= 2000000000ULL) break;
        __asm__ volatile("pause");
    }
    unsigned first = port_total;
    for (unsigned i = 0; i < 32 && port_total < MAX_PORTS; i++) if (usable & (1u << i)) port_probe(hba, i);
    if (first == port_total) { wr(abar, HBA_GHC, GHC_AE); return; }
    hba->irq = block_pci_irq(pci, ahci_irq, hba);
    bool interrupts = hba->irq != 0;
    wr(abar, HBA_IS, ~0u);
    if (interrupts) wr(abar, HBA_GHC, GHC_AE | GHC_IE);
    for (unsigned i = first; i < port_total; i++) { ports[i]->dev.irq = hba->irq; block_register(&ports[i]->dev); }
    ahci_pm[hba->index] = (struct pm_ops){"ahci", ahci_pm_suspend, ahci_pm_resume, hba}; pm_register_storage(&ahci_pm[hba->index]);
}
bool ahci_probe_all(void) {
    pci_device_t* list = pci_get_devices();
    unsigned before = port_total;
    for (int i = 0; i < pci_device_count(); i++) {
        if (list[i].class_code == PCI_CLASS_STORAGE && list[i].subclass == 6 && list[i].prog_if == 1) hba_probe(&list[i]);
        else if (list[i].class_code == PCI_CLASS_STORAGE && list[i].subclass == 4) {   /* RAID mode: vendor driver only */
            static const char digits[] = "0123456789abcdef"; char id[10];
            for (unsigned k = 0; k < 4; k++) { id[k] = digits[(list[i].vendor_id >> (12 - 4 * k)) & 15]; id[5 + k] = digits[(list[i].device_id >> (12 - 4 * k)) & 15]; }
            id[4] = ':'; id[9] = 0;
            vga_print("[64] storage controller "); vga_print(id);
            vga_print(" is in RAID mode (class 01/04): its disks are not visible; set the SATA/NVMe mode to AHCI in the firmware setup\n");
        }
    }
    return port_total > before;
}

/* ============================================================================
 * NexusOS — Intel AC'97 Audio Driver (Implementation) — Phase 38
 * ============================================================================
 * Brings up the Intel 82801AA AC'97 controller (QEMU "-device AC97") and plays
 * 16-bit stereo PCM through its bus-master DMA engine.
 *
 * Two PCI I/O windows:
 *   BAR0 = NAM  (Native Audio Mixer)      — codec registers (volume, rate)
 *   BAR1 = NABM (Native Audio Bus Master) — DMA: BDL, control, status
 *
 * PCM-out playback uses a Buffer Descriptor List (BDL). We keep one DMA buffer
 * and, per chunk, reset the engine, point the BDL at the buffer, run, and poll
 * the status register to completion (no IRQ needed). QEMU advances the DMA at
 * the codec rate regardless of the host audio backend, so the poll also paces
 * playback; a tick-based timeout guarantees we never hang.
 * ============================================================================ */

#include "ac97.h"
#include "audio.h"
#include "pci.h"
#include "port.h"
#include "vga.h"
#include "string.h"
#include "heap.h"

extern volatile uint32_t system_ticks;
#define TICK_MS 55

/* --- NAM (mixer) register offsets --- */
#define NAM_RESET         0x00
#define NAM_MASTER_VOL    0x02
#define NAM_PCM_OUT_VOL   0x18
#define NAM_EXT_AUDIO_ID  0x28
#define NAM_EXT_AUDIO_CTL 0x2A    /* bit0 = VRA (variable rate audio)      */
#define NAM_PCM_DAC_RATE  0x2C

/* --- NABM (bus master) register offsets — PCM OUT box at +0x10 --- */
#define NABM_PO_BDBAR     0x10    /* dword: BDL base physical address       */
#define NABM_PO_CIV       0x14    /* byte : current index value (RO)        */
#define NABM_PO_LVI       0x15    /* byte : last valid index                */
#define NABM_PO_SR        0x16    /* word : status                          */
#define NABM_PO_PICB      0x18    /* word : position in current buffer (RO) */
#define NABM_PO_CR        0x1B    /* byte : control                         */
#define NABM_GLOB_CNT     0x2C    /* dword: global control                  */
#define NABM_GLOB_STA     0x30    /* dword: global status                   */

/* Status register bits */
#define SR_DCH   0x0001          /* DMA controller halted                  */
#define SR_CELV  0x0002          /* current == last valid                   */
#define SR_LVBCI 0x0004
#define SR_BCIS  0x0008
#define SR_FIFOE 0x0010
/* Control register bits */
#define CR_RPBM  0x01            /* run / pause bus master                  */
#define CR_RR    0x02            /* reset registers                         */
/* Global */
#define GLOB_CNT_COLD 0x00000002 /* cold reset (1 = operating)             */
#define GLOB_STA_PCR  0x00000100 /* primary codec ready                     */

/* BDL: 32 entries × 8 bytes. We only ever arm one. */
#define BDL_ENTRIES   32
#define BDL_IOC       0x80000000u   /* interrupt on completion (in dword1)  */
#define BDL_BUP       0x40000000u   /* buffer underrun policy / last        */

typedef struct __attribute__((packed)) {
    uint32_t addr;      /* buffer physical address               */
    uint32_t ctrl;      /* bits[15:0] = #samples, bit31 IOC, bit30 BUP */
} bdl_entry_t;

/* DMA buffer holds one mix chunk of stereo 16-bit samples. */
#define DMA_FRAMES   AUDIO_MIX_FRAMES
#define DMA_SAMPLES  (DMA_FRAMES * 2)
#define DMA_BYTES    (DMA_SAMPLES * 2)

static struct {
    bool      present;
    uint16_t  nam;          /* NAM  I/O base (BAR0) */
    uint16_t  nabm;         /* NABM I/O base (BAR1) */
    uint32_t  rate;
    bdl_entry_t* bdl;       /* identity-mapped, 8-byte aligned */
    int16_t*  dma;          /* identity-mapped DMA buffer       */
    char      status[80];
} ac;

/* --- register helpers --- */
static inline void     nam_out16(uint8_t r, uint16_t v) { port_word_out(ac.nam + r, v); }
static inline uint16_t nam_in16 (uint8_t r)             { return port_word_in(ac.nam + r); }
static inline void     bm_out8 (uint8_t r, uint8_t v)   { port_byte_out(ac.nabm + r, v); }
static inline void     bm_out16(uint8_t r, uint16_t v)  { port_word_out(ac.nabm + r, v); }
static inline void     bm_out32(uint8_t r, uint32_t v)  { port_dword_out(ac.nabm + r, v); }
static inline uint8_t  bm_in8 (uint8_t r)               { return port_byte_in(ac.nabm + r); }
static inline uint16_t bm_in16(uint8_t r)               { return port_word_in(ac.nabm + r); }
static inline uint32_t bm_in32(uint8_t r)               { return port_dword_in(ac.nabm + r); }

/* Map a 0..100 volume to a 5-bit AC'97 attenuation field (0 = loudest). */
static uint16_t vol_to_reg(uint8_t vol_0_100) {
    if (vol_0_100 == 0) return 0x8000;            /* mute bit */
    if (vol_0_100 > 100) vol_0_100 = 100;
    uint16_t atten = (uint16_t)((100 - vol_0_100) * 31 / 100);  /* 0..31 */
    return (uint16_t)((atten << 8) | atten);      /* left | right */
}

/* --------------------------------------------------------------------------
 * Sound-device ops (registered with audio.c)
 * -------------------------------------------------------------------------- */
static bool ac97_ready(void) { return ac.present; }
static uint32_t ac97_rate(void) { return ac.rate; }

static int ac97_set_rate(uint32_t hz) {
    /* The mixer resamples to the device rate, so we keep the codec at its
     * 48 kHz default and just report it. (VRA left disabled for stability.) */
    (void)hz;
    ac.rate = 48000;
    return 0;
}

static void ac97_set_master(uint8_t vol_0_100) {
    if (!ac.present) return;
    nam_out16(NAM_MASTER_VOL, vol_to_reg(vol_0_100));
}

/* Play one chunk of interleaved 16-bit stereo. Blocks until the DMA drains. */
static int ac97_write(const int16_t* stereo, uint32_t frames) {
    if (!ac.present || !stereo || frames == 0) return 0;
    if (frames > DMA_FRAMES) frames = DMA_FRAMES;

    uint32_t samples = frames * 2;                 /* 16-bit sample count */
    memcpy(ac.dma, stereo, samples * sizeof(int16_t));

    /* Halt, then reset the PCM-out DMA registers. */
    bm_out8(NABM_PO_CR, 0x00);
    bm_out8(NABM_PO_CR, CR_RR);
    for (int i = 0; i < 100000 && (bm_in8(NABM_PO_CR) & CR_RR); i++) io_wait();

    /* Arm a single-buffer BDL. */
    ac.bdl[0].addr = (uint32_t)ac.dma;
    ac.bdl[0].ctrl = (samples & 0xFFFF) | BDL_IOC | BDL_BUP;
    bm_out32(NABM_PO_BDBAR, (uint32_t)ac.bdl);
    bm_out8 (NABM_PO_LVI, 0);
    bm_out16(NABM_PO_SR, SR_BCIS | SR_LVBCI | SR_FIFOE);   /* clear status */

    /* Run (poll mode — no interrupts armed). */
    bm_out8(NABM_PO_CR, CR_RPBM);

    /* Poll to completion, bounded by a tick timeout (chunk dur + slack) plus
     * a spin hard-cap so a wedged engine can never hang the kernel. */
    uint32_t chunk_ms = (frames * 1000) / (ac.rate ? ac.rate : 48000);
    uint32_t timeout  = chunk_ms / TICK_MS + 3;
    uint32_t start    = system_ticks;
    uint32_t spins    = 0;
    while (!(bm_in16(NABM_PO_SR) & SR_DCH)) {
        if ((system_ticks - start) >= timeout) break;
        if (++spins > 50000000u) break;
        __asm__ volatile("hlt");
    }
    bm_out8(NABM_PO_CR, 0x00);                              /* stop          */
    bm_out16(NABM_PO_SR, SR_BCIS | SR_LVBCI | SR_FIFOE);   /* clear status  */
    return (int)frames;
}

static sound_device_t ac97_dev = {
    .name       = "AC97",
    .ready      = ac97_ready,
    .set_rate   = ac97_set_rate,
    .write      = ac97_write,
    .set_master = ac97_set_master,
    .rate       = ac97_rate,
};

/* --------------------------------------------------------------------------
 * ac97_init: probe PCI, reset codec, allocate DMA, register with audio core
 * -------------------------------------------------------------------------- */
void ac97_init(void) {
    memset(&ac, 0, sizeof(ac));
    ac.rate = 48000;
    strcpy(ac.status, "AC97: not present");

    /* Find the controller: prefer the exact QEMU id, else any audio device. */
    pci_device_t* dev = pci_find_device(AC97_VENDOR_INTEL, AC97_DEVICE_82801AA);
    if (!dev) dev = pci_find_class(PCI_CLASS_MULTIMEDIA, 0x01);
    if (!dev) {
        vga_print_color("[--] ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        vga_print("AC97: no audio controller found (add QEMU '-device AC97')\n");
        return;
    }

    /* Two I/O BARs: BAR0 = NAM (mixer), BAR1 = NABM (bus master). */
    ac.nam  = (uint16_t)(dev->bar[0] & 0xFFFC);
    ac.nabm = (uint16_t)(dev->bar[1] & 0xFFFC);
    if (!(dev->bar[0] & 1) || !(dev->bar[1] & 1) || ac.nam == 0 || ac.nabm == 0) {
        vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("AC97: unexpected BAR layout\n");
        return;
    }

    /* Enable PCI I/O space + bus mastering (DMA). */
    uint32_t cmd = pci_read32(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd |= (1 << 0) | (1 << 2);
    pci_write32(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);

    /* DMA structures — heap is identity-mapped (<16MB), so virt == phys. */
    ac.bdl = (bdl_entry_t*)kmalloc_aligned(sizeof(bdl_entry_t) * BDL_ENTRIES, 16);
    ac.dma = (int16_t*)kmalloc_aligned(DMA_BYTES, 4096);
    if (!ac.bdl || !ac.dma) {
        vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("AC97: cannot allocate DMA buffers\n");
        return;
    }
    memset(ac.bdl, 0, sizeof(bdl_entry_t) * BDL_ENTRIES);
    memset(ac.dma, 0, DMA_BYTES);

    /* Cold reset / bring the controller out of reset. */
    bm_out32(NABM_GLOB_CNT, GLOB_CNT_COLD);

    /* Reset the codec via the NAM reset register. */
    nam_out16(NAM_RESET, 0);

    /* Wait for the primary codec to report ready. */
    bool ready = false;
    for (int i = 0; i < 200000; i++) {
        if (bm_in32(NABM_GLOB_STA) & GLOB_STA_PCR) { ready = true; break; }
        io_wait();
    }

    /* Unmute and set sane volumes (master from the mixer default, PCM full). */
    nam_out16(NAM_MASTER_VOL,  vol_to_reg(audio_get_master_volume()));
    nam_out16(NAM_PCM_OUT_VOL, 0x0000);   /* 0 dB attenuation, unmuted */

    ac.present = true;

    /* Build status string (hex_to_str already prepends "0x"). */
    char* p = ac.status;
    const char* base = "AC97 @ NAM ";
    while (*base) *p++ = *base++;
    char b[12];
    hex_to_str(ac.nam, b);  for (char* q = b; *q; q++) *p++ = *q;
    const char* mid = " NABM ";
    while (*mid) *p++ = *mid++;
    hex_to_str(ac.nabm, b); for (char* q = b; *q; q++) *p++ = *q;
    const char* tail = ready ? ", codec ready" : ", codec timeout";
    while (*tail) *p++ = *tail++;
    *p = '\0';

    /* Register as the active sound device. */
    audio_register_device(&ac97_dev);

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("AC97 audio: ");
    vga_print(ready ? "Intel 82801AA codec ready\n" : "controller up (codec slow)\n");
}

bool ac97_present(void) { return ac.present; }
const char* ac97_status(void) { return ac.status; }

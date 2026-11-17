/* AC'97 (ICH 8086:2415) PCM-out engine behind the shared ALSA mixer (sound.c). The 32-entry BDL covers the mixer's
   ring only up to the furthest committed frame and LVI marks its end: when every client runs dry the bus master halts
   (DCH) instead of looping stale audio. The position comes from CIV/PICB; no IRQ is used.
   Capture (PCM-in box, NABM 0x00): its own 32-entry BDL splits the capture ring into 32 slots of 4096 frames and is
   kept cyclic by moving LVI one slot behind CIV on every position read, so the bus master never halts while recording.
   Record select = mic (boot option ac97.capture=line: line-in); controls: Capture Volume/Switch (record gain, 0 to
   +22.5 dB in 1.5 dB steps) and Mic Boost (+20dB) Switch. */
#include "sound_hw.h"
#include "boot_info.h"
#include "../../kernel/pci.h"
#include "../../kernel/port.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

#define SLOTS 32U
#define SLOT_LIMIT 30U
#define SLOT_FRAMES_MAX 32767U
enum { NAM_RESET = 0x00, NAM_MASTER = 0x02, NAM_MIC = 0x0E, NAM_PCM_OUT = 0x18, NAM_RECORD_SELECT = 0x1A, NAM_RECORD_GAIN = 0x1C,
       NAM_FRONT_RATE = 0x2C, NAM_ADC_RATE = 0x32 };
enum { PI_BDBAR = 0x00, PI_CIV = 0x04, PI_LVI = 0x05, PI_SR = 0x06, PI_PICB = 0x08, PI_CR = 0x0B };
#define CAPTURE_SLOT_FRAMES (SOUND_RING_FRAMES / SLOTS)   /* 4096 frames = 8192 samples per descriptor */
#define CAPTURE_BDL_OFFSET 256U                         /* in the BDL page, after the PCM-out list */
enum { PO_BDBAR = 0x10, PO_CIV = 0x14, PO_LVI = 0x15, PO_SR = 0x16, PO_PICB = 0x18, PO_CR = 0x1B, GLOB_CNT = 0x2C, GLOB_STA = 0x30 };
enum { SR_DCH = 1, SR_LVBCI = 4, SR_BCIS = 8, SR_FIFOE = 16, CR_RPBM = 1, CR_RR = 2 };

struct descriptor { uint32_t address, control; };
/* BDL bookkeeping: slot_pos/slot_frames = device frames each descriptor covers; head = CIV last seen, last = LVI. */
static struct {
    uint16_t nam, nabm; volatile struct descriptor* bdl; uint64_t ring;
    uint64_t slot_pos[SLOTS]; uint32_t slot_frames[SLOTS]; unsigned head, count, last;
    volatile struct descriptor* cap_bdl; uint64_t cap_ring, cap_slots; unsigned cap_head; bool cap_running, line;
    struct sound_engine* engine;
} ac97;

static void bm8(unsigned reg, uint8_t value) { port_byte_out((uint16_t)(ac97.nabm + reg), value); }
static void bm16(unsigned reg, uint16_t value) { port_word_out((uint16_t)(ac97.nabm + reg), value); }
static uint32_t bm_in32(unsigned reg) { return port_dword_in((uint16_t)(ac97.nabm + reg)); }
static void nam16(unsigned reg, uint16_t value) { port_word_out((uint16_t)(ac97.nam + reg), value); }

static void ac97_stop(void) {
    bm8(PO_CR, 0); bm8(PO_CR, CR_RR);
    for (unsigned i = 0; i < 1000 && (port_byte_in((uint16_t)(ac97.nabm + PO_CR)) & CR_RR); i++) __asm__ volatile("pause");
    ac97.count = 0;
}
/* Cover [*queued, target) of the DMA ring with descriptors (split at the ring end and the 16-bit sample count). */
static unsigned append(uint64_t* queued, uint64_t target) {
    unsigned added = 0;
    while (*queued < target && ac97.count < SLOT_LIMIT) {
        uint64_t index = *queued % SOUND_RING_FRAMES, frames = target - *queued;
        if (frames > SOUND_RING_FRAMES - index) frames = SOUND_RING_FRAMES - index;
        if (frames > SLOT_FRAMES_MAX) frames = SLOT_FRAMES_MAX;
        unsigned slot = (ac97.last + 1) % SLOTS;
        ac97.bdl[slot].address = (uint32_t)(ac97.ring + index * SOUND_FRAME_BYTES);
        ac97.bdl[slot].control = (uint32_t)(frames * SOUND_CHANNELS);
        ac97.slot_pos[slot] = *queued; ac97.slot_frames[slot] = (uint32_t)frames;
        ac97.last = slot; ac97.count++; *queued += frames; added++;
    }
    return added;
}
static bool ac97_start(uint64_t* queued, uint64_t target) {
    port_dword_out((uint16_t)(ac97.nabm + PO_BDBAR), (uint32_t)virt_to_phys(ac97.bdl));
    ac97.last = SLOTS - 1; ac97.head = 0;
    if (!append(queued, target)) return false;
    __asm__ volatile("mfence" ::: "memory");
    bm8(PO_LVI, (uint8_t)ac97.last); bm16(PO_SR, SR_LVBCI | SR_BCIS | SR_FIFOE); bm8(PO_CR, CR_RPBM);
    return true;
}
/* Extend the BDL; a halted engine restarts through LVI (resumes at PIV). */
static bool ac97_extend(uint64_t* queued, uint64_t target) {
    if (!append(queued, target)) return false;
    __asm__ volatile("mfence" ::: "memory");
    bm8(PO_LVI, (uint8_t)ac97.last);
    return true;
}
/* Device position from CIV/PICB; false once the bus master halted after the last descriptor. */
static bool ac97_read(uint64_t* position) {
    uint32_t a, b, c; unsigned tries = 0;
    do { a = bm_in32(PO_CIV); b = bm_in32(PO_PICB); c = bm_in32(PO_CIV); } while (((a ^ c) & 0x1001FU) && ++tries < 8);
    unsigned civ = c & 31U, done = (civ - ac97.head) & 31U;
    if (((c >> 16) & SR_DCH) || done >= ac97.count) { ac97.count = 0; ac97.head = (ac97.last + 1) % SLOTS; return false; }
    uint32_t left = (b & 0xFFFFU) / SOUND_CHANNELS; if (left > ac97.slot_frames[civ]) left = ac97.slot_frames[civ];
    ac97.head = civ; ac97.count -= done; *position = ac97.slot_pos[civ] + ac97.slot_frames[civ] - left;
    return true;
}
static bool ac97_full(void) { return ac97.count >= SLOT_LIMIT; }
static void capture_restore(void);
/* System sleep (sound.c): cold reset de-asserted and the codec set up again as at probe; the bus master stays off. */
static bool ac97_resume(void) {
    port_dword_out((uint16_t)(ac97.nabm + GLOB_CNT), 2U);
    uint64_t deadline = native_clock_ns() + 50000000ULL;
    while (!(bm_in32(GLOB_STA) & 0x100U) && native_clock_ns() < deadline) __asm__ volatile("pause");
    nam16(NAM_RESET, 0); nam16(NAM_MASTER, 0x0000); nam16(NAM_PCM_OUT, 0x0000);
    bm8(PO_CR, 0); bm8(PO_CR, CR_RR); ac97.count = 0;
    capture_restore();
    return (bm_in32(GLOB_STA) & 0x100U) != 0;
}

/* ---------- capture ---------- */
static void capture_reset(void) {
    bm8(PI_CR, 0); bm8(PI_CR, CR_RR);
    for (unsigned i = 0; i < 1000 && (port_byte_in((uint16_t)(ac97.nabm + PI_CR)) & CR_RR); i++) __asm__ volatile("pause");
}
static bool ac97_capture_start(void) {
    capture_reset();
    for (unsigned i = 0; i < SLOTS; i++) {
        ac97.cap_bdl[i].address = (uint32_t)(ac97.cap_ring + (uint64_t)i * CAPTURE_SLOT_FRAMES * SOUND_FRAME_BYTES);
        ac97.cap_bdl[i].control = CAPTURE_SLOT_FRAMES * SOUND_CHANNELS;
    }
    port_dword_out((uint16_t)(ac97.nabm + PI_BDBAR), (uint32_t)virt_to_phys(ac97.cap_bdl));
    __asm__ volatile("mfence" ::: "memory");
    ac97.cap_head = 0; ac97.cap_slots = 0;
    bm8(PI_LVI, SLOTS - 1); bm16(PI_SR, SR_LVBCI | SR_BCIS | SR_FIFOE); bm8(PI_CR, CR_RPBM);
    ac97.cap_running = true;
    return true;
}
static void ac97_capture_stop(void) { if (ac97.cap_running) { capture_reset(); ac97.cap_running = false; } }
/* Frames written since the start: whole slots passed (CIV steps) plus the current slot's progress (PICB). */
static bool ac97_capture_read(uint64_t* position) {
    uint32_t a, b, c; unsigned tries = 0;
    do { a = bm_in32(PI_CIV); b = bm_in32(PI_PICB); c = bm_in32(PI_CIV); } while (((a ^ c) & 0x1FU) && ++tries < 8);
    if ((c >> 16) & SR_DCH) { vga_print("[64] AC97 capture DMA halted\n"); ac97.cap_running = false; return false; }
    unsigned civ = c & 31U;
    ac97.cap_slots += (civ - ac97.cap_head) & 31U; ac97.cap_head = civ;
    bm8(PI_LVI, (uint8_t)((civ + SLOTS - 1) % SLOTS));                  /* stay cyclic */
    uint32_t left = (b & 0xFFFFU) / SOUND_CHANNELS; if (left > CAPTURE_SLOT_FRAMES) left = CAPTURE_SLOT_FRAMES;
    *position = ac97.cap_slots * CAPTURE_SLOT_FRAMES + CAPTURE_SLOT_FRAMES - left;
    return true;
}
/* Record gain 0x1C: bit 15 mute, left 11:8, right 3:0 (0..15 = 0..+22.5 dB). Mic 0x0E bit 6: +20 dB pre-amp (the mic
   stays muted in the output mix: bit 15). */
static bool ac97_control_put(unsigned index) {
    (void)index;
    const struct sound_control* c = ac97.engine->control;
    nam16(NAM_RECORD_GAIN, (uint16_t)((c[1].value[0] ? 0 : 0x8000U) | (unsigned)c[0].value[0] << 8 | (unsigned)c[0].value[1]));
    nam16(NAM_MIC, (uint16_t)(0x8008U | (c[2].value[0] ? 0x40U : 0)));
    return true;
}
/* System sleep (ac97_resume): the codec reset cleared record select, gain and boost; the input DMA is stopped
   (sound.c gave the recorders an xrun at suspend; the next start reprograms the BDL). */
static void capture_restore(void) {
    if (!ac97.cap_ring || !ac97.engine) return;
    nam16(NAM_RECORD_SELECT, ac97.line ? 0x0404 : 0x0000);
    capture_reset(); ac97.cap_running = false;
    ac97_control_put(0);
}

bool ac97_probe(struct sound_engine* engine, uint64_t ring) {
    pci_device_t* device = pci_find_device(0x8086, 0x2415);
    if (!device) return false;
    uint32_t nam = device->bar[0] & ~3U, nabm = device->bar[1] & ~3U;
    if (!(device->bar[0] & 1) || !(device->bar[1] & 1) || !nam || !nabm || nam > 0xFF00 || nabm > 0xFFC0) {
        vga_print("[64] AC97 unexpected BAR layout\n"); return false;
    }
    uint64_t bdl = dma32_alloc(4096);
    if (!bdl || ring > 0x100000000ULL - SOUND_RING_FRAMES * SOUND_FRAME_BYTES) {
        arena_free(bdl, 4096); vga_print("[64] AC97 no DMA memory\n"); return false;
    }
    ac97.nam = (uint16_t)nam; ac97.nabm = (uint16_t)nabm; ac97.bdl = (volatile struct descriptor*)bdl; ac97.ring = ring;
    ac97.cap_bdl = (volatile struct descriptor*)(bdl + CAPTURE_BDL_OFFSET); ac97.engine = engine;
    ac97.cap_ring = sound_capture_ring && sound_capture_ring <= 0x100000000ULL - SOUND_RING_FRAMES * SOUND_FRAME_BYTES ? sound_capture_ring : 0;
    /* I/O + bus master on, INTx off: this driver never enables AC'97 interrupt sources. */
    uint16_t command = pci_read16(device->bus, device->device, device->function, PCI_COMMAND);
    pci_write32(device->bus, device->device, device->function, PCI_COMMAND, (command | 5U | 0x400U) & 0xFFFFU);
    port_dword_out((uint16_t)(ac97.nabm + GLOB_CNT), 2U); /* cold reset de-asserted, 2-channel PCM */
    uint64_t deadline = native_clock_ns() + 50000000ULL;
    while (!(bm_in32(GLOB_STA) & 0x100U) && native_clock_ns() < deadline) __asm__ volatile("pause");
    bool ready = (bm_in32(GLOB_STA) & 0x100U) != 0;
    /* Fixed 48 kHz (VRA off). QEMU maps 0x0000 on both to unity gain (0x0808 PCM would scale samples by 190/255). */
    nam16(NAM_RESET, 0); nam16(NAM_MASTER, 0x0000); nam16(NAM_PCM_OUT, 0x0000);
    bm8(PO_CR, 0); bm8(PO_CR, CR_RR);
    memset(engine, 0, sizeof(*engine));
    strcpy(engine->id, "NexusAC97"); strcpy(engine->driver, "NexusAC97"); strcpy(engine->name, "NexusOS AC97");
    strcpy(engine->mixer, "NexusOS AC97"); strcpy(engine->components, "AC97a:00000000"); strcpy(engine->pcm_name, "NexusOS AC97 PCM");
    strcpy(engine->longname, "NexusOS AC97 (Intel 82801AA) at nam ");
    char number[12]; hex_to_str(ac97.nam, number); strcat(engine->longname, number); strcat(engine->longname, " nabm ");
    hex_to_str(ac97.nabm, number); strcat(engine->longname, number);
    engine->cyclic = false; engine->stop = ac97_stop; engine->start = ac97_start; engine->extend = ac97_extend;
    engine->read = ac97_read; engine->full = ac97_full; engine->resume = ac97_resume;
    if (ac97.cap_ring) {
        ac97.line = boot_option("ac97.capture=line");
        nam16(NAM_RECORD_SELECT, ac97.line ? 0x0404 : 0x0000);          /* both channels: mic (0) or line-in (4) */
        bm8(PI_CR, 0); bm8(PI_CR, CR_RR);
        strcpy(engine->capture_name, ac97.line ? "NexusOS AC97 Line-in" : "NexusOS AC97 Mic");
        engine->capture_start = ac97_capture_start; engine->capture_stop = ac97_capture_stop; engine->capture_read = ac97_capture_read;
        struct sound_control* c = engine->control; engine->controls = 3;
        strcpy(c[0].name, "Capture Volume"); c[0].type = SOUND_CONTROL_INTEGER; c[0].channels = 2; c[0].max = 15; c[0].db_step = 150;
        strcpy(c[1].name, "Capture Switch"); c[1].type = SOUND_CONTROL_BOOLEAN; c[1].channels = 1; c[1].max = 1; c[1].value[0] = 1;
        strcpy(c[2].name, "Mic Boost (+20dB) Switch"); c[2].type = SOUND_CONTROL_BOOLEAN; c[2].channels = 1; c[2].max = 1;
        engine->control_put = ac97_control_put; ac97_control_put(0);
    }
    vga_print(ready ? "[64] AC97 native PCM ready: " : "[64] AC97 native PCM (codec not ready): ");
    vga_print(engine->longname); sound_log_hex(" front_rate=", port_word_in((uint16_t)(ac97.nam + NAM_FRONT_RATE))); vga_print("\n");
    return true;
}

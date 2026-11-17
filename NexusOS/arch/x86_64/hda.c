/* Intel High Definition Audio: controller + codec driver and the PCM engine behind the shared ALSA mixer (sound.c).
   Any PCI class 04/03 controller (QEMU intel-hda 8086:2668 and ich9-intel-hda 8086:293E, VMware 15AD:1977, Intel/AMD/
   NVIDIA chipsets). BAR0 is mapped uncached; the controller is reset and codecs announce themselves in STATESTS. Verbs
   go one at a time through the CORB/RIRB rings (polled, with a timeout), falling back to the Immediate Command
   registers when the rings do not answer (boot option hda.immediate forces the fallback); a codec that stops
   answering is given up after a few timeouts, so boot never hangs on a codec.
   A generic parser in the spirit of Linux's hda_generic reads every widget of each codec's audio function group
   (capabilities, pin capabilities and default configuration, amplifier capabilities, connection lists), picks an
   analog output pin (line-out/speaker/headphone, presence-detected jacks first; digital HDMI/S/PDIF pins only when no
   codec has an analog output), walks connection lists through mixers/selectors back to an output converter and
   programs the route: D0, converter format 48 kHz/16-bit/stereo and stream tag, connection selects, amplifiers at
   0 dB (other mixer inputs muted), pin control (+HP drive) and EAPD. Further speaker/headphone pins are driven from the
   same stream (joining the first route or through their own converter), so a laptop plays through its speakers; when
   a headphone jack reports presence the speakers are muted (sensed at boot and every 300 ms from sound_poll).
   PCM: the first output stream descriptor loops over the mixer's whole ring (cyclic BDL, 128-byte aligned buffers);
   the position is LPIB. No interrupt is enabled (INTCTL = 0, PCI INTx disabled): the mixer polls. DMA memory is
   cacheable kernel memory, so CPU writes are flushed with CLFLUSH for controllers that do not snoop.
   Jacks: with the CORB/RIRB rings, presence-detect pins (outputs, microphones, line-in, HDMI/DP) get unsolicited
   responses (GCTL.UNSOL, verb 0x708 with a tag); sound_poll drains them from the RIRB and re-senses the pin, with a 2 s
   backstop poll (300 ms polling without the rings).
   Capture (pcmC0D0c): see "capture" below; it uses the driven controller, or when that one has no input path (HDMI/DP
   playback on a graphics card), the first other HDA controller with one (the motherboard codec's microphone jacks),
   driven as a second controller instance (hda_states[1]).
   Realtek ALC662 family (ALC662/663/665/667/668/670/671/272, ALC891/892/897): the PLL fix (coefficient 0x04 bit 15 of
   NID 0x20) and the per-codec EAPD coefficient Linux applies at init.
   HDMI/DisplayPort (Intel, NVIDIA, AMD/ATI): presence and ELD-valid from the pin sense (DP-MST pins: device 0
   selected), the ELD read through GET_HDMI_ELDD or, on AMD 1002:aa01, emulated from the AMD vendor verbs (speaker
   allocation, sink info, audio descriptors); a monitor counts when its ELD lists LPCM at 48 kHz/16-bit (or no SAD:
   basic audio). Every such monitor is driven from the stream (its own converter where the wiring is 1:1, as on AMD).
   Channel mapping for 2-channel LPCM: AMD multichannel slot verbs + channel allocation (no InfoFrame), else ASP slot
   mapping (0x734) and an HDMI or DisplayPort audio InfoFrame (NVIDIA DP: HDMI layout, as its hardware expects).
   Output choice over all controllers: analog with something connected (a present jack, or a pin without detection),
   then an HDMI/DP monitor with audio, then any analog route, then any digital route. Boot option hda.output=hdmi puts
   HDMI first, hda.output=analog skips the monitor pass. */
#include "sound_hw.h"
#include "pcidev.h"
#include "boot_info.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

enum { GCAP = 0x00, VMIN = 0x02, VMAJ = 0x03, GCTL = 0x08, WAKEEN = 0x0C, STATESTS = 0x0E, INTCTL = 0x20,
       CORBLBASE = 0x40, CORBUBASE = 0x44, CORBWP = 0x48, CORBRP = 0x4A, CORBCTL = 0x4C, CORBSTS = 0x4D, CORBSIZE = 0x4E,
       RIRBLBASE = 0x50, RIRBUBASE = 0x54, RIRBWP = 0x58, RINTCNT = 0x5A, RIRBCTL = 0x5C, RIRBSTS = 0x5D, RIRBSIZE = 0x5E,
       ICOI = 0x60, ICII = 0x64, ICIS = 0x68, DPLBASE = 0x70, DPUBASE = 0x74, SD_FIRST = 0x80, SD_SIZE = 0x20 };
enum { SD_CTL = 0x00, SD_CTL_TAG = 0x02, SD_STS = 0x03, SD_LPIB = 0x04, SD_CBL = 0x08, SD_LVI = 0x0C, SD_FIFOS = 0x10,
       SD_FMT = 0x12, SD_BDPL = 0x18, SD_BDPU = 0x1C };
/* Verbs: 12-bit id + 8-bit payload (0x7xx set / 0xFxx get) or 4-bit id + 16-bit payload (format, amplifier). */
enum { V_GET_PARAM = 0xF00, V_GET_CONN = 0xF02, V_GET_POWER = 0xF05, V_GET_SENSE = 0xF09, V_GET_EAPD = 0xF0C,
       V_GET_CONFIG = 0xF1C, V_GET_SUBSYSTEM = 0xF20, V_SET_SEL = 0x701, V_SET_POWER = 0x705, V_SET_STREAM = 0x706,
       V_SET_PINCTL = 0x707, V_EXEC_SENSE = 0x709, V_SET_EAPD = 0x70C, V_SET_DIGI1 = 0x70D, V_SET_CHANNELS = 0x72D,
       V_DIP_INDEX = 0x730, V_DIP_DATA = 0x731, V_DIP_XMIT = 0x732, V_SET_FORMAT = 0x2, V_SET_AMP = 0x3,
       V_SET_UNSOL = 0x708, V_SET_CHAN_SLOT = 0x734, V_SET_DEVICE_SEL = 0x735, V_GET_DIP_SIZE = 0xF2E, V_GET_ELDD = 0xF2F,
       V_SET_PROC_COEF = 0x4, V_SET_COEF_INDEX = 0x5, V_GET_PROC_COEF = 0xC,
       V_ATI_RAMP_RATE = 0x770, V_ATI_CHANNEL_ALLOCATION = 0x771, V_ATI_DOWNMIX = 0x772, V_ATI_AUDIO_DESCRIPTOR = 0x776,
       V_ATI_MULTICHANNEL_01 = 0x777, V_ATI_SINK_INFO_INDEX = 0x780, V_ATI_MULTICHANNEL_1 = 0x785, V_ATI_MULTICHANNEL_MODE = 0x789,
       V_ATI_GET_SPEAKER_ALLOCATION = 0xF70, V_ATI_GET_AUDIO_DESCRIPTOR = 0xF76, V_ATI_GET_AV_DELAY = 0xF7B, V_ATI_GET_SINK_INFO = 0xF81 };
enum { P_VENDOR = 0x00, P_REVISION = 0x02, P_NODES = 0x04, P_FG_TYPE = 0x05, P_WIDGET = 0x09, P_PCM = 0x0A,
       P_PINCAP = 0x0C, P_AMP_IN = 0x0D, P_CONN_LEN = 0x0E, P_AMP_OUT = 0x12, P_DEVLIST = 0x15 };
enum { W_OUT = 0, W_IN = 1, W_MIX = 2, W_SEL = 3, W_PIN = 4 };
enum { DEV_LINE_OUT = 0, DEV_SPEAKER = 1, DEV_HP = 2, DEV_SPDIF = 4, DEV_DIGITAL_OUT = 5, DEV_LINE_IN = 8, DEV_MIC = 0xA };
enum { MODE_ANALOG_CONNECTED, MODE_HDMI_MONITOR, MODE_ANALOG_ANY, MODE_DIGITAL_ANY, MODE_CAPTURE };
#define WC_IN_AMP (1U << 1)
#define WC_OUT_AMP (1U << 2)
#define WC_AMP_OVERRIDE (1U << 3)
#define WC_FORMAT_OVERRIDE (1U << 4)
#define WC_UNSOL (1U << 7)
#define WC_CONN_LIST (1U << 8)
#define WC_DIGITAL (1U << 9)
#define WC_POWER (1U << 10)
#define PC_TRIGGER (1U << 1)
#define PC_IN (1U << 5)
#define PC_VREF_50 (1U << 9)
#define PC_VREF_80 (1U << 12)
#define PC_VREF_100 (1U << 13)
#define CAPTURE_TAG 2U            /* input stream tag (output streams use TAG) */
#define CAP_BDL_OFFSET 3584U      /* capture BDL: 32 x 16 bytes after the output BDL in the DMA page */
#define MAX_SOURCES 6U
#define PC_PRESENCE (1U << 2)
#define PC_HP_DRIVE (1U << 3)
#define PC_OUT (1U << 4)
#define PC_HDMI (1U << 7)
#define PC_EAPD (1U << 16)
#define PC_DP (1U << 24)

#define FORMAT 0x0011U            /* 48 kHz base, x1 /1, 16 bits, 2 channels */
#define TAG 1U                    /* stream tag shared by the descriptor and every converter we drive */
#define VERB_TIMEOUT_NS 20000000ULL
#define CORB_OFFSET 0U            /* DMA page: CORB 256 x 4 bytes, RIRB 256 x 8 bytes, BDL 64 x 16 bytes */
#define RIRB_OFFSET 1024U
#define BDL_OFFSET 3072U
#define BDL_CHUNK_FRAMES 16384U   /* 64 KiB per descriptor: the 512 KiB ring takes 8 or 9 entries */
#define MAX_WIDGETS 128U
#define MAX_CONN 24U
#define MAX_PATH 8U
#define MAX_OUTS 4U
#define MAX_JACKS 12U
#define ELD_MAX 256U

/* HDMI/DP sink (from the ELD): usable = LPCM at 48 kHz/16-bit (or basic audio); conn 0 HDMI, 1 DisplayPort. */
struct eld { bool valid, usable; uint8_t conn, channels, speakers; char name[17]; };
struct widget { uint16_t nid, conn[MAX_CONN]; uint8_t type, conns, devices; bool present; uint32_t caps, pincaps, config, amp_in, amp_out, pcm; struct eld eld; };
struct codec { unsigned address, count; uint16_t afg; uint32_t vendor, subsystem, revision; struct widget w[MAX_WIDGETS]; };
/* A route: node[0] = pin ... node[length-1] = output converter (widget indices); index[i] = connection of node[i]
   that selects node[i+1]. */
struct route { uint16_t node[MAX_PATH]; uint8_t index[MAX_PATH], length; };
struct plan { unsigned count; bool digital; struct route route[MAX_OUTS]; };

struct hda_state {
    volatile uint8_t* regs; uint64_t mapped, dma, ring; uint16_t vendor, device; uint32_t bar; const pci_device_t* pci;
    volatile uint32_t *corb, *rirb, *bdl; unsigned corb_size, rirb_size, corb_wp, rirb_rp; bool immediate, ok64;
    uint8_t timeouts[16];
    unsigned sd; bool bidirectional;
    uint64_t base, bytes; uint32_t lpib; bool error_logged;
    unsigned codec; bool automute, hp_present; uint64_t next_sense_ns;
    struct { uint16_t nid; uint8_t device, control; bool detect, trigger; } pin[MAX_OUTS]; unsigned pins;
    /* presence-detect jacks of the driven codec: unsolicited tag = index + 1 */
    struct { uint16_t nid; uint8_t device, location; bool trigger, present, digital, devices, events; } jack[MAX_JACKS]; unsigned jacks;
    bool unsol, poll_all; uint32_t unsol_pending; uint32_t vendor_codec, revision_codec;
};
/* Controller instances: [0] drives the PCM, [1] a separate capture controller; every helper works on *hda_ctl. */
static struct hda_state hda_states[2], *hda_ctl = &hda_states[0];
#define hda (*hda_ctl)

/* ---------- registers, time, logging ---------- */
#ifndef HDA_TEST_MMIO   /* tests/hda_codec_test.c supplies a simulated controller instead */
static uint8_t rd8(unsigned r) { return *(volatile uint8_t*)(hda.regs + r); }
static uint16_t rd16(unsigned r) { return *(volatile uint16_t*)(hda.regs + r); }
static uint32_t rd32(unsigned r) { return *(volatile uint32_t*)(hda.regs + r); }
static void wr8(unsigned r, uint8_t v) { *(volatile uint8_t*)(hda.regs + r) = v; }
static void wr16(unsigned r, uint16_t v) { *(volatile uint16_t*)(hda.regs + r) = v; }
static void wr32(unsigned r, uint32_t v) { *(volatile uint32_t*)(hda.regs + r) = v; }
#endif
static uint32_t rd(unsigned r, unsigned width) { return width == 1 ? rd8(r) : width == 2 ? rd16(r) : rd32(r); }
/* Poll register r until (value & mask) == want; false after `ns`. */
static bool wait_reg(unsigned r, unsigned width, uint32_t mask, uint32_t want, uint64_t ns) {
    uint64_t deadline = native_clock_ns() + ns;
    for (;;) {
        if ((rd(r, width) & mask) == want) return true;
        if (native_clock_ns() >= deadline) return false;
        cpu_pause();
    }
}
static void delay_us(unsigned us) { uint64_t end = native_clock_ns() + us * 1000ULL; while (native_clock_ns() < end) cpu_pause(); }
static void flush_range(const void* data, uint64_t bytes) {
    for (uint64_t p = (uint64_t)data & ~63ULL, end = (uint64_t)data + bytes; p < end; p += 64) __asm__ volatile("clflush (%0)" :: "r"(p) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}
static void hex_text(char* out, uint64_t value, unsigned digits, bool upper) {
    const char* set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    for (unsigned i = 0; i < digits; i++) out[i] = set[(value >> (4 * (digits - 1 - i))) & 15];
    out[digits] = 0;
}
static void put_hex(uint64_t value, unsigned digits, bool upper) { char text[17]; hex_text(text, value, digits, upper); vga_print(text); }
static void add(char* out, unsigned size, const char* text) { unsigned n = (unsigned)strlen(out); while (*text && n + 1 < size) out[n++] = *text++; out[n] = 0; }
static void add_hex(char* out, unsigned size, uint64_t value, unsigned digits, bool upper) { char text[17]; hex_text(text, value, digits, upper); add(out, size, text); }
static void prefix(void) { vga_print("[64] HDA "); put_hex(hda.vendor, 4, true); vga_print(":"); put_hex(hda.device, 4, true); vga_print(" "); }

/* ---------- verbs ---------- */
static uint32_t verb_word(unsigned codec, unsigned nid, unsigned verb, unsigned payload) {
    uint32_t head = (uint32_t)codec << 28 | (uint32_t)(nid & 0xFF) << 20;
    return verb < 0x10 ? head | (uint32_t)verb << 16 | (payload & 0xFFFF) : head | (uint32_t)(verb & 0xFFF) << 8 | (payload & 0xFF);
}
static bool corb_command(uint32_t word, uint32_t* response) {
    wr8(RIRBSTS, 0x05);                      /* response-interrupt + overrun flags: QEMU pauses the CORB until cleared */
    unsigned wp = (hda.corb_wp + 1) & (hda.corb_size - 1);
    hda.corb[wp] = word; flush_range((const void*)&hda.corb[wp], 4);
    hda.corb_wp = wp; wr16(CORBWP, (uint16_t)wp);
    uint64_t deadline = native_clock_ns() + VERB_TIMEOUT_NS;
    for (;;) {
        unsigned written = rd16(RIRBWP) & (hda.rirb_size - 1);
        while (hda.rirb_rp != written) {
            hda.rirb_rp = (hda.rirb_rp + 1) & (hda.rirb_size - 1);
            volatile uint32_t* entry = hda.rirb + 2 * hda.rirb_rp;
            flush_range((const void*)entry, 8);
            uint32_t value = entry[0], extended = entry[1];
            wr8(RIRBSTS, 0x05);
            if (extended & 0x10) { if ((extended & 0xF) == hda.codec && (value >> 26) >= 1 && (value >> 26) <= MAX_JACKS) hda.unsol_pending |= 1U << (value >> 26); continue; }
            if ((extended & 0xF) != word >> 28) continue;                     /* a late answer */
            *response = value; return true;
        }
        if (native_clock_ns() >= deadline) return false;
        cpu_pause();
    }
}
static bool immediate_command(uint32_t word, uint32_t* response) {
    if (!wait_reg(ICIS, 2, 1, 0, VERB_TIMEOUT_NS)) return false;   /* ICB: previous command still busy */
    wr16(ICIS, 2);                                                  /* clear IRV (write 1 to clear) */
    wr32(ICOI, word); wr16(ICIS, 1);                                /* ICB: send */
    if (!wait_reg(ICIS, 2, 3, 2, VERB_TIMEOUT_NS)) return false;
    *response = rd32(ICII); wr16(ICIS, 2);
    return true;
}
static void rings_stop(void) {
    wr8(CORBCTL, 0); wr8(RIRBCTL, 0);
    wait_reg(CORBCTL, 1, 2, 0, 1000000); wait_reg(RIRBCTL, 1, 2, 0, 1000000);
}
static bool command(unsigned codec, unsigned nid, unsigned verb, unsigned payload, uint32_t* response) {
    uint32_t ignored, word = verb_word(codec, nid, verb, payload);
    if (!response) response = &ignored;
    *response = 0;
    if (codec > 14 || hda.timeouts[codec] >= 3) return false;
    bool ok = hda.immediate ? immediate_command(word, response) : corb_command(word, response);
    if (!ok && !hda.immediate) {
        prefix(); vga_print("CORB/RIRB timeout (verb "); put_hex(word, 8, false); vga_print("), using immediate commands\n");
        rings_stop(); hda.immediate = true;
        ok = immediate_command(word, response);
    }
    if (ok) { hda.timeouts[codec] = 0; return true; }
    if (++hda.timeouts[codec] == 3) { prefix(); vga_print("codec "); put_hex(codec, 1, false); vga_print(" stopped answering, giving it up\n"); }
    return false;
}
static uint32_t param(unsigned codec, unsigned nid, unsigned id) { uint32_t value; command(codec, nid, V_GET_PARAM, id, &value); return value; }
static uint32_t get(unsigned codec, unsigned nid, unsigned verb, unsigned payload) { uint32_t value; command(codec, nid, verb, payload, &value); return value; }
static void set(unsigned codec, unsigned nid, unsigned verb, unsigned payload) { command(codec, nid, verb, payload, NULL); }
static bool codec_alive(unsigned codec) { return hda.timeouts[codec] < 3; }

/* ---------- controller ---------- */
static unsigned stream_count(uint16_t gcap) { return ((gcap >> 12) & 15U) + ((gcap >> 8) & 15U) + ((gcap >> 3) & 31U); }
static bool controller_reset(uint16_t gcap) {
    rings_stop();
    for (unsigned i = 0; i < stream_count(gcap) && SD_FIRST + (i + 1) * SD_SIZE <= hda.mapped; i++) wr8(SD_FIRST + i * SD_SIZE + SD_CTL, 0);
    wr32(DPLBASE, 0); wr32(DPUBASE, 0); wr32(INTCTL, 0); wr16(WAKEEN, 0);
    wr16(STATESTS, 0x7FFF);
    wr32(GCTL, rd32(GCTL) & ~1U);                         /* enter link reset */
    if (!wait_reg(GCTL, 4, 1, 0, 100000000ULL)) return false;
    delay_us(150);                                         /* >= 100 us for the codec PLLs */
    wr32(GCTL, (rd32(GCTL) | 1U) & ~0x100U);               /* leave reset; unsolicited responses stay off */
    if (!wait_reg(GCTL, 4, 1, 1, 100000000ULL)) return false;
    delay_us(1000);                                        /* codecs request SDI wake within 521 us */
    return true;
}
static unsigned ring_entries(unsigned reg) {
    uint8_t value = rd8(reg); unsigned code = value & 0x40 ? 2 : value & 0x20 ? 1 : 0;
    wr8(reg, (uint8_t)((value & ~3U) | code));
    return code == 2 ? 256 : code == 1 ? 16 : 2;
}
static void rings_start(void) {
    hda.corb = (volatile uint32_t*)(hda.dma + CORB_OFFSET); hda.rirb = (volatile uint32_t*)(hda.dma + RIRB_OFFSET);
    uint64_t corb = virt_to_phys(hda.corb), rirb = virt_to_phys(hda.rirb);
    hda.corb_size = ring_entries(CORBSIZE);
    wr32(CORBLBASE, (uint32_t)corb); wr32(CORBUBASE, (uint32_t)(corb >> 32));
    wr16(CORBRP, 0x8000); wait_reg(CORBRP, 2, 0x8000, 0x8000, 1000000);   /* some controllers clear it themselves */
    wr16(CORBRP, 0); wait_reg(CORBRP, 2, 0x8000, 0, 1000000);
    wr16(CORBWP, 0); hda.corb_wp = 0; wr8(CORBSTS, 1);
    wr8(CORBCTL, 2); wait_reg(CORBCTL, 1, 2, 2, 1000000);
    hda.rirb_size = ring_entries(RIRBSIZE);
    wr32(RIRBLBASE, (uint32_t)rirb); wr32(RIRBUBASE, (uint32_t)(rirb >> 32));
    wr16(RIRBWP, 0x8000); hda.rirb_rp = 0;
    /* RINTCTL on (INTCTL.GIE stays off, so nothing is signalled): QEMU stops fetching verbs after RINTCNT responses
       until RIRBSTS.RINTFL is cleared, and only sets RINTFL with RINTCTL enabled. */
    wr16(RINTCNT, 1); wr8(RIRBSTS, 0x05);
    wr8(RIRBCTL, 3); wait_reg(RIRBCTL, 1, 2, 2, 1000000);
    flush_range((const void*)hda.dma, 4096);
}

/* ---------- codec parsing ---------- */
static void read_connections(struct codec* c, struct widget* w) {
    uint32_t length = param(c->address, w->nid, P_CONN_LEN), entry = 0, previous = 0;
    bool wide = (length & 0x80) != 0; unsigned total = length & 0x7F, per = wide ? 2 : 4, bits = wide ? 16 : 8;
    uint32_t mask = wide ? 0x7FFF : 0x7F, range = wide ? 0x8000 : 0x80;
    for (unsigned i = 0; i < total && w->conns < MAX_CONN; i++) {
        if (i % per == 0) entry = get(c->address, w->nid, V_GET_CONN, i);
        uint32_t value = entry & ((1U << bits) - 1), nid = value & mask; entry >>= bits;
        if ((value & range) && previous && previous < nid) {           /* a range previous+1 .. nid */
            for (uint32_t n = previous + 1; n <= nid && w->conns < MAX_CONN; n++) w->conn[w->conns++] = (uint16_t)n;
        } else w->conn[w->conns++] = (uint16_t)nid;                     /* 0 keeps the index numbering of later entries */
        previous = nid;
    }
}
static bool codec_parse(struct codec* c, unsigned address) {
    memset(c, 0, sizeof(*c)); c->address = address;
    uint32_t vendor;
    if (!command(address, 0, V_GET_PARAM, P_VENDOR, &vendor) || !vendor || vendor == 0xFFFFFFFFU) return false;
    c->vendor = vendor; c->revision = param(address, 0, P_REVISION);
    uint32_t nodes = param(address, 0, P_NODES);
    for (unsigned n = (nodes >> 16) & 0xFF, end = n + (nodes & 0xFF); n < end && !c->afg && codec_alive(address); n++)
        if ((param(address, n, P_FG_TYPE) & 0xFF) == 1) c->afg = (uint16_t)n;
    if (!c->afg) return false;
    set(address, c->afg, V_SET_POWER, 0);                                  /* audio function group to D0 */
    for (uint64_t deadline = native_clock_ns() + 100000000ULL; codec_alive(address) && native_clock_ns() < deadline; delay_us(500))
        if (!(get(address, c->afg, V_GET_POWER, 0) & 0xF0)) break;
    c->subsystem = get(address, c->afg, V_GET_SUBSYSTEM, 0);
    uint32_t afg_in = param(address, c->afg, P_AMP_IN), afg_out = param(address, c->afg, P_AMP_OUT), afg_pcm = param(address, c->afg, P_PCM);
    nodes = param(address, c->afg, P_NODES);
    unsigned first = (nodes >> 16) & 0xFF, count = nodes & 0xFF;
    if (count > MAX_WIDGETS) { prefix(); vga_print("codec has more widgets than parsed\n"); count = MAX_WIDGETS; }
    for (unsigned i = 0; i < count && codec_alive(address); i++) {
        struct widget* w = &c->w[c->count++];
        w->nid = (uint16_t)(first + i); w->caps = param(address, w->nid, P_WIDGET); w->type = (uint8_t)((w->caps >> 20) & 15);
        if (w->caps & WC_CONN_LIST) read_connections(c, w);
        if (w->type == W_PIN) {
            w->pincaps = param(address, w->nid, P_PINCAP); w->config = get(address, w->nid, V_GET_CONFIG, 0);
            if ((w->caps & WC_DIGITAL) && (w->pincaps & (PC_HDMI | PC_DP))) w->devices = (uint8_t)(param(address, w->nid, P_DEVLIST) & 0x3F);
        }
        if (w->caps & WC_IN_AMP) w->amp_in = w->caps & WC_AMP_OVERRIDE ? param(address, w->nid, P_AMP_IN) : afg_in;
        if (w->caps & WC_OUT_AMP) w->amp_out = w->caps & WC_AMP_OVERRIDE ? param(address, w->nid, P_AMP_OUT) : afg_out;
        if (w->type == W_OUT || w->type == W_IN) w->pcm = w->caps & WC_FORMAT_OVERRIDE ? param(address, w->nid, P_PCM) : afg_pcm;
    }
    return codec_alive(address);
}

static int widget_index_nid(const struct codec* c, unsigned nid);
/* ---------- Realtek ALC662 family init (Linux alc_fix_pll + alc_fill_eapd_coef) ---------- */
static bool coef_update(unsigned codec, unsigned nid, unsigned index, uint16_t clear, uint16_t bits) {
    uint32_t value;
    set(codec, nid, V_SET_COEF_INDEX, index);
    if (!command(codec, nid, V_GET_PROC_COEF, 0, &value)) return false;
    uint16_t updated = (uint16_t)((value & ~clear) | bits);
    set(codec, nid, V_SET_COEF_INDEX, index); set(codec, nid, V_SET_PROC_COEF, updated);
    return true;
}
static void realtek_init(const struct codec* c) {
    if (c->vendor >> 16 != 0x10EC) return;
    unsigned id = c->vendor & 0xFFFF;
    static const uint16_t family[] = {0x0272, 0x0662, 0x0663, 0x0665, 0x0667, 0x0668, 0x0670, 0x0671, 0x0867, 0x0892, 0x0897};
    bool member = false; for (unsigned i = 0; i < sizeof(family) / sizeof(*family); i++) member |= family[i] == id;
    int vendor_widget = widget_index_nid(c, 0x20);
    if (!member || vendor_widget < 0 || c->w[vendor_widget].type != 0xF) return;
    coef_update(c->address, 0x20, 0x04, 1U << 15, 0);                     /* PLL fix */
    if (id == 0x0892 || id == 0x0897) coef_update(c->address, 0x20, 0x07, 1U << 5, 0);
    else if (id == 0x0668) coef_update(c->address, 0x20, 0x07, 3U << 13, 0);
    else if (id == 0x0867) coef_update(c->address, 0x20, 0x04, 1U << 10, 0);
    else if (id == 0x0272 || id == 0x0663 || id == 0x0665 || id == 0x0670 || id == 0x0671) coef_update(c->address, 0x20, 0x0D, 0, 1U << 14);
    else if (id == 0x0662) { uint32_t coef0; set(c->address, 0x20, V_SET_COEF_INDEX, 0);
                             if (command(c->address, 0x20, V_GET_PROC_COEF, 0, &coef0) && (coef0 & 0xF0) == 0x30) coef_update(c->address, 0x20, 0x04, 1U << 10, 0); }
    prefix(); vga_print("codec "); put_hex(c->address, 1, false); vga_print(" Realtek ALC"); put_hex(id, 3, false); vga_print(" coefficients set (PLL, EAPD)\n");
}

/* ---------- HDMI / DisplayPort sinks (ELD) ---------- */
static bool amd_hdmi(const struct codec* c) { return c->vendor == 0x1002AA01; }
static bool amd_rev3(const struct codec* c) { return amd_hdmi(c) && (c->revision & 0xFF00) >= 0x0300; }
static bool nvidia_hdmi(const struct codec* c) { return c->vendor >> 16 == 0x10DE; }
static void eld_parse(const uint8_t* b, unsigned size, struct eld* e) {
    memset(e, 0, sizeof(*e));
    if (size < 20 || size > ELD_MAX || ((b[0] >> 3) != 2 && (b[0] >> 3) != 31)) return;
    unsigned mnl = b[4] & 0x1F, sads = b[5] >> 4;
    if (mnl > 16 || 20 + mnl + 3 * sads > size) return;
    e->conn = (uint8_t)((b[5] >> 2) & 3); e->speakers = b[7];
    unsigned n = 0; for (unsigned i = 0; i < mnl; i++) if (b[20 + i] >= 0x20 && b[20 + i] < 0x7F) e->name[n++] = (char)b[20 + i];
    e->name[n] = 0;
    e->usable = sads == 0; e->channels = 2;                               /* no SAD: basic audio (2-channel LPCM) */
    for (unsigned i = 0; i < sads; i++) {
        const uint8_t* s = b + 20 + mnl + 3 * i;
        unsigned coding = (s[0] >> 3) & 15, channels = (s[0] & 7) + 1U;
        if (coding != 1) continue;
        if ((s[1] & 0x04) && (s[2] & 0x01)) { e->usable = true; if (channels > e->channels) e->channels = (uint8_t)channels; }
    }
    e->valid = true;
}
/* Standard ELD (GET_HDMI_DIP_SIZE with the ELD buffer selector, then GET_HDMI_ELDD byte by byte). */
static unsigned eld_read(const struct codec* c, unsigned nid, uint8_t* buf) {
    unsigned size = get(c->address, nid, V_GET_DIP_SIZE, 0x08) & 0x1FF;
    if (size < 20 || size > ELD_MAX) return 0;
    for (unsigned i = 0; i < size; i++) {
        uint32_t value = get(c->address, nid, V_GET_ELDD, i);
        if (!(value & 0x80000000U) || !codec_alive(c->address)) return 0;
        buf[i] = (uint8_t)value;
    }
    return size;
}
/* AMD/ATI codecs have no ELD buffer: build one from the vendor verbs (Linux get_eld_ati). */
static unsigned eld_read_amd(const struct codec* c, unsigned nid, uint8_t* buf) {
    uint32_t speakers = get(c->address, nid, V_ATI_GET_SPEAKER_ALLOCATION, 0);
    if (!speakers || speakers == 0xFFFFFFFFU) return 0;
    memset(buf, 0, ELD_MAX);
    buf[0] = 2 << 3; buf[7] = (uint8_t)(speakers & 0x7F); if (speakers & 0x200) buf[5] |= 0x04;
    unsigned position = 20, name = 0;
    if (amd_rev3(c)) {
        set(c->address, nid, V_ATI_SINK_INFO_INDEX, 2); name = get(c->address, nid, V_ATI_GET_SINK_INFO, 0) & 0xFF;
        if (name > 16) name = 16;
        buf[4] |= (uint8_t)name;
        for (unsigned i = 0; i < name; i++) { set(c->address, nid, V_ATI_SINK_INFO_INDEX, 5 + i); buf[position++] = (uint8_t)get(c->address, nid, V_ATI_GET_SINK_INFO, 0); }
    }
    set(c->address, nid, V_ATI_AUDIO_DESCRIPTOR, 1 << 3);                 /* LPCM descriptor */
    uint32_t sad = get(c->address, nid, V_ATI_GET_AUDIO_DESCRIPTOR, 0);
    if (sad & 0xFF00) { buf[position++] = (uint8_t)sad; buf[position++] = (uint8_t)(sad >> 8); buf[position++] = (uint8_t)(sad >> 16); }
    if ((sad & 0xFF000000U) && (sad >> 24) != ((sad >> 8) & 0xFF)) {      /* separate stereo rate mask */
        buf[position++] = (uint8_t)((sad & 0xF8) | 1); buf[position++] = (uint8_t)(sad >> 24); buf[position++] = (uint8_t)(sad >> 16);
    }
    if (position == 20 + name) return 0;
    buf[5] |= (uint8_t)(((position - 20 - name) / 3) << 4);
    position = (position + 3) & ~3U; buf[2] = (uint8_t)((position - 4) / 4);
    return position;
}
static void hdmi_sense(const struct codec* c, struct widget* w) {
    if (w->devices) set(c->address, w->nid, V_SET_DEVICE_SEL, 0);
    if (w->pincaps & PC_TRIGGER) set(c->address, w->nid, V_EXEC_SENSE, 0);
    uint32_t sense = get(c->address, w->nid, V_GET_SENSE, 0);
    w->present = (sense >> 31) != 0;
    memset(&w->eld, 0, sizeof(w->eld));
    if (!w->present || !(sense & (1U << 30))) return;
    uint8_t buf[ELD_MAX]; unsigned size = amd_hdmi(c) ? eld_read_amd(c, w->nid, buf) : eld_read(c, w->nid, buf);
    if (size) eld_parse(buf, size, &w->eld);
}
/* ---------- output routes (generic parser) ---------- */
static int widget_index(const struct codec* c, unsigned nid) {
    if (!c->count || nid < c->w[0].nid || nid - c->w[0].nid >= c->count) return -1;
    return (int)(nid - c->w[0].nid);
}
static int widget_index_nid(const struct codec* c, unsigned nid) { return widget_index(c, nid); }
static unsigned pin_device(const struct widget* w) { return (w->config >> 20) & 15; }
static bool pin_digital(const struct widget* w) {
    return (w->caps & WC_DIGITAL) || (w->pincaps & (PC_HDMI | PC_DP)) || pin_device(w) == DEV_SPDIF || pin_device(w) == DEV_DIGITAL_OUT;
}
static bool pin_detects(const struct widget* w) { return (w->pincaps & PC_PRESENCE) && !(w->config & 0x100); }
static bool pin_sense(unsigned codec, unsigned nid, bool trigger) {
    if (trigger) set(codec, nid, V_EXEC_SENSE, 0);
    return (get(codec, nid, V_GET_SENSE, 0) >> 31) != 0;
}
/* An output pin we may drive: output-capable, physically connected, analog line-out/speaker/headphone (or digital). */
static bool pin_usable(const struct widget* w, bool digital) {
    unsigned device = pin_device(w);
    if (w->type != W_PIN || !(w->pincaps & PC_OUT) || (w->config >> 30) == 1 || pin_digital(w) != digital) return false;
    return digital || device == DEV_LINE_OUT || device == DEV_SPEAKER || device == DEV_HP;
}
/* Preference: presence-detected jacks first, then line-out > speaker > headphone; built-in devices before empty jacks. */
static int pin_score(const struct widget* w) {
    unsigned device = pin_device(w);
    int score = device == DEV_LINE_OUT ? 30 : device == DEV_SPEAKER ? 20 : device == DEV_HP ? 10 : 0;
    if (w->eld.usable) score += 200;                                       /* an HDMI/DP monitor that takes our LPCM */
    if ((w->config >> 30) == 2) score += 5;                                  /* fixed (built-in) device */
    if (pin_detects(w)) score += w->present ? 100 : -50;
    return score;
}
static unsigned pin_order(const struct widget* w) {     /* lower = earlier: default association, then sequence */
    unsigned association = (w->config >> 4) & 15;
    return (association ? association : 16) << 4 | (w->config & 15);
}
static bool route_member(const struct plan* plan, unsigned node, unsigned* which, unsigned* position) {
    for (unsigned r = 0; r < plan->count; r++)
        for (unsigned i = 1; i < plan->route[r].length; i++)
            if (plan->route[r].node[i] == node) { *which = r; *position = i; return true; }
    return false;
}
/* Shortest route from pin to an output converter through mixers/selectors (breadth-first, in connection-list order).
   Reaching a node of an already chosen route ends the search: the new route joins it and shares its tail. */
static bool find_route(const struct codec* c, unsigned pin, bool digital, const struct plan* plan, struct route* out) {
    uint8_t parent[MAX_WIDGETS], via[MAX_WIDGETS], depth[MAX_WIDGETS], queue[MAX_WIDGETS]; bool seen[MAX_WIDGETS];
    memset(seen, 0, sizeof(seen));
    unsigned head = 0, tail = 0; queue[tail++] = (uint8_t)pin; seen[pin] = true; depth[pin] = 0;
    while (head < tail) {
        unsigned n = queue[head++];
        for (unsigned k = 0; k < c->w[n].conns; k++) {
            int t = widget_index(c, c->w[n].conn[k]);
            if (t < 0 || seen[t]) continue;
            const struct widget* w = &c->w[t];
            unsigned which = 0, position = 0;
            bool joined = route_member(plan, (unsigned)t, &which, &position);
            bool converter = w->type == W_OUT && ((w->caps & WC_DIGITAL) != 0) == digital;
            if (joined || converter) {
                unsigned chain[MAX_PATH], length = 0, tail_length = joined ? plan->route[which].length - position : 1;
                for (unsigned m = n;; m = parent[m]) { chain[length++] = m; if (m == pin) break; }
                if (length + tail_length > MAX_PATH) continue;
                memset(out, 0, sizeof(*out));
                for (unsigned i = 0; i < length; i++) {
                    out->node[i] = (uint16_t)chain[length - 1 - i];
                    out->index[i] = (uint8_t)(i + 1 < length ? via[chain[length - 2 - i]] : k);
                }
                if (joined) for (unsigned i = position; i < plan->route[which].length; i++) {
                    out->node[length] = plan->route[which].node[i]; out->index[length++] = plan->route[which].index[i];
                } else out->node[length++] = (uint16_t)t;
                out->length = (uint8_t)length;
                return true;
            }
            if ((w->type == W_MIX || w->type == W_SEL) && depth[n] + 3u <= MAX_PATH) {
                seen[t] = true; parent[t] = (uint8_t)n; via[t] = (uint8_t)k; depth[t] = (uint8_t)(depth[n] + 1); queue[tail++] = (uint8_t)t;
            }
        }
    }
    return false;
}
/* Primary output = best-scoring pin with a route; analog plans also drive the other speaker/headphone pins and
   front (sequence 0) line-outs. */
static bool codec_plan(struct codec* c, bool digital, struct plan* plan) {
    memset(plan, 0, sizeof(*plan)); plan->digital = digital;
    uint8_t order[MAX_WIDGETS]; int scores[MAX_WIDGETS]; unsigned candidates = 0;
    for (unsigned i = 0; i < c->count; i++) {
        struct widget* w = &c->w[i];
        if (!pin_usable(w, digital)) continue;
        if (digital && (w->pincaps & (PC_HDMI | PC_DP))) hdmi_sense(c, w);
        else if (pin_detects(w)) w->present = pin_sense(c->address, w->nid, (w->pincaps & PC_TRIGGER) != 0);
        int score = pin_score(w);
        unsigned at = candidates++;
        while (at && (scores[at - 1] < score || (scores[at - 1] == score && pin_order(&c->w[order[at - 1]]) > pin_order(w)))) {
            order[at] = order[at - 1]; scores[at] = scores[at - 1]; at--;
        }
        order[at] = (uint8_t)i; scores[at] = score;
    }
    for (unsigned i = 0; i < candidates && !plan->count; i++)
        if (find_route(c, order[i], digital, plan, &plan->route[0])) plan->count = 1;
    for (unsigned i = 0; i < candidates && plan->count && plan->count < MAX_OUTS; i++) {
        const struct widget* w = &c->w[order[i]];
        if (order[i] == plan->route[0].node[0]) continue;
        if (digital ? !w->eld.usable : (pin_device(w) == DEV_LINE_OUT && (w->config & 15))) continue;   /* digital: every monitor */
        if (find_route(c, order[i], digital, plan, &plan->route[plan->count])) plan->count++;
    }
    return plan->count > 0;
}
static void amp(const struct codec* c, const struct widget* w, bool output, unsigned index, bool mute) {
    uint32_t caps = output ? w->amp_out : w->amp_in;
    unsigned offset = caps & 0x7F, steps = (caps >> 8) & 0x7F, gain = offset <= steps ? offset : steps; /* 0 dB */
    set(c->address, w->nid, V_SET_AMP, (output ? 0x8000U : 0x4000U) | 0x3000U | (index & 15) << 8 | (mute ? 0x80U : 0) | gain);
}
static void program_route(const struct codec* c, const struct route* r, bool digital) {
    for (unsigned i = 0; i < r->length; i++) {
        const struct widget* w = &c->w[r->node[i]];
        if (w->caps & WC_POWER) set(c->address, w->nid, V_SET_POWER, 0);
        if (i + 1 < r->length) {
            if (w->type == W_MIX) {
                for (unsigned j = 0; j < w->conns && j < 16; j++) if (w->caps & WC_IN_AMP) amp(c, w, false, j, j != r->index[i]);
            } else {
                if (w->conns > 1) set(c->address, w->nid, V_SET_SEL, r->index[i]);
                if (w->type != W_PIN && (w->caps & WC_IN_AMP)) amp(c, w, false, r->index[i], false);
            }
        }
        if (w->caps & WC_OUT_AMP) amp(c, w, true, 0, false);
    }
    const struct widget *pin = &c->w[r->node[0]], *dac = &c->w[r->node[r->length - 1]];
    set(c->address, dac->nid, V_SET_FORMAT, FORMAT);
    set(c->address, dac->nid, V_SET_STREAM, TAG << 4);
    if (digital) {
        if (amd_rev3(c)) set(c->address, dac->nid, V_ATI_RAMP_RATE, 180);  /* PCM ramp-up/down (AMD spec default) */
        set(c->address, dac->nid, V_SET_DIGI1, 1);                       /* digital output enable */
        set(c->address, dac->nid, V_SET_CHANNELS, 1);                    /* 2 channels */
    }
    uint8_t control = (uint8_t)(0x40 | (pin_device(pin) == DEV_HP && (pin->pincaps & PC_HP_DRIVE) ? 0x80 : 0));
    if (pin->devices) set(c->address, pin->nid, V_SET_DEVICE_SEL, 0);     /* DP-MST: the first device */
    set(c->address, pin->nid, V_SET_PINCTL, control);
    if (pin->pincaps & PC_EAPD) set(c->address, pin->nid, V_SET_EAPD, (get(c->address, pin->nid, V_GET_EAPD, 0) & 0xFF) | 2);
    if (digital && (pin->pincaps & (PC_HDMI | PC_DP))) {
        if (amd_hdmi(c)) {                                                /* AMD: slot verbs + channel allocation, no InfoFrame */
            set(c->address, pin->nid, V_ATI_DOWNMIX, 0);
            if (amd_rev3(c)) set(c->address, pin->nid, V_ATI_MULTICHANNEL_MODE, 1);   /* channel-wise remapping */
            for (unsigned slot = 0; slot < 8; slot++)
                set(c->address, pin->nid, (unsigned)(V_ATI_MULTICHANNEL_01 + slot / 2 + (slot % 2) * 0x0E), slot < 2 ? (slot << 4) | 1 : 0);
            set(c->address, pin->nid, V_ATI_CHANNEL_ALLOCATION, 0);       /* CA 0: front left/right */
        } else {
            for (unsigned slot = 0; slot < 8; slot++) set(c->address, pin->nid, V_SET_CHAN_SLOT, (slot < 2 ? slot : 0xF) << 4 | slot);
            /* Audio InfoFrame, 2-channel LPCM, CA 0. DisplayPort uses its own header; NVIDIA DP keeps the HDMI layout. */
            uint8_t frame[9]; unsigned length;
            if (pin->eld.conn == 1 && !nvidia_hdmi(c)) { static const uint8_t dp[9] = {0x84, 0x1B, 0x44, 0x01, 0, 0, 0, 0, 0}; memcpy(frame, dp, 9); length = 9; }   /* Linux writes sizeof(union audio_infoframe) = 9 */
            else {
                frame[0] = 0x84; frame[1] = pin->eld.conn == 1 ? 0x1B : 0x01; frame[2] = pin->eld.conn == 1 ? 0x44 : 0x0A; frame[3] = 0;
                frame[4] = 0x01; frame[5] = frame[6] = frame[7] = frame[8] = 0; length = 9;
                uint8_t sum = 0; for (unsigned i = 0; i < length; i++) sum = (uint8_t)(sum + frame[i]);
                frame[3] = (uint8_t)(0x100 - sum);
            }
            set(c->address, pin->nid, V_DIP_INDEX, 0);
            for (unsigned i = 0; i < length; i++) set(c->address, pin->nid, V_DIP_DATA, frame[i]);
            set(c->address, pin->nid, V_DIP_XMIT, 0xC0);
        }
    }
    if (hda.pins < MAX_OUTS) {
        hda.pin[hda.pins].nid = pin->nid; hda.pin[hda.pins].device = (uint8_t)pin_device(pin); hda.pin[hda.pins].control = control;
        hda.pin[hda.pins].detect = pin_detects(pin); hda.pin[hda.pins].trigger = (pin->pincaps & PC_TRIGGER) != 0; hda.pins++;
    }
}
static const char* device_name(unsigned device, bool digital) {
    if (digital) return device == DEV_SPDIF ? "S/PDIF" : "digital";
    return device == DEV_LINE_OUT ? "line-out" : device == DEV_SPEAKER ? "speaker" : device == DEV_HP ? "headphone" : "output";
}
static void print_route(const struct codec* c, const struct route* r, bool primary, bool digital) {
    prefix(); vga_print("codec "); put_hex(c->address, 1, false); vga_print(" vendor "); put_hex(c->vendor, 8, false);
    vga_print(primary ? " out pin 0x" : " also pin 0x"); put_hex(c->w[r->node[0]].nid, 2, false);
    for (unsigned i = 1; i < r->length; i++) {
        const struct widget* w = &c->w[r->node[i]];
        vga_print(w->type == W_OUT ? " <- dac 0x" : w->type == W_MIX ? " <- mix 0x" : " <- sel 0x"); put_hex(w->nid, 2, false);
    }
    const struct widget *pin = &c->w[r->node[0]], *dac = &c->w[r->node[r->length - 1]];
    vga_print(" ("); vga_print(device_name(pin_device(pin), digital));
    if (pin->eld.valid) {
        vga_print(pin->eld.conn == 1 ? ", DisplayPort monitor \"" : ", HDMI monitor \""); vga_print(pin->eld.name); vga_print("\"");
        vga_print(pin->eld.usable ? ", LPCM" : ", no 48 kHz/16-bit LPCM");
    } else if (pin_detects(pin)) vga_print(pin->present ? ", jack present" : ", jack empty");
    if ((dac->pcm & ((1U << 6) | (1U << 17))) != ((1U << 6) | (1U << 17))) vga_print(", converter does not list 48 kHz/16-bit");
    vga_print(")\n");
}
static const char* vendor_name(uint32_t vendor) {
    static const struct { uint16_t id; const char* name; } names[] = {
        {0x1AF4, "QEMU"}, {0x15AD, "VMware"}, {0x10EC, "Realtek"}, {0x14F1, "Conexant"}, {0x111D, "IDT"}, {0x8384, "SigmaTel"},
        {0x11D4, "Analog Devices"}, {0x1106, "VIA"}, {0x1013, "Cirrus Logic"}, {0x8086, "Intel"}, {0x10DE, "NVIDIA"},
        {0x1002, "ATI"}, {0x1022, "AMD"}, {0x434D, "C-Media"}, {0x1102, "Creative"}, {0x17E8, "Chrontel"}};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) if (names[i].id == vendor >> 16) return names[i].name;
    return "HDA";
}

/* ---------- headphone auto-mute (sound_poll) ---------- */
static bool headphone_present(void) {
    for (unsigned i = 0; i < hda.pins; i++)
        if (hda.pin[i].device == DEV_HP && hda.pin[i].detect && pin_sense(hda.codec, hda.pin[i].nid, hda.pin[i].trigger)) return true;
    return false;
}
static void apply_automute(bool headphone) {
    hda.hp_present = headphone;
    for (unsigned i = 0; i < hda.pins; i++)
        if (hda.pin[i].device == DEV_SPEAKER) set(hda.codec, hda.pin[i].nid, V_SET_PINCTL, headphone ? 0 : hda.pin[i].control);
}
static const char* jack_name(unsigned device, unsigned location, bool digital) {
    if (digital) return "HDMI/DP";
    bool front = (location & 0x0F) == 2 && !(location & 0x30), rear = (location & 0x0F) == 1 && !(location & 0x30);
    if (device == DEV_HP) return front ? "front headphone" : "headphone";
    if (device == DEV_MIC) return front ? "front mic" : rear ? "rear mic" : (location & 0x30) == 0x10 ? "internal mic" : "mic";
    if (device == DEV_LINE_IN) return rear ? "rear line-in" : "line-in";
    if (device == DEV_LINE_OUT) return rear ? "rear line-out" : front ? "front line-out" : "line-out";
    return device == DEV_SPEAKER ? "speaker" : "jack";
}
static void jack_changed(unsigned index) {
    bool present = pin_sense(hda.codec, hda.jack[index].nid, hda.jack[index].trigger);
    if (present == hda.jack[index].present) return;
    hda.jack[index].present = present;
    prefix(); vga_print("jack 0x"); put_hex(hda.jack[index].nid, 2, false); vga_print(" (");
    vga_print(jack_name(hda.jack[index].device, hda.jack[index].location, hda.jack[index].digital)); vga_print(present ? ") plugged\n" : ") unplugged\n");
}
/* Drain unsolicited responses that arrived while no verb was pending. */
static void rirb_drain(void) {
    if (hda.immediate || !hda.rirb) return;
    unsigned written = rd16(RIRBWP) & (hda.rirb_size - 1);
    while (hda.rirb_rp != written) {
        hda.rirb_rp = (hda.rirb_rp + 1) & (hda.rirb_size - 1);
        volatile uint32_t* entry = hda.rirb + 2 * hda.rirb_rp;
        flush_range((const void*)entry, 8);
        uint32_t value = entry[0], extended = entry[1];
        wr8(RIRBSTS, 0x05);
        if ((extended & 0x10) && (extended & 0xF) == hda.codec && (value >> 26) >= 1 && (value >> 26) <= MAX_JACKS) hda.unsol_pending |= 1U << (value >> 26);
    }
}
static void hda_poll(uint64_t now) {
    if (!hda.jacks || !codec_alive(hda.codec)) return;
    if (hda.unsol) rirb_drain();
    uint32_t pending = hda.unsol_pending; hda.unsol_pending = 0;
    bool backstop = now >= hda.next_sense_ns;                             /* unsolicited events lost or not available */
    if (!pending && !backstop) return;
    if (backstop) hda.next_sense_ns = now + (hda.unsol && !hda.poll_all ? 2000000000ULL : 300000000ULL);
    for (unsigned i = 0; i < hda.jacks; i++) if (backstop || (pending & (1U << (i + 1)))) jack_changed(i);
    if (!hda.automute) return;
    bool headphone = headphone_present();
    if (headphone == hda.hp_present) return;
    apply_automute(headphone);
    prefix(); vga_print(headphone ? "headphone plugged: speakers muted\n" : "headphone unplugged: speakers on\n");
}
/* Every presence-detect pin of the driven codec: outputs, mic/line-in jacks, HDMI/DP; unsolicited responses on. */
static void jacks_setup(const struct codec* c) {
    hda.jacks = 0; hda.unsol = false; hda.unsol_pending = 0;
    for (unsigned i = 0; i < c->count && hda.jacks < MAX_JACKS; i++) {
        const struct widget* w = &c->w[i];
        if (w->type != W_PIN || !pin_detects(w) || (w->config >> 30) == 1) continue;
        unsigned device = pin_device(w);
        bool digital = pin_digital(w);
        if (!digital && device != DEV_LINE_OUT && device != DEV_SPEAKER && device != DEV_HP && device != DEV_MIC && device != DEV_LINE_IN) continue;
        unsigned j = hda.jacks++;
        hda.jack[j].nid = w->nid; hda.jack[j].device = (uint8_t)device; hda.jack[j].location = (uint8_t)((w->config >> 24) & 0x3F);
        hda.jack[j].trigger = (w->pincaps & PC_TRIGGER) != 0; hda.jack[j].digital = digital; hda.jack[j].devices = w->devices != 0;
        hda.jack[j].events = (w->caps & WC_UNSOL) != 0;
        hda.jack[j].present = digital ? w->present : pin_sense(c->address, w->nid, hda.jack[j].trigger);
        if (!digital && (device == DEV_MIC || device == DEV_LINE_IN)) {
            prefix(); vga_print("jack 0x"); put_hex(w->nid, 2, false); vga_print(" ("); vga_print(jack_name(device, hda.jack[j].location, false));
            vga_print(hda.jack[j].present ? ") present\n" : ") empty\n");
        }
    }
    hda.poll_all = false;
    for (unsigned j = 0; j < hda.jacks; j++) hda.poll_all |= !hda.jack[j].events;   /* a pin without events: keep 300 ms polling */
    if (hda.immediate || !hda.jacks) { hda.poll_all = true; return; }
    bool any = false;
    for (unsigned j = 0; j < hda.jacks; j++) if (hda.jack[j].events) { set(c->address, hda.jack[j].nid, V_SET_UNSOL, 0x80 | (j + 1)); any = true; }
    if (!any) return;
    wr32(GCTL, rd32(GCTL) | 0x100U);                                       /* accept unsolicited responses */
    hda.unsol = true;
}

/* ---------- PCM stream (cyclic over the mixer ring) ---------- */
static void stream_stop_at(unsigned sd) {
    wr8(sd + SD_CTL, (uint8_t)(rd8(sd + SD_CTL) & ~0x1FU));              /* RUN and interrupt enables off */
    wait_reg(sd + SD_CTL, 1, 2, 0, 1000000);
    wr8(sd + SD_STS, 0x1C);                                              /* clear BCIS, FIFOE, DESE */
}
static void stream_stop(void) { stream_stop_at(hda.sd); }
static bool stream_reset(void) {
    stream_stop();
    wr8(hda.sd + SD_CTL, (uint8_t)(rd8(hda.sd + SD_CTL) | 1));
    wait_reg(hda.sd + SD_CTL, 1, 1, 1, 1000000);
    wr8(hda.sd + SD_CTL, (uint8_t)(rd8(hda.sd + SD_CTL) & ~1U));
    return wait_reg(hda.sd + SD_CTL, 1, 1, 0, 1000000);
}
static void hda_stop(void) { stream_stop(); }
/* Start at the ring index of *queued rounded down to 32 frames (128-byte aligned buffer); the frames before it were
   cleared, so the extra lead-in is silence. The BDL covers the whole ring rotated to that index. */
static bool hda_start(uint64_t* queued, uint64_t target) {
    if (!stream_reset()) {
        if (!hda.error_logged) { hda.error_logged = true; prefix(); vga_print("stream reset timed out\n"); }
        return false;
    }
    uint64_t base = *queued & ~31ULL, bdl_physical = virt_to_phys(hda.bdl); unsigned start = (unsigned)(base % SOUND_RING_FRAMES), entries = 0;
    for (unsigned done = 0; done < SOUND_RING_FRAMES; entries++) {
        unsigned index = (start + done) % SOUND_RING_FRAMES, frames = SOUND_RING_FRAMES - index;
        if (frames > BDL_CHUNK_FRAMES) frames = BDL_CHUNK_FRAMES;
        if (frames > SOUND_RING_FRAMES - done) frames = SOUND_RING_FRAMES - done;
        uint64_t address = hda.ring + (uint64_t)index * SOUND_FRAME_BYTES;
        hda.bdl[entries * 4] = (uint32_t)address; hda.bdl[entries * 4 + 1] = (uint32_t)(address >> 32);
        hda.bdl[entries * 4 + 2] = frames * SOUND_FRAME_BYTES; hda.bdl[entries * 4 + 3] = 0;   /* no IOC: polled */
        done += frames;
    }
    flush_range((const void*)hda.bdl, entries * 16ULL);
    wr32(hda.sd + SD_CBL, SOUND_RING_FRAMES * SOUND_FRAME_BYTES);
    wr16(hda.sd + SD_LVI, (uint16_t)(entries - 1));
    wr16(hda.sd + SD_FMT, FORMAT);
    wr32(hda.sd + SD_BDPL, (uint32_t)bdl_physical); wr32(hda.sd + SD_BDPU, (uint32_t)(bdl_physical >> 32));
    wr8(hda.sd + SD_CTL_TAG, (uint8_t)(TAG << 4 | (hda.bidirectional ? 0x08 : 0)));
    __asm__ volatile("mfence" ::: "memory");
    wr8(hda.sd + SD_CTL, (uint8_t)((rd8(hda.sd + SD_CTL) & ~0x1DU) | 0x02));   /* RUN */
    hda.base = base; hda.bytes = 0; hda.lpib = 0;
    if (target > *queued) *queued = target;
    return true;
}
static bool hda_extend(uint64_t* queued, uint64_t target) { if (target > *queued) *queued = target; return true; }
/* Absolute position = start + bytes the DMA consumed (LPIB deltas). The mixer reads it every millisecond while the
   DMA runs; whole laps are never inferred from elapsed time: a stalled VM stalls its DMA too, and a lap missed after
   a longer kernel stall only shifts the timeline by exactly one ring (same ring indices), whereas a false lap would
   push every client into an xrun. false = the stream stopped (descriptor error). */
static bool hda_read(uint64_t* position) {
    const uint32_t lap = SOUND_RING_FRAMES * SOUND_FRAME_BYTES;
    uint32_t lpib = rd32(hda.sd + SD_LPIB) % lap;
    if (lpib == hda.lpib) {                       /* not moving: is the stream still running? (one MMIO read otherwise) */
        uint8_t control = rd8(hda.sd + SD_CTL), status = rd8(hda.sd + SD_STS);
        if (!(control & 2) || (status & 0x10)) {
            if (!hda.error_logged) { hda.error_logged = true; prefix(); vga_print("stream stopped, status 0x"); put_hex(status, 2, false); vga_print("\n"); }
            return false;
        }
    }
    hda.bytes += lpib >= hda.lpib ? lpib - hda.lpib : lpib + lap - hda.lpib;
    hda.lpib = lpib;
    *position = hda.base + hda.bytes / SOUND_FRAME_BYTES;
    return true;
}

/* ---------- capture: input converter (ADC) <- selectors/mixers <- mic or line-in pin ----------
   One ADC of a codec on the driven controller is chosen (the one reaching the most input pins); every reachable
   mic/line-in pin is a source with its route. The best source is used: a plugged external mic, then plugged line-in,
   then a built-in mic (fixed pin or no presence detect: laptop and webcam arrays), then an empty jack. Sources are
   re-checked when recording starts and once a second while it runs (a headset plugged in mid-call takes over).
   Mic pins get VREF bias (80 % > 50 % > 100 %) and one step of their input-amp boost; the route's amps are unmuted at
   0 dB. ALSA controls: "Capture Volume"/"Capture Switch" on the first amp with gain steps (ADC input amp, else a
   mixer/selector amp), "Mic Boost Volume" on the mic pin's input amp. The input DMA is input stream descriptor 0,
   a cyclic BDL over the whole capture ring (sound.c sound_capture_ring), stream tag CAPTURE_TAG. */
struct capture_source {
    uint16_t pin; uint8_t device, location; bool detect, trigger, fixed;
    uint16_t node[MAX_PATH]; uint8_t index[MAX_PATH], length;     /* node[0] = ADC ... node[length-1] = pin (nids) */
    uint32_t caps[MAX_PATH], amp_in[MAX_PATH], amp_out[MAX_PATH]; uint8_t conns[MAX_PATH]; uint32_t pincaps;
};
static struct {
    bool ok, running; unsigned codec, sources, current; struct capture_source source[MAX_SOURCES];
    unsigned sd; uint64_t ring, bytes, next_check_ns; uint32_t lpib; struct hda_state* ctl;
    int vol_k; bool vol_output;                                      /* vol_k: route position of the volume amp, -1 none */
    int volume_control, switch_control, boost_control;               /* engine control indices, -1 none */
    struct sound_engine* engine;
} hcap;
/* The engine sound.c owns (hda_probe's argument). A later re-probe (e.g. after a power loss) may hand
   controller_probe a scratch engine: the capture controls keep living in, and are applied from, this one. */
static struct sound_engine* live_engine;
static unsigned amp_steps(uint32_t caps) { return (caps >> 8) & 0x7F; }
static unsigned amp_offset(uint32_t caps) { unsigned offset = caps & 0x7F, steps = amp_steps(caps); return offset <= steps ? offset : steps; }
static int source_score(const struct capture_source* s, bool present) {
    bool external = !s->fixed && s->detect;
    if (s->device == DEV_MIC) return external ? (present ? 100 : 20) + ((s->location & 0x0F) == 2 ? 5 : 0) : 60;
    return external ? (present ? 80 : 10) : 40;                      /* line-in */
}
/* Breadth-first from the ADC through selectors/mixers to input pins (connection-list order). */
static unsigned capture_routes(const struct codec* c, unsigned adc, struct capture_source* out, unsigned room) {
    uint8_t parent[MAX_WIDGETS], via[MAX_WIDGETS], depth[MAX_WIDGETS], queue[MAX_WIDGETS]; bool seen[MAX_WIDGETS];
    memset(seen, 0, sizeof(seen));
    unsigned head = 0, tail = 0, found = 0; queue[tail++] = (uint8_t)adc; seen[adc] = true; depth[adc] = 0;
    while (head < tail && found < room) {
        unsigned n = queue[head++];
        for (unsigned k = 0; k < c->w[n].conns && found < room; k++) {
            int t = widget_index(c, c->w[n].conn[k]);
            if (t < 0 || seen[t]) continue;
            const struct widget* w = &c->w[t];
            seen[t] = true; parent[t] = (uint8_t)n; via[t] = (uint8_t)k; depth[t] = (uint8_t)(depth[n] + 1);
            if (w->type == W_PIN) {
                unsigned device = pin_device(w);
                if (!(w->pincaps & PC_IN) || (w->config >> 30) == 1 || pin_digital(w) || (device != DEV_MIC && device != DEV_LINE_IN)) continue;
                if (depth[t] + 1U > MAX_PATH) continue;
                struct capture_source* s = &out[found++]; memset(s, 0, sizeof(*s));
                unsigned chain[MAX_PATH], length = 0;
                for (unsigned m = (unsigned)t;; m = parent[m]) { chain[length++] = m; if (m == adc) break; }
                for (unsigned i = 0; i < length; i++) {                  /* chain is pin..adc: reverse it */
                    const struct widget* x = &c->w[chain[length - 1 - i]];
                    s->node[i] = x->nid; s->caps[i] = x->caps; s->amp_in[i] = x->amp_in; s->amp_out[i] = x->amp_out; s->conns[i] = x->conns;
                    s->index[i] = (uint8_t)(i + 1 < length ? via[chain[length - 2 - i]] : 0);
                }
                s->length = (uint8_t)length; s->pin = w->nid; s->device = (uint8_t)device; s->location = (uint8_t)((w->config >> 24) & 0x3F);
                s->detect = pin_detects(w); s->trigger = (w->pincaps & PC_TRIGGER) != 0; s->fixed = (w->config >> 30) == 2;
                s->pincaps = w->pincaps;
            } else if ((w->type == W_MIX || w->type == W_SEL) && depth[t] + 2U <= MAX_PATH) queue[tail++] = (uint8_t)t;
        }
    }
    return found;
}
static void cap_amp(unsigned nid, uint32_t caps, bool output, unsigned index, bool mute, unsigned gain) {
    unsigned steps = amp_steps(caps); if (gain > steps) gain = steps;
    set(hcap.codec, nid, V_SET_AMP, (output ? 0x8000U : 0x4000U) | 0x3000U | (index & 15) << 8 | (mute ? 0x80U : 0) | gain);
}
/* Gain of the amp at route position k for the current controls (channel-wise for the volume amp). */
static void capture_amps(const struct capture_source* s) {
    struct sound_engine* e = hcap.engine;
    bool muted = hcap.switch_control >= 0 && !e->control[hcap.switch_control].value[0];
    for (unsigned k = 0; k + 1 < s->length; k++) {
        bool volume = (int)k == hcap.vol_k;
        if (s->caps[k] & WC_IN_AMP) {
            if (s->conns[k] > 1 && (s->caps[k] >> 20 & 15) == W_MIX)
                for (unsigned j = 0; j < s->conns[k] && j < 16; j++) {
                    if (j == s->index[k] && volume && !hcap.vol_output && hcap.volume_control >= 0) continue;
                    cap_amp(s->node[k], s->amp_in[k], false, j, j != s->index[k] || (volume && muted), amp_offset(s->amp_in[k]));
                }
            if (!(volume && !hcap.vol_output && hcap.volume_control >= 0))
                cap_amp(s->node[k], s->amp_in[k], false, s->index[k], false, amp_offset(s->amp_in[k]));
        }
        if ((s->caps[k] & WC_OUT_AMP) && !(volume && hcap.vol_output && hcap.volume_control >= 0))
            cap_amp(s->node[k], s->amp_out[k], true, 0, false, amp_offset(s->amp_out[k]));
    }
    if (hcap.vol_k >= 0 && hcap.volume_control >= 0) {                  /* the volume amp, left and right separately */
        unsigned k = (unsigned)hcap.vol_k; const struct sound_control* v = &e->control[hcap.volume_control];
        uint32_t caps = hcap.vol_output ? s->amp_out[k] : s->amp_in[k]; unsigned index = hcap.vol_output ? 0 : s->index[k];
        for (unsigned ch = 0; ch < 2; ch++)
            set(hcap.codec, s->node[k], V_SET_AMP, (hcap.vol_output ? 0x8000U : 0x4000U) | (ch ? 0x1000U : 0x2000U) | (index & 15) << 8 |
                (muted ? 0x80U : 0) | ((unsigned)v->value[ch] <= amp_steps(caps) ? (unsigned)v->value[ch] : amp_steps(caps)));
    }
    unsigned pin = s->length - 1;
    if (s->caps[pin] & WC_IN_AMP) {                                      /* pin boost */
        unsigned gain = hcap.boost_control >= 0 && s->device == DEV_MIC ? (unsigned)e->control[hcap.boost_control].value[0] : 0;
        cap_amp(s->pin, s->amp_in[pin], false, 0, false, gain);
    }
}
static void capture_select(unsigned which) {
    const struct capture_source* s = &hcap.source[which];
    hcap.current = which;
    for (unsigned k = 0; k < s->length; k++) {
        if (s->caps[k] & WC_POWER) set(hcap.codec, s->node[k], V_SET_POWER, 0);
        if (k + 1 < s->length && (s->caps[k] >> 20 & 15) != W_MIX && s->conns[k] > 1) set(hcap.codec, s->node[k], V_SET_SEL, s->index[k]);
    }
    uint8_t control = 0x20;                                             /* input enable */
    if (s->device == DEV_MIC) control |= s->pincaps & PC_VREF_80 ? 4 : s->pincaps & PC_VREF_50 ? 1 : s->pincaps & PC_VREF_100 ? 5 : 0;
    set(hcap.codec, s->pin, V_SET_PINCTL, control);
    capture_amps(s);
    set(hcap.codec, s->node[0], V_SET_FORMAT, FORMAT);
    set(hcap.codec, s->node[0], V_SET_STREAM, CAPTURE_TAG << 4);
}
static unsigned capture_best(void) {
    unsigned best = 0; int best_score = -1;
    for (unsigned i = 0; i < hcap.sources; i++) {
        const struct capture_source* s = &hcap.source[i];
        bool present = s->detect && pin_sense(hcap.codec, s->pin, s->trigger);
        int score = source_score(s, present);
        if (score > best_score) { best_score = score; best = i; }
    }
    return best;
}
/* "codec 0 capture pin 0x19 -> mix 0x23 -> adc 0x08 (front mic, jack present)" */
static void capture_log(const char* why) {
    const struct capture_source* s = &hcap.source[hcap.current];
    prefix(); vga_print("codec "); put_hex(hcap.codec, 1, false); vga_print(why); vga_print(" pin 0x"); put_hex(s->pin, 2, false);
    for (unsigned k = s->length - 1; k-- > 0; ) {
        unsigned type = (s->caps[k] >> 20) & 15;
        vga_print(type == W_IN ? " -> adc 0x" : type == W_SEL ? " -> sel 0x" : " -> mix 0x"); put_hex(s->node[k], 2, false);
    }
    vga_print(" ("); vga_print(s->fixed || !s->detect ? (s->device == DEV_MIC ? "built-in mic" : "line-in") : jack_name(s->device, s->location, false));
    if (!s->fixed && s->detect) vga_print(pin_sense(hcap.codec, s->pin, s->trigger) ? ", jack present" : ", jack empty");
    vga_print(")\n");
}
static void capture_recheck(uint64_t now, bool force) {
    if (!hcap.ok || (!force && now < hcap.next_check_ns)) return;
    hcap.next_check_ns = now + 1000000000ULL;
    unsigned best = capture_best();
    if (best != hcap.current) { capture_select(best); capture_log(" capture switched to"); }
}
/* The codec's capture plan: the ADC reaching most usable input pins (48 kHz/16-bit capable), its sources, controls. */
static bool capture_setup(const struct codec* c, struct sound_engine* engine) {
    struct capture_source found[MAX_SOURCES]; unsigned best_count = 0; int best_adc = -1;
    for (unsigned i = 0; i < c->count; i++) {
        const struct widget* w = &c->w[i];
        if (w->type != W_IN || (w->caps & WC_DIGITAL) || ((w->pcm & ((1U << 6) | (1U << 17))) != ((1U << 6) | (1U << 17)) && w->pcm)) continue;
        unsigned count = capture_routes(c, i, found, MAX_SOURCES);
        if (count > best_count) { best_count = count; best_adc = (int)i; }
    }
    if (best_adc < 0) return false;
    memset(&hcap, 0, sizeof(hcap));
    hcap.codec = c->address; hcap.sources = capture_routes(c, (unsigned)best_adc, hcap.source, MAX_SOURCES);
    hcap.engine = live_engine ? live_engine : engine;
    hcap.vol_k = -1; hcap.volume_control = hcap.switch_control = hcap.boost_control = -1;
    /* volume amp: ADC input amp, else the first selector/mixer amp with steps (from the ADC side) */
    const struct capture_source* s = &hcap.source[0];
    for (unsigned k = 0; k + 1 < s->length && hcap.vol_k < 0; k++) {
        if ((s->caps[k] & WC_IN_AMP) && amp_steps(s->amp_in[k])) { hcap.vol_k = (int)k; hcap.vol_output = false; }
        else if ((s->caps[k] & WC_OUT_AMP) && amp_steps(s->amp_out[k])) { hcap.vol_k = (int)k; hcap.vol_output = true; }
    }
    if (hcap.vol_k >= 0) {
        uint32_t caps = hcap.vol_output ? s->amp_out[hcap.vol_k] : s->amp_in[hcap.vol_k];
        int step = (int)(((caps >> 16) & 0x7F) + 1) * 25;
        struct sound_control* v = &engine->control[engine->controls]; hcap.volume_control = (int)engine->controls++;
        strcpy(v->name, "Capture Volume"); v->type = SOUND_CONTROL_INTEGER; v->channels = 2; v->min = 0; v->max = (int32_t)amp_steps(caps);
        v->value[0] = v->value[1] = (int32_t)amp_offset(caps); v->db_step = step; v->db_min = -(int32_t)amp_offset(caps) * step;
        if (caps & 0x80000000U) {
            struct sound_control* m = &engine->control[engine->controls]; hcap.switch_control = (int)engine->controls++;
            strcpy(m->name, "Capture Switch"); m->type = SOUND_CONTROL_BOOLEAN; m->channels = 1; m->min = 0; m->max = 1; m->value[0] = 1;
        }
    }
    for (unsigned i = 0; i < hcap.sources && hcap.boost_control < 0; i++) {
        const struct capture_source* x = &hcap.source[i]; unsigned pin = x->length - 1;
        if (x->device != DEV_MIC || !(x->caps[pin] & WC_IN_AMP) || !amp_steps(x->amp_in[pin])) continue;
        struct sound_control* b = &engine->control[engine->controls]; hcap.boost_control = (int)engine->controls++;
        strcpy(b->name, "Mic Boost Volume"); b->type = SOUND_CONTROL_INTEGER; b->channels = 1; b->min = 0; b->max = (int32_t)amp_steps(x->amp_in[pin]);
        b->value[0] = 1; b->db_step = (int32_t)(((x->amp_in[pin] >> 16) & 0x7F) + 1) * 25; b->db_min = -(int32_t)amp_offset(x->amp_in[pin]) * b->db_step;
    }
    hcap.ok = hcap.sources > 0;
    if (!hcap.ok) return false;
    capture_select(capture_best());
    capture_log(" capture");
    return true;
}
static bool capture_control(unsigned index) {
    (void)index;
    if (!hcap.ok || !codec_alive(hcap.codec)) return false;
    capture_amps(&hcap.source[hcap.current]);
    return true;
}
static bool capture_dma_start(void) {
    if (!hcap.ok || !hcap.ring) return false;
    capture_recheck(native_clock_ns(), true);                          /* pick the source now */
    unsigned sd = hcap.sd;
    stream_stop_at(sd);
    wr8(sd + SD_CTL, (uint8_t)(rd8(sd + SD_CTL) | 1)); wait_reg(sd + SD_CTL, 1, 1, 1, 1000000);
    wr8(sd + SD_CTL, (uint8_t)(rd8(sd + SD_CTL) & ~1U));
    if (!wait_reg(sd + SD_CTL, 1, 1, 0, 1000000)) { prefix(); vga_print("capture stream reset timed out\n"); return false; }
    volatile uint32_t* bdl = (volatile uint32_t*)(hda.dma + CAP_BDL_OFFSET); unsigned entries = 0;
    for (unsigned done = 0; done < SOUND_RING_FRAMES; entries++, done += BDL_CHUNK_FRAMES) {
        uint64_t address = hcap.ring + (uint64_t)done * SOUND_FRAME_BYTES;
        bdl[entries * 4] = (uint32_t)address; bdl[entries * 4 + 1] = (uint32_t)(address >> 32);
        bdl[entries * 4 + 2] = BDL_CHUNK_FRAMES * SOUND_FRAME_BYTES; bdl[entries * 4 + 3] = 0;
    }
    flush_range((const void*)bdl, entries * 16ULL);
    uint64_t bdl_physical = virt_to_phys((const void*)bdl);
    wr32(sd + SD_CBL, SOUND_RING_FRAMES * SOUND_FRAME_BYTES); wr16(sd + SD_LVI, (uint16_t)(entries - 1)); wr16(sd + SD_FMT, FORMAT);
    wr32(sd + SD_BDPL, (uint32_t)bdl_physical); wr32(sd + SD_BDPU, (uint32_t)(bdl_physical >> 32));
    wr8(sd + SD_CTL_TAG, (uint8_t)(CAPTURE_TAG << 4));
    __asm__ volatile("mfence" ::: "memory");
    wr8(sd + SD_CTL, (uint8_t)((rd8(sd + SD_CTL) & ~0x1DU) | 0x02));   /* RUN */
    hcap.bytes = 0; hcap.lpib = 0; hcap.running = true;
    return true;
}
static void capture_dma_stop(void) { if (hcap.running) { stream_stop_at(hcap.sd); hcap.running = false; } }
/* Frames written since the start: LPIB deltas, as hda_read. */
static bool capture_dma_read(uint64_t* position) {
    const uint32_t lap = SOUND_RING_FRAMES * SOUND_FRAME_BYTES;
    uint32_t lpib = rd32(hcap.sd + SD_LPIB) % lap;
    if (lpib == hcap.lpib) {
        uint8_t control = rd8(hcap.sd + SD_CTL), status = rd8(hcap.sd + SD_STS);
        if (!(control & 2) || (status & 0x10)) { prefix(); vga_print("capture stream stopped, status 0x"); put_hex(status, 2, false); vga_print("\n"); hcap.running = false; return false; }
    }
    hcap.bytes += lpib >= hcap.lpib ? lpib - hcap.lpib : lpib + lap - hcap.lpib;
    hcap.lpib = lpib;
    uint64_t frames = hcap.bytes / SOUND_FRAME_BYTES;     /* LPIB may run a FIFO ahead of memory: hold 64 frames back */
    *position = frames > 64 ? frames - 64 : 0;
    capture_recheck(native_clock_ns(), false);
    return true;
}

/* The engine's capture entry points run on the capture controller's instance. */
#define ON_CAPTURE_CONTROLLER(call) do { struct hda_state* saved = hda_ctl; hda_ctl = hcap.ctl; call; hda_ctl = saved; } while (0)
static bool hda_capture_start(void) { bool ok = false; if (hcap.ctl) ON_CAPTURE_CONTROLLER(ok = capture_dma_start()); return ok; }
static void hda_capture_stop(void) { if (hcap.ctl) ON_CAPTURE_CONTROLLER(capture_dma_stop()); }
static bool hda_capture_read(uint64_t* position) { bool ok = false; if (hcap.ctl) ON_CAPTURE_CONTROLLER(ok = capture_dma_read(position)); return ok; }
static bool hda_control_put(unsigned index) { bool ok = false; if (hcap.ctl) ON_CAPTURE_CONTROLLER(ok = capture_control(index)); return ok; }
static void capture_attach(struct sound_engine* engine, uint32_t vendor) {
    hcap.sd = SD_FIRST; hcap.ring = sound_capture_ring; hcap.ctl = hda_ctl;
    strcpy(engine->capture_name, vendor_name(vendor)); add(engine->capture_name, sizeof(engine->capture_name), " Capture");
    engine->capture_start = hda_capture_start; engine->capture_stop = hda_capture_stop; engine->capture_read = hda_capture_read;
    engine->capture_flush = flush_range; engine->control_put = hda_control_put;
}

/* ---------- probe ---------- */
static void release(const pci_device_t* device, uint16_t command) {
    rings_stop();
    wr32(GCTL, rd32(GCTL) & ~1U);                       /* leave the link in reset */
    pci_restore_command(device, command & ~4U);
}
static pci_device_t* resume_device; static int resume_mode;   /* the PCM controller and the mode it was accepted in */
static bool hda_resume(void);
/* An analog plan whose primary output reaches someone: a present jack, or a pin without presence detection. */
static bool plan_connected(const struct codec* c, const struct plan* plan) {
    const struct widget* pin = &c->w[plan->route[0].node[0]];
    return !pin_detects(pin) || pin->present;
}
/* 1 = this controller now drives the PCM in this mode; 0 = nothing for this mode; -1 = unusable in every mode. */
static int controller_probe(pci_device_t* device, int mode, struct codec* c, struct sound_engine* engine) {
    memset(hda.timeouts, 0, sizeof(hda.timeouts)); hda.pins = 0; hda.automute = false; hda.error_logged = false; hda.jacks = 0; hda.unsol = false;
    hda.vendor = device->vendor_id; hda.device = device->device_id;
    uint64_t size, bar = pci_bar_memory(device, 0, &size);
    if (!bar || size < 0x100) { prefix(); vga_print("BAR0 is not a memory BAR, skipped\n"); return -1; }
    hda.mapped = size < 0x4000 ? size : 0x4000; hda.bar = (uint32_t)bar;
    hda.regs = (volatile uint8_t*)pci_mmio_map(bar, hda.mapped);
    if (!hda.regs) { prefix(); vga_print("BAR0 mapping failed\n"); return -1; }
    uint16_t command_word = pci_read16(device->bus, device->device, device->function, PCI_COMMAND);
    pci_write32(device->bus, device->device, device->function, PCI_COMMAND, (command_word | 6U | 0x400U) & 0xFFFFU); /* MMIO + DMA, INTx off */
    if (device->vendor_id == 0x8086) {                  /* Intel TCSEL (config 0x44 bits 2:0): traffic class 0, as Linux does */
        uint32_t tcsel = pci_read32(device->bus, device->device, device->function, 0x44);
        if (tcsel & 7) pci_write32(device->bus, device->device, device->function, 0x44, tcsel & ~7U);
    }
    uint16_t gcap = rd16(GCAP);
    unsigned inputs = (gcap >> 8) & 15, outputs = (gcap >> 12) & 15, bidirectional = (gcap >> 3) & 31;
    hda.ok64 = (gcap & 1) != 0;
    if (gcap == 0xFFFF || (!outputs && !bidirectional) || SD_FIRST + (inputs + 1) * SD_SIZE > hda.mapped) {
        prefix(); vga_print("no usable output stream (GCAP 0x"); put_hex(gcap, 4, false); vga_print(")\n");
        pci_restore_command(device, command_word); return -1;
    }
    if (!hda.ok64 && (virt_to_phys((void*)hda.dma) + 4096 > 0x100000000ULL || hda.ring + SOUND_RING_FRAMES * SOUND_FRAME_BYTES > 0x100000000ULL)) {
        prefix(); vga_print("DMA memory above 4 GiB without 64-bit addressing\n"); pci_restore_command(device, command_word); return -1;
    }
    if (!controller_reset(gcap)) { prefix(); vga_print("controller reset timed out\n"); release(device, command_word); return -1; }
    unsigned codecs = 0;
    for (uint64_t deadline = native_clock_ns() + 25000000ULL; !(codecs = rd16(STATESTS) & 0x7FFF) && native_clock_ns() < deadline;) cpu_pause();
    wr16(STATESTS, (uint16_t)codecs);
    prefix(); vga_print("at 0x"); put_hex(bar, bar >> 32 ? 16 : 8, false); vga_print(" v"); put_hex(rd8(VMAJ), 1, false); vga_print(".");
    put_hex(rd8(VMIN), 1, false); vga_print(" GCAP=0x"); put_hex(gcap, 4, false); vga_print(hda.ok64 ? " 64-bit" : " 32-bit");
    vga_print(" codecs=0x"); put_hex(codecs, 4, false); vga_print("\n");
    if (!codecs) { prefix(); vga_print("no codec attached (STATESTS=0), giving up\n"); release(device, command_word); return -1; }
    hda.immediate = boot_option("hda.immediate");
    if (!hda.immediate) rings_start(); else rings_stop();
    bool digital_seen = false, analog_seen = false, any_codec = false;
    for (unsigned address = 0; address < 15; address++) {
        if (!(codecs >> address & 1)) continue;
        if (!codec_parse(c, address)) {
            prefix(); vga_print("codec "); put_hex(address, 1, false);
            vga_print(c->vendor ? (c->afg ? " did not answer while being parsed\n" : " has no audio function group\n") : " did not answer\n");
            continue;
        }
        prefix(); vga_print("codec "); put_hex(address, 1, false); vga_print(" vendor "); put_hex(c->vendor, 8, false);
        vga_print(" subsystem "); put_hex(c->subsystem, 8, false); vga_print(" rev "); put_hex(c->revision, 8, false);
        char number[12]; uint_to_str(c->count, number);
        vga_print(" afg 0x"); put_hex(c->afg, 2, false); vga_print(" widgets="); vga_print(number); vga_print("\n");
        any_codec = true;
        if (mode == MODE_CAPTURE) {                         /* a second controller, for its input path only */
            if (!inputs || !capture_setup(c, engine)) continue;
            capture_attach(engine, c->vendor); hda.pci = device;
            prefix(); vga_print("capture controller (playback on another controller)\n");
            return 1;
        }
        struct plan plan, digital;
        bool analog = codec_plan(c, false, &plan), accepted = false;
        analog_seen |= analog;
        if (analog && (mode == MODE_ANALOG_ANY || (mode == MODE_ANALOG_CONNECTED && plan_connected(c, &plan)))) accepted = true;
        else if (analog && mode == MODE_ANALOG_CONNECTED) {
            prefix(); vga_print("codec "); put_hex(address, 1, false); vga_print(" analog outputs, no jack connected: HDMI/DP monitors first\n");
        }
        if (!accepted && (mode == MODE_HDMI_MONITOR || (mode == MODE_DIGITAL_ANY && !analog)) && codec_plan(c, true, &digital)) {
            digital_seen = true;
            if (mode == MODE_DIGITAL_ANY || c->w[digital.route[0].node[0]].eld.usable) { plan = digital; accepted = true; }
        } else if (!accepted && !analog && mode == MODE_ANALOG_CONNECTED && codec_plan(c, true, &digital)) {
            digital_seen = true;
            prefix(); vga_print("codec "); put_hex(address, 1, false); vga_print(" has digital outputs only, kept as a fallback\n");
        } else if (!accepted && !analog && mode == MODE_ANALOG_ANY) digital_seen |= codec_plan(c, true, &digital);   /* keep it for the last pass */
        if (!accepted) continue;
        /* Program every route; then drive this codec. */
        hda.codec = address; hda.vendor_codec = c->vendor; hda.revision_codec = c->revision;
        realtek_init(c);
        for (unsigned r = 0; r < plan.count; r++) { program_route(c, &plan.route[r], plan.digital); print_route(c, &plan.route[r], r == 0, plan.digital); }
        if (!codec_alive(address)) { hda.pins = 0; continue; }
        bool speaker = false, headphone = false;
        for (unsigned i = 0; i < hda.pins; i++) { speaker |= hda.pin[i].device == DEV_SPEAKER; headphone |= hda.pin[i].device == DEV_HP && hda.pin[i].detect; }
        hda.automute = speaker && headphone;
        if (hda.automute) {
            bool plugged = headphone_present(); apply_automute(plugged);
            if (plugged) { prefix(); vga_print("headphone plugged: speakers muted\n"); }
        }
        jacks_setup(c);
        hda.next_sense_ns = native_clock_ns() + (hda.unsol && !hda.poll_all ? 2000000000ULL : 300000000ULL);
        unsigned stream = inputs; hda.bidirectional = !outputs;    /* first output (else first bidirectional) descriptor */
        hda.sd = SD_FIRST + stream * SD_SIZE;
        stream_reset();
        memset(engine, 0, sizeof(*engine));
        strcpy(engine->id, "NexusHDA"); strcpy(engine->driver, "NexusHDA"); strcpy(engine->name, "NexusOS HDA");
        strcpy(engine->longname, "NexusOS HD Audio "); add_hex(engine->longname, sizeof(engine->longname), hda.vendor, 4, true);
        add(engine->longname, sizeof(engine->longname), ":"); add_hex(engine->longname, sizeof(engine->longname), hda.device, 4, true);
        add(engine->longname, sizeof(engine->longname), " codec "); add_hex(engine->longname, sizeof(engine->longname), c->vendor, 8, false);
        add(engine->longname, sizeof(engine->longname), " at 0x"); add_hex(engine->longname, sizeof(engine->longname), bar, bar >> 32 ? 16 : 8, false);
        strcpy(engine->mixer, vendor_name(c->vendor)); add(engine->mixer, sizeof(engine->mixer), " codec ");
        add_hex(engine->mixer, sizeof(engine->mixer), c->vendor, 8, false);
        strcpy(engine->components, "HDA:"); add_hex(engine->components, sizeof(engine->components), c->vendor, 8, false);
        add(engine->components, sizeof(engine->components), ","); add_hex(engine->components, sizeof(engine->components), c->subsystem, 8, false);
        add(engine->components, sizeof(engine->components), ","); add_hex(engine->components, sizeof(engine->components), c->revision, 8, false);
        strcpy(engine->pcm_name, vendor_name(c->vendor)); add(engine->pcm_name, sizeof(engine->pcm_name), " ");
        add_hex(engine->pcm_name, sizeof(engine->pcm_name), c->vendor & 0xFFFF, 4, false);
        add(engine->pcm_name, sizeof(engine->pcm_name), plan.digital ? " Digital" : " Analog");
        engine->cyclic = true; engine->stop = hda_stop; engine->start = hda_start; engine->extend = hda_extend; engine->read = hda_read;
        engine->resume = hda_resume; resume_device = device; resume_mode = mode;
        engine->flush = flush_range; engine->poll = hda.jacks ? hda_poll : NULL;
        /* Capture: a codec of this controller with an input path (the driven one first), input stream 0. */
        memset(&hcap, 0, sizeof(hcap));
        if (inputs && sound_capture_ring) {
            bool found = codec_parse(c, address) && capture_setup(c, engine);
            for (unsigned other = 0; other < 15 && !found; other++)
                if (other != address && (codecs >> other & 1)) found = codec_parse(c, other) && capture_setup(c, engine);
            if (found) capture_attach(engine, c->vendor);
        }
        vga_print("[64] HDA native PCM ready: "); vga_print(engine->longname); vga_print(" (stream "); put_hex(stream, 1, false);
        vga_print(hda.bidirectional ? " bidirectional" : ""); vga_print(", fifo "); put_hex(rd16(hda.sd + SD_FIFOS) + 1U, 4, false);
        vga_print(hda.immediate ? ", immediate commands)" : hda.unsol ? ", CORB/RIRB, jack events)" : ", CORB/RIRB)");
        vga_print("\n");
        hda.pci = device;
        return 1;
    }
    if (mode == MODE_ANALOG_CONNECTED || mode == MODE_DIGITAL_ANY) {
        prefix(); vga_print(digital_seen && !analog_seen ? "no analog output; digital outputs kept as a fallback\n" :
                            analog_seen ? "analog output kept for a later pass\n" : "no usable output path, giving up\n");
    }
    release(device, command_word);
    return any_codec && (analog_seen || digital_seen) ? 0 : -1;
}
/* System sleep (sound.c): the controllers lost their state with the power. Probe the PCM controller again in the mode
   it was accepted in at boot (if its jack or monitor went away meanwhile: the relaxed analog/digital mode), then the
   separate capture controller (hda_states[1]) if one records - controller reset, CORB/RIRB, codec parse and routes over
   the same DMA pages with the streams stopped. The probes fill a scratch engine; the capture controls live on in
   live_engine (capture_setup applies them from there). */
static bool hda_resume(void) {
    if (!resume_device) return false;
    uint64_t parse = arena_alloc(sizeof(struct codec)), scratch = arena_alloc(sizeof(struct sound_engine));
    if (!parse || !scratch) { arena_free(parse, sizeof(struct codec)); arena_free(scratch, sizeof(struct sound_engine)); return false; }
    struct hda_state* capture = hcap.ctl == &hda_states[1] && hda_states[1].pci ? &hda_states[1] : NULL;
    int result = controller_probe(resume_device, resume_mode, (struct codec*)parse, (struct sound_engine*)scratch);
    if (result != 1 && (resume_mode == MODE_ANALOG_CONNECTED || resume_mode == MODE_HDMI_MONITOR))
        result = controller_probe(resume_device, resume_mode == MODE_ANALOG_CONNECTED ? MODE_ANALOG_ANY : MODE_DIGITAL_ANY,
                                  (struct codec*)parse, (struct sound_engine*)scratch);
    if (capture) {
        struct hda_state* driven = hda_ctl; pci_device_t* device = (pci_device_t*)capture->pci; uint64_t dma = capture->dma;
        hda_ctl = capture; memset(&hda, 0, sizeof(hda)); hda.dma = dma;
        memset((void*)scratch, 0, sizeof(struct sound_engine));
        if (controller_probe(device, MODE_CAPTURE, (struct codec*)parse, (struct sound_engine*)scratch) != 1) {
            memset(&hcap, 0, sizeof(hcap)); vga_print("[64] HDA capture controller did not come back after resume\n");
        }
        hda_ctl = driven;
    }
    arena_free(parse, sizeof(struct codec)); arena_free(scratch, sizeof(struct sound_engine));
    return result == 1;
}
const pci_device_t* hda_pci(void) { return hda.pci; }   /* the controller driving the PCM (diag.c), NULL if none */
bool hda_probe(struct sound_engine* engine, uint64_t ring) {
    pci_device_t* list = pci_get_devices(); int count = pci_device_count();
    bool any = false;
    for (int i = 0; i < count; i++) any |= list[i].class_code == PCI_CLASS_MULTIMEDIA && list[i].subclass == 3;
    if (!any) return false;
    live_engine = engine;
    uint64_t dma = dma32_alloc(4096), parse = arena_alloc(sizeof(struct codec));
    if (!dma || !parse) { arena_free(dma, 4096); arena_free(parse, sizeof(struct codec)); vga_print("[64] HDA no memory\n"); return false; }
    hda.dma = dma; hda.ring = ring; hda.bdl = (volatile uint32_t*)(dma + BDL_OFFSET);
    /* Passes over every controller (see the header); controllers unusable in one pass are skipped later. */
    static const uint8_t normal[] = {MODE_ANALOG_CONNECTED, MODE_HDMI_MONITOR, MODE_ANALOG_ANY, MODE_DIGITAL_ANY};
    static const uint8_t hdmi_first[] = {MODE_HDMI_MONITOR, MODE_ANALOG_CONNECTED, MODE_ANALOG_ANY, MODE_DIGITAL_ANY};
    static const uint8_t analog_only[] = {MODE_ANALOG_CONNECTED, MODE_ANALOG_ANY, MODE_DIGITAL_ANY};
    const uint8_t* modes = normal; unsigned mode_count = 4;
    if (boot_option("hda.output=hdmi")) modes = hdmi_first;
    else if (boot_option("hda.output=analog")) { modes = analog_only; mode_count = 3; }
    uint64_t unusable[4] = {0, 0, 0, 0};
    int result = 0;
    for (unsigned m = 0; m < mode_count && result != 1; m++)
        for (int i = 0; i < count && result != 1; i++) {
            if (list[i].class_code != PCI_CLASS_MULTIMEDIA || list[i].subclass != 3 || (i < 256 && (unusable[i / 64] >> (i % 64) & 1))) continue;
            result = controller_probe(&list[i], modes[m], (struct codec*)parse, engine);
            if (result < 0 && i < 256) unusable[i / 64] |= 1ULL << (i % 64);
        }
    if (result == 1 && !engine->capture_start && sound_capture_ring) {
        /* No input path on the driven controller (HDMI/DP on a graphics card): the microphone jacks are on another one. */
        struct hda_state* driven = hda_ctl; uint64_t second = dma32_alloc(4096);
        hda_ctl = &hda_states[1];
        for (int i = 0; i < count && second && !engine->capture_start; i++) {
            if (list[i].class_code != PCI_CLASS_MULTIMEDIA || list[i].subclass != 3 || &list[i] == driven->pci) continue;
            memset(&hda, 0, sizeof(hda)); hda.dma = second;
            controller_probe(&list[i], MODE_CAPTURE, (struct codec*)parse, engine);
        }
        if (!engine->capture_start) { memset(&hda, 0, sizeof(hda)); arena_free(second, 4096); }
        hda_ctl = driven;
    }
    arena_free(parse, sizeof(struct codec));
    if (result != 1) { arena_free(dma, 4096); memset(&hda, 0, sizeof(hda)); return false; }
    return true;
}

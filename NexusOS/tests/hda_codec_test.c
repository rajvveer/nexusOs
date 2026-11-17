/* Exercise the production HD Audio driver (arch/x86_64/hda.c) against a simulated controller (CORB/RIRB with QEMU's
   RINTCNT pause semantics, the Immediate Command registers, stream descriptor 4) and simulated codecs modelled on
   real parts: QEMU's hda-output, a Realtek ALC269-style laptop codec (speaker + headphone jack sharing output
   mixers, analog loopback mixer, retaskable mic pin, unused pins), an ALC892-style desktop codec (front/surround
   line-outs, front headphone, S/PDIF, a long-form connection list with a range), an IDT-style codec routed through
   selectors with one converter per pin, an HDMI-only codec, a codec that never answers and a codec-less link.
   Corpus from real codec dumps (tests/fixtures/hda, converted by tests/hda_dump_to_c.py; sources in each file): desktop
   ALC897 (Lenovo), laptop ALC897 (ASUS), MSI's ALC892 on the AMD 1022:15e3 controller, AMD 1002:aa01 HDMI/DP (revisions
   0x100700 and 0x100800, the latter on the Rembrandt 1002:1640 controller of the user's PC) and NVIDIA Blackwell HDMI/DP
   (10de:00ab, the nvhdmi model of the RTX 5070's 10de:00af). The simulator adds unsolicited responses (GCTL.UNSOL, verb
   0x708 tags), Realtek processing coefficients, ELD buffers (GET_HDMI_DIP_SIZE / GET_HDMI_ELDD, presence + ELDV), the AMD
   ELD-emulation, slot and channel-allocation verbs, DP-MST device select, ASP slot mapping, and several controllers
   (the user's PC: AMD analog HDA, AMD HDMI, NVIDIA HDMI).
   Capture: QEMU's hda-duplex/hda-micro, a Conexant-style laptop codec whose ADC selects among its pins (headset mic
   jack, dock line-in, built-in mic) and the corpus' ALC897/ALC892 front/rear mic + line-in behind mixer 0x23; the
   simulated ADC writes a frame counter through input stream descriptor 0's BDL and moves LPIB. */
#define HDA_TEST_MMIO 1
#define NEXUS_HOST_TEST 1 /* pointers are bus addresses (native.h) */
#include "../arch/x86_64/native.h"
#include "../kernel/string.h"
#undef NULL
#include <stdio.h>
#include <stdlib.h>
#undef NDEBUG
#include <assert.h>

/* ---------- simulated codecs ---------- */
struct sim_node {
    unsigned nid; uint32_t caps, pincaps, config, amp_in, amp_out, pcm, conn_len; uint16_t conn[16]; bool sense;
    uint32_t pinctl, select, eapd, stream, format, digi1, channels, power, out_amp, in_amp[16], senses; bool selected, streamed;
    uint8_t dip[16]; unsigned dips, dip_xmit;
    /* jack events, HDMI/DP sinks */
    uint32_t unsol, devices, devsel; bool eldv; uint8_t eld[300]; unsigned eld_size, eld_bad_byte;
    uint32_t slot[8], ati_mc[0x12], ati_ca, ati_downmix, ati_mode, ati_ramp, ati_spkalloc, ati_sad, ati_sad_index; char ati_name[24];
    unsigned ati_sink_index; bool ati_written;
};
struct sim_codec {
    bool present, dead, limited; unsigned budget;    /* limited: stops answering after `budget` verbs */
    uint32_t vendor, subsystem, revision, afg_amp_in, afg_amp_out, afg_pcm, afg_power; unsigned afg, first, count;
    struct sim_node node[64];
    uint16_t coef[256]; unsigned coef_index, coef_writes;
};
/* Controllers: the driver maps one BAR at a time, so `cur` (chosen by pci_mmio_map) holds the active one. */
struct sim_controller {
    uint8_t s_regs[0x4000]; struct sim_codec s_codec[16];
    unsigned s_corb_rp, s_rirb_wp, s_rirb_count, s_corb_processed; uint16_t s_command; uint32_t bar; uint16_t vendor, device;
};
static struct sim_controller controllers[3], *cur = &controllers[0];
static uint8_t* regs = controllers[0].s_regs;
static void use(struct sim_controller* k) { cur = k; regs = k->s_regs; }
static unsigned controller_count = 1;
#define sim (cur->s_codec)
#define corb_rp (cur->s_corb_rp)
#define rirb_wp (cur->s_rirb_wp)
#define rirb_count (cur->s_rirb_count)
#define corb_processed (cur->s_corb_processed)
#define pci_command (cur->s_command)
static unsigned verbs_answered;
static struct sim_node* sim_node(unsigned address, unsigned nid) {
    struct sim_codec* c = &sim[address];
    return nid >= c->first && nid < c->first + c->count ? &c->node[nid - c->first] : NULL;
}
static void sim_codec(unsigned address, uint32_t vendor, unsigned first, unsigned count) {
    struct sim_codec* c = &sim[address]; memset(c, 0, sizeof(*c));
    c->present = true; c->vendor = vendor; c->subsystem = 0x10251234; c->revision = 0x00100101; c->afg = 1; c->first = first; c->count = count;
    c->afg_pcm = 0x000E0560;
    for (unsigned i = 0; i < count; i++) c->node[i].nid = first + i;   /* type 0xF (vendor) until defined */
    for (unsigned i = 0; i < count; i++) c->node[i].caps = 0xF00000;
}
static struct sim_node* define(unsigned address, unsigned nid, uint32_t caps) { struct sim_node* n = sim_node(address, nid); assert(n); n->caps = caps; return n; }
static void connect(struct sim_node* n, unsigned count, const uint16_t* raw, bool wide) {
    n->caps |= 1U << 8; n->conn_len = count | (wide ? 0x80 : 0); memcpy(n->conn, raw, count * sizeof(uint16_t));
}
#define CONNECT(n, ...) do { static const uint16_t list[] = {__VA_ARGS__}; connect(n, sizeof(list) / sizeof(list[0]), list, false); } while (0)
enum { T_OUT = 0, T_IN = 1 << 20, T_MIX = 2 << 20, T_SEL = 3 << 20, T_PIN = 4 << 20 };
enum { C_STEREO = 1, C_IN_AMP = 2, C_OUT_AMP = 4, C_AMP_OVR = 8, C_FMT_OVR = 16, C_DIGITAL = 1 << 9, C_POWER = 1 << 10 };
static struct sim_node* dac(unsigned address, unsigned nid, bool digital) {
    struct sim_node* n = define(address, nid, T_OUT | C_STEREO | C_FMT_OVR | (digital ? C_DIGITAL : C_OUT_AMP | C_AMP_OVR | C_POWER));
    n->amp_out = 0x00025757; n->pcm = 0x000E0560; return n;
}
static struct sim_node* pin(unsigned address, unsigned nid, uint32_t pincaps, uint32_t config) {
    struct sim_node* n = define(address, nid, T_PIN | C_STEREO | C_OUT_AMP | C_AMP_OVR | (1U << 7)); n->pincaps = pincaps; n->config = config; n->amp_out = 0x80000000; return n;   /* unsolicited-capable */
}
static uint32_t sim_param(struct sim_codec* c, unsigned nid, unsigned id) {
    if (nid == 0) return id == 0x00 ? c->vendor : id == 0x02 ? c->revision : id == 0x04 ? (uint32_t)c->afg << 16 | 1 : 0;
    if (nid == c->afg) return id == 0x05 ? 1 : id == 0x04 ? c->first << 16 | c->count : id == 0x0D ? c->afg_amp_in : id == 0x12 ? c->afg_amp_out : id == 0x0A ? c->afg_pcm : 0;
    struct sim_node* n = nid >= c->first && nid < c->first + c->count ? &c->node[nid - c->first] : NULL;
    if (!n) return 0;
    return id == 0x09 ? n->caps : id == 0x0C ? n->pincaps : id == 0x0D ? n->amp_in : id == 0x12 ? n->amp_out : id == 0x0E ? n->conn_len : id == 0x0A ? n->pcm :
           id == 0x15 ? n->devices : 0;
}
static bool amd(const struct sim_codec* c) { return c->vendor == 0x1002AA01; }
static bool sim_verb(uint32_t word, uint32_t* response) {
    unsigned address = word >> 28, nid = (word >> 20) & 0xFF, top = (word >> 16) & 0xF, verb, payload;
    struct sim_codec* c = &sim[address];
    if (!c->present || c->dead) return false;
    if (c->limited) { if (!c->budget) return false; c->budget--; }
    if (top == 0x7 || top == 0xF) { verb = (word >> 8) & 0xFFF; payload = word & 0xFF; } else { verb = top; payload = word & 0xFFFF; }
    struct sim_node* n = sim_node(address, nid);
    *response = 0; verbs_answered++;
    switch (verb) {
    case 0xF00: *response = sim_param(c, nid, payload); break;
    case 0xF20: *response = c->subsystem; break;
    case 0xF05: *response = nid == c->afg ? c->afg_power << 4 | c->afg_power : n ? n->power << 4 | n->power : 0; break;
    case 0x705: if (nid == c->afg) c->afg_power = payload & 3; else if (n) n->power = payload & 3; break;
    case 0xF02: {
        assert(n && (n->caps & (1U << 8)));
        bool wide = n->conn_len & 0x80; unsigned per = wide ? 2 : 4;
        for (unsigned i = 0; i < per && payload + i < (n->conn_len & 0x7F); i++) *response |= (uint32_t)n->conn[payload + i] << (i * (wide ? 16 : 8));
        break;
    }
    case 0xF1C: *response = n ? n->config : 0; break;
    case 0xF09: { assert(n); n->senses++; bool here = n->sense && !(n->devices && n->devsel);         /* DP-MST: the selected device */
        *response = (here ? 0x80000000U : 0) | (here && n->eldv ? 0x40000000U : 0); break; }
    case 0x709: break;
    case 0x708: assert(n && (n->caps & (1U << 7))); n->unsol = payload; break;                    /* unsolicited-capable widgets only */
    case 0x5: assert(nid == 0x20 && (c->vendor >> 16) == 0x10EC); c->coef_index = payload & 0xFF; break;
    case 0xC: assert(nid == 0x20); *response = c->coef[c->coef_index]; break;
    case 0x4: assert(nid == 0x20); c->coef[c->coef_index] = (uint16_t)payload; c->coef_writes++; break;
    case 0x735: assert(n && n->devices); n->devsel = payload; break;
    case 0x734: assert(n && !amd(c)); n->slot[payload & 7] = payload >> 4; break;
    case 0xF2E: assert(n && payload == 0x08 && !amd(c)); *response = n->devices && n->devsel ? 0 : n->eld_size; break;   /* stale when !ELDV */
    case 0xF2F: assert(n && !amd(c)); *response = payload < n->eld_size && !(n->devices && n->devsel) ? (payload == n->eld_bad_byte ? 0 : 0x80000000U) | n->eld[payload] : 0; break;
    case 0x770: assert(n && amd(c) && (n->caps >> 20) == 0); n->ati_ramp = payload; break;
    case 0x771: assert(n && amd(c)); n->ati_ca = payload; n->ati_written = true; break;
    case 0x772: assert(n && amd(c)); n->ati_downmix = payload; n->ati_written = true; break;
    case 0x776: assert(n && amd(c)); n->ati_sad_index = payload; break;
    case 0x777: case 0x778: case 0x779: case 0x77A: case 0x785: case 0x786: case 0x787: case 0x788: assert(n && amd(c)); n->ati_mc[verb - 0x777] = payload; break;
    case 0x780: assert(n && amd(c)); n->ati_sink_index = payload; break;
    case 0x789: assert(n && amd(c)); n->ati_mode = payload; break;
    case 0xF70: assert(n && amd(c)); *response = n->sense && n->eldv ? n->ati_spkalloc : 0; break;
    case 0xF76: assert(n && amd(c)); *response = n->ati_sad_index == (1U << 3) ? n->ati_sad : 0; break;
    case 0xF7B: assert(n && amd(c)); *response = 0; break;
    case 0xF81: assert(n && amd(c)); { unsigned length = (unsigned)strlen(n->ati_name);
        *response = n->ati_sink_index == 2 ? length : n->ati_sink_index >= 5 && n->ati_sink_index < 5 + length ? (uint8_t)n->ati_name[n->ati_sink_index - 5] : 0; break; }
    case 0xF0C: *response = n ? n->eapd : 0; break;
    case 0x70C: assert(n); n->eapd = payload; break;
    case 0x707: assert(n); n->pinctl = payload; break;
    case 0x701: assert(n); n->select = payload; n->selected = true; break;
    case 0x706: assert(n); n->stream = payload; n->streamed = true; break;
    case 0x2: assert(n); n->format = payload; break;
    case 0x3: assert(n); if (payload & 0x8000) n->out_amp = payload; if (payload & 0x4000) n->in_amp[(payload >> 8) & 15] = payload; break;
    case 0x70D: assert(n); n->digi1 = payload; break;
    case 0x72D: assert(n); n->channels = payload; break;
    case 0x730: assert(n); n->dips = 0; break;
    case 0x731: assert(n && n->dips < 16 && !amd(c)); n->dip[n->dips++] = (uint8_t)payload; break;
    case 0x732: assert(n); n->dip_xmit = payload; break;
    default: fprintf(stderr, "unexpected verb %08x\n", word); assert(0);
    }
    return true;
}

/* ---------- simulated controller (registers) ---------- */
static uint32_t get32(unsigned r) { uint32_t v; memcpy(&v, regs + r, 4); return v; }
static uint16_t get16(unsigned r) { uint16_t v; memcpy(&v, regs + r, 2); return v; }
static void put32(unsigned r, uint32_t v) { memcpy(regs + r, &v, 4); }
static void put16(unsigned r, uint16_t v) { memcpy(regs + r, &v, 2); }
static void controller_power_on(void) {
    memset(regs, 0, sizeof(cur->s_regs)); put16(0x00, 0x4401); regs[0x02] = 0; regs[0x03] = 1; regs[0x4E] = 0x42; regs[0x5E] = 0x42;
    corb_rp = rirb_wp = rirb_count = 0;
}
static void corb_run(void) {
    if (!(regs[0x4C] & 2) || !(regs[0x5C] & 2)) return;
    uint32_t* corb = (uint32_t*)(uintptr_t)(get32(0x40) | (uint64_t)get32(0x44) << 32);
    uint32_t* rirb = (uint32_t*)(uintptr_t)(get32(0x50) | (uint64_t)get32(0x54) << 32);
    while ((corb_rp & 0xFF) != get16(0x48)) {
        if (rirb_count == get16(0x5A)) return;                           /* QEMU: paused until RIRBSTS.RINTFL is cleared */
        corb_rp = (corb_rp + 1) & 0xFF; put16(0x4A, (uint16_t)corb_rp); corb_processed++;
        uint32_t word = corb[corb_rp], response;
        if (!sim_verb(word, &response)) continue;                        /* a dead codec never answers */
        rirb_wp = (rirb_wp + 1) & 0xFF; rirb[2 * rirb_wp] = response; rirb[2 * rirb_wp + 1] = word >> 28; put16(0x58, (uint16_t)rirb_wp);
        if (++rirb_count == get16(0x5A) && (regs[0x5C] & 1)) regs[0x5D] |= 1;
    }
}
/* A pin's presence changed: an unsolicited response (tag << 26) when the codec enabled one and GCTL.UNSOL is set. */
static void rirb_push(uint32_t response, unsigned extended) {
    uint32_t* rirb = (uint32_t*)(uintptr_t)(get32(0x50) | (uint64_t)get32(0x54) << 32);
    rirb_wp = (rirb_wp + 1) & 0xFF; rirb[2 * rirb_wp] = response; rirb[2 * rirb_wp + 1] = extended; put16(0x58, (uint16_t)rirb_wp);
    if (++rirb_count == get16(0x5A) && (regs[0x5C] & 1)) regs[0x5D] |= 1;
}
static unsigned unsol_sent;
static void jack(unsigned address, unsigned nid, bool present) {
    struct sim_node* n = sim_node(address, nid); assert(n);
    n->sense = present;
    if ((n->unsol & 0x80) && (get32(0x08) & 0x100) && (regs[0x5C] & 2)) { rirb_push((n->unsol & 0x3F) << 26 | (present ? 1 : 0), 0x10 | address); unsol_sent++; }
}
static void written(unsigned r, unsigned width, uint32_t old) {
    if (r == 0x08) {                                                     /* GCTL */
        if (!(get32(0x08) & 1)) { uint32_t gctl = get32(0x08); controller_power_on(); put32(0x08, gctl); }
        else if (!(old & 1)) { uint16_t mask = 0; for (unsigned i = 0; i < 15; i++) mask |= (uint16_t)(sim[i].present << i); put16(0x0E, mask); }
    } else if (r == 0x0E) put16(0x0E, (uint16_t)(old & ~get16(0x0E)));    /* STATESTS: write 1 to clear */
    else if (r == 0x4A) { if (get16(0x4A) & 0x8000) corb_rp = 0; }      /* CORBRP reset (latches until 0 is written) */
    else if (r == 0x48) corb_run();
    else if (r == 0x58) { if (get16(0x58) & 0x8000) { rirb_wp = 0; put16(0x58, 0); } }
    else if (r == 0x5D) {                                                /* RIRBSTS: write 1 to clear */
        uint8_t value = regs[0x5D]; regs[0x5D] = (uint8_t)(old & ~value);
        if ((old & 1) && !(regs[0x5D] & 1)) { rirb_count = 0; corb_run(); }
    } else if (r == 0x68) {                                              /* ICIS: IRV write 1 to clear, ICB sends */
        uint16_t value = get16(0x68); put16(0x68, (uint16_t)(old & ~value & 2));
        if (value & 1) { uint32_t response; if (sim_verb(get32(0x60), &response)) { put32(0x64, response); put16(0x68, 2); } }
    } else if (r == 0x4C || r == 0x5C) corb_run();
    else if (r >= 0x80 && (r - 0x80) % 0x20 == 0 && width == 1 && (regs[r] & 1)) put32(r + 0x04, 0);   /* SD_CTL.SRST clears LPIB */
    else if (r >= 0x80 && (r - 0x80) % 0x20 == 3 && width == 1) regs[r] = (uint8_t)(old & ~regs[r]);    /* SD_STS: write 1 to clear */
    (void)width;
}
static void follow_driver(void);   /* MMIO goes to the controller the driver instance (hda_ctl) has mapped */
static uint8_t rd8(unsigned r) { follow_driver(); return regs[r]; }
static uint16_t rd16(unsigned r) { follow_driver(); return get16(r); }
static uint32_t rd32(unsigned r) { follow_driver(); return get32(r); }
static void wr8(unsigned r, uint8_t v) { follow_driver(); uint32_t old = regs[r]; regs[r] = v; written(r, 1, old); }
static void wr16(unsigned r, uint16_t v) { follow_driver(); uint32_t old = get16(r); put16(r, v); written(r, 2, old); }
static void wr32(unsigned r, uint32_t v) { follow_driver(); uint32_t old = get32(r); put32(r, v); written(r, 4, old); }

/* ---------- kernel/PCI stubs ---------- */
#include "../arch/x86_64/hda.c"
static void follow_driver(void) {
    for (unsigned i = 0; i < controller_count; i++) if (hda.regs == (volatile uint8_t*)controllers[i].s_regs) { if (cur != &controllers[i]) use(&controllers[i]); return; }
}
static char log_text[1 << 16]; static unsigned log_used;
void vga_print(const char* text) { size_t n = strlen(text); if (log_used + n < sizeof(log_text)) { memcpy(log_text + log_used, text, n); log_used += (unsigned)n; log_text[log_used] = 0; } fputs(text, stdout); }
void uint_to_str(uint32_t value, char* buffer) { sprintf(buffer, "%u", value); }
static uint64_t clock_ns;
uint64_t native_clock_ns(void) { return clock_ns += 1000; }
uint64_t arena_alloc(uint64_t bytes) {
    uint8_t* block = calloc(1, bytes + 4096); assert(block);
    return ((uint64_t)(uintptr_t)block + 4095) & ~4095ULL;
}
void arena_free(uint64_t address, uint64_t bytes) { (void)address; (void)bytes; }
uint64_t dma32_alloc(uint64_t bytes) { return arena_alloc(bytes); }
uint64_t sound_capture_ring;
static bool force_immediate; static const char* output_option;
bool boot_option(const char* word) { return (force_immediate && !strcmp(word, "hda.immediate")) || (output_option && !strcmp(word, output_option)); }
static pci_device_t devices[3];
pci_device_t* pci_get_devices(void) { return devices; }
int pci_device_count(void) { return (int)controller_count; }
static struct sim_controller* by_slot(unsigned slot) { assert(slot >= 4 && slot < 4 + controller_count); return &controllers[slot - 4]; }
uint64_t pci_bar_memory(const pci_device_t* device, unsigned index, uint64_t* size) { assert(index == 0); *size = 0x4000; return by_slot(device->device)->bar; }
volatile void* pci_mmio_map(uint64_t physical, uint64_t size) {
    assert(size == 0x4000);
    for (unsigned i = 0; i < controller_count; i++) if (controllers[i].bar == physical) { use(&controllers[i]); return regs; }
    assert(!"unknown BAR"); return NULL;
}
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) { (void)bus; (void)func; assert(offset == PCI_COMMAND); return by_slot(dev)->s_command; }
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) { (void)bus; (void)func; return offset == 0x44 && by_slot(dev)->vendor == 0x8086 ? 0x00000003 : 0; }
void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value) {
    (void)bus; (void)func;
    if (offset == PCI_COMMAND) by_slot(dev)->s_command = (uint16_t)value; else assert(offset == 0x44 && value == 0);   /* Intel TCSEL cleared */
}
void pci_restore_command(const pci_device_t* device, uint16_t command) { by_slot(device->device)->s_command = command; }

/* ---------- scenarios ---------- */
static struct sound_engine engine;
static uint64_t ring;
static bool probe(void) {
    memset(&engine, 0, sizeof(engine)); log_used = 0; log_text[0] = 0; verbs_answered = 0;
    static const uint16_t ids[3][2] = {{0x8086, 0x2668}, {0x1002, 0x1640}, {0x10DE, 0x2F80}};
    for (unsigned i = 0; i < controller_count; i++) {
        struct sim_controller* k = &controllers[i]; use(k); controller_power_on(); corb_processed = 0; pci_command = 0;
        if (!k->vendor) { k->vendor = ids[i][0]; k->device = ids[i][1]; }
        k->bar = 0xFEB00000U + i * 0x10000U;
        devices[i] = (pci_device_t){.bus = 0, .device = (uint8_t)(4 + i), .function = (uint8_t)(i ? 1 : 0), .vendor_id = k->vendor, .device_id = k->device, .class_code = 4, .subclass = 3};
    }
    use(&controllers[0]); memset(hda_states, 0, sizeof(hda_states)); hda_ctl = &hda_states[0]; memset(&hcap, 0, sizeof(hcap));
    bool ok = hda_probe(&engine, ring);
    assert(hda_ctl == &hda_states[0]); follow_driver();
    if (ok) assert((pci_command & 0x406) == 0x406);                     /* MMIO + bus master, INTx disabled (driven controller) */
    return ok;
}
static bool logged(const char* text) { return strstr(log_text, text) != NULL; }
static void qemu_output(unsigned address) {
    sim_codec(address, 0x1AF40012, 2, 2);
    struct sim_node* d = dac(address, 2, false); d->caps = T_OUT | C_STEREO | C_FMT_OVR | C_AMP_OVR | C_OUT_AMP; d->amp_out = 0x80034A4A; d->pcm = 0x000201FC;
    struct sim_node* p = define(address, 3, T_PIN | C_STEREO); p->pincaps = 0x10; p->config = 0x00004010; CONNECT(p, 2);
}
static void alc269(unsigned address, bool headphone) {
    sim_codec(address, 0x10EC0269, 2, 0x22);                         /* nodes 0x02..0x23 */
    dac(address, 0x02, false); dac(address, 0x03, false);
    struct sim_node* aamix = define(address, 0x0B, T_MIX | C_STEREO | C_IN_AMP | C_AMP_OVR); aamix->amp_in = 0x80051F17; CONNECT(aamix, 0x18, 0x19, 0x1A, 0x1B, 0x1D);
    struct sim_node* m = define(address, 0x0C, T_MIX | C_STEREO | C_IN_AMP); CONNECT(m, 0x02, 0x0B);
    m = define(address, 0x0D, T_MIX | C_STEREO | C_IN_AMP); CONNECT(m, 0x03, 0x0B);
    sim[address].afg_amp_in = 0x80000000;                               /* mixers: mute-only input amps (AFG default) */
    struct sim_node* p = pin(address, 0x12, 0x20, 0x90A60140);          /* internal digital mic */
    p = pin(address, 0x14, 0x00010014, 0x90170110); CONNECT(p, 0x0C, 0x0D);       /* speaker: fixed, no jack detect (misc bit 8) */
    p = pin(address, 0x21, 0x0001001C, 0x0221101F); CONNECT(p, 0x0C, 0x0D); p->sense = headphone; /* headphone jack */
    p = pin(address, 0x18, 0x00003734, 0x02A11030); CONNECT(p, 0x0C, 0x0D);       /* mic jack, output-capable */
    for (unsigned nid = 0x17; nid <= 0x1E; nid++) if (nid != 0x18 && nid != 0x1B) { p = pin(address, nid, 0x10, 0x411111F0); CONNECT(p, 0x0C); }
    p = pin(address, 0x1B, 0x00003734, 0x411111F0); CONNECT(p, 0x0C, 0x0D);
    p = define(address, 0x1E, T_PIN | C_STEREO | C_DIGITAL); p->pincaps = 0x10; p->config = 0x411111F0; CONNECT(p, 0x06);
}
static void alc892(unsigned address) {
    sim_codec(address, 0x10EC0892, 2, 0x25);                         /* nodes 0x02..0x26; 0x26 stays vendor-defined */
    for (unsigned nid = 0x02; nid <= 0x05; nid++) dac(address, nid, false);
    dac(address, 0x06, true);
    struct sim_node* aamix = define(address, 0x0B, T_MIX | C_STEREO | C_IN_AMP | C_AMP_OVR); aamix->amp_in = 0x80051F17;
    static const uint16_t wide[] = {0x0018, 0x801B, 0x001D}; connect(aamix, 3, wide, true);   /* long form with a range */
    for (unsigned i = 0; i < 4; i++) { struct sim_node* m = define(address, 0x0C + i, T_MIX | C_STEREO | C_IN_AMP); m->conn[0] = (uint16_t)(0x02 + i); m->conn[1] = 0x0B; m->caps |= 1U << 8; m->conn_len = 2; }
    sim[address].afg_amp_in = 0x80000000;
    struct sim_node* p = pin(address, 0x14, 0x00010014, 0x01014010); CONNECT(p, 0x0C);   /* rear green, front L/R */
    p = pin(address, 0x15, 0x00010014, 0x01011012); CONNECT(p, 0x0D);                    /* rear surround (seq 2) */
    p = pin(address, 0x16, 0x00010014, 0x01016011); CONNECT(p, 0x0E);                    /* C/LFE (seq 1) */
    p = pin(address, 0x17, 0x00010014, 0x01012014); CONNECT(p, 0x0F);                    /* side (seq 4) */
    p = pin(address, 0x1B, 0x0001001C, 0x0221401F); CONNECT(p, 0x0C, 0x0D, 0x0E, 0x0F, 0x26); p->sense = true;   /* front HP, plugged */
    p = define(address, 0x1E, T_PIN | C_STEREO | C_DIGITAL); p->pincaps = 0x10; p->config = 0x01441130; CONNECT(p, 0x06);   /* S/PDIF out */
    p = pin(address, 0x18, 0x00003724, 0x01A19040); CONNECT(p, 0x0C, 0x0D, 0x0E, 0x0F);    /* rear mic */
}
static void idt(unsigned address) {
    sim_codec(address, 0x111D7675, 0x0A, 0x0B);                      /* nodes 0x0A..0x14 */
    dac(address, 0x10, false); dac(address, 0x11, false);
    struct sim_node* s = define(address, 0x13, T_SEL | C_STEREO | C_OUT_AMP | C_AMP_OVR); s->amp_out = 0x80037F7F; CONNECT(s, 0x10, 0x11);
    s = define(address, 0x14, T_SEL | C_STEREO | C_OUT_AMP | C_AMP_OVR); s->amp_out = 0x80037F7F; CONNECT(s, 0x11, 0x10);
    struct sim_node* p = pin(address, 0x0A, 0x0001001C, 0x0421101F); CONNECT(p, 0x13);   /* headphone jack, empty */
    p = pin(address, 0x0D, 0x00010010, 0x90170110); CONNECT(p, 0x14);                    /* built-in speaker */
}
static void hdmi(unsigned address) {
    sim_codec(address, 0x80862882, 2, 6);                            /* nodes 0x02..0x07 */
    dac(address, 0x02, true); dac(address, 0x03, true);
    for (unsigned nid = 0x05; nid <= 0x06; nid++) {
        struct sim_node* p = define(address, nid, T_PIN | C_STEREO | C_DIGITAL); p->pincaps = 0x01000094; p->config = 0x18560010 + (nid - 5) * 0x10;
        CONNECT(p, 0x02, 0x03); p->sense = nid == 0x06;              /* a monitor on the second port */
    }
}
/* QEMU hda-duplex (line-in) / hda-micro (mic): dac 2 -> out 3, adc 4 <- in 5 (no presence detect). */
static void qemu_duplex(unsigned address, bool mic) {
    sim_codec(address, mic ? 0x1AF40032 : 0x1AF40022, 2, 4);
    struct sim_node* d = dac(address, 2, false); d->caps = T_OUT | C_STEREO | C_FMT_OVR | C_AMP_OVR | C_OUT_AMP; d->amp_out = 0x80034A4A; d->pcm = 0x000201FC;
    struct sim_node* p = define(address, 3, T_PIN | C_STEREO); p->pincaps = 0x10; p->config = 0x00004010; CONNECT(p, 2);
    struct sim_node* a = define(address, 4, T_IN | C_STEREO | C_FMT_OVR | C_AMP_OVR | C_IN_AMP); a->amp_in = 0x80034A4A; a->pcm = 0x000201FC; CONNECT(a, 5);
    p = define(address, 5, T_PIN | C_STEREO); p->pincaps = 0x20; p->config = mic ? 0x00A05010 : 0x00805010;
}
/* Conexant-style laptop: ADC 0x14 selects (connection select + per-input amp) among a headset mic jack (VREF 50 %
   only), a dock line-in and the built-in mic; no pin boost amps. */
static void cx_laptop(unsigned address) {
    sim_codec(address, 0x14F15069, 0x10, 0x10);                      /* nodes 0x10..0x1F */
    dac(address, 0x10, false);
    struct sim_node* a = define(address, 0x14, T_IN | C_STEREO | C_IN_AMP | C_AMP_OVR | C_POWER); a->amp_in = 0x80051F0A; a->pcm = 0x000E0560;
    CONNECT(a, 0x1A, 0x1B, 0x1D, 0x1C);
    struct sim_node* p = pin(address, 0x19, 0x0001001C, 0x04211040); CONNECT(p, 0x10);          /* headphone jack */
    p = pin(address, 0x1F, 0x00010010, 0x90170110); CONNECT(p, 0x10);                           /* speaker */
    p = pin(address, 0x1A, 0x00000224, 0x04A11030);                                             /* headset mic jack */
    p = pin(address, 0x1B, 0x00000024, 0x01813050);                                             /* dock line-in */
    p = pin(address, 0x1D, 0x00000020, 0x90A70160);                                             /* built-in mic */
    p = pin(address, 0x1C, 0x00000014, 0x02A11040); p->sense = true;                            /* "front mic" without input capability */
}
/* The simulated ADC: `frames` frames through input stream descriptor 0 (BDL walk, LPIB wraps at CBL). Frame f carries
   (low 16 bits, high 16 bits) of f. */
static uint64_t sim_captured;
static void sim_capture(unsigned frames) {
    const unsigned sd = 0x80; assert(regs[sd] & 2);
    const uint32_t* bdl = (const uint32_t*)(uintptr_t)(get32(sd + 0x18) | (uint64_t)get32(sd + 0x1C) << 32);
    unsigned entries = get16(sd + 0x0C) + 1U, cbl = get32(sd + 0x08);
    for (unsigned i = 0; i < frames; i++, sim_captured++) {
        uint32_t lpib = get32(sd + 0x04), offset = lpib; unsigned e = 0;
        while (offset >= bdl[4 * e + 2]) { offset -= bdl[4 * e + 2]; e++; assert(e < entries); }
        int16_t* sample = (int16_t*)(uintptr_t)((bdl[4 * e] | (uint64_t)bdl[4 * e + 1] << 32) + offset);
        sample[0] = (int16_t)(uint16_t)sim_captured; sample[1] = (int16_t)(uint16_t)(sim_captured >> 16);
        put32(sd + 0x04, (lpib + 4) % cbl);
    }
}
static void check_route(unsigned address, unsigned pin_nid, uint32_t control, unsigned dac_nid) {
    struct sim_node *p = sim_node(address, pin_nid), *d = sim_node(address, dac_nid);
    assert(p->pinctl == control);
    assert(d->format == 0x0011 && d->streamed && d->stream == 0x10);
}
/* ---------- corpus of real codec dumps ---------- */
#include "hda_dumps.h"
static const struct dump_codec* dump(const char* name) {
    for (unsigned i = 0; i < sizeof(dumps) / sizeof(*dumps); i++) if (!strcmp(dumps[i].name, name)) return &dumps[i];
    fprintf(stderr, "no dump %s\n", name); exit(2);
}
static void load(unsigned controller, unsigned address, const char* name) {
    const struct dump_codec* d = dump(name); struct sim_controller* saved = cur; use(&controllers[controller]);
    assert(d->count <= 64);
    sim_codec(address, d->vendor, d->first, d->count);
    struct sim_codec* c = &sim[address];
    c->subsystem = d->subsystem; c->revision = d->revision; c->afg_pcm = d->afg_pcm; c->afg_amp_in = d->afg_in; c->afg_amp_out = d->afg_out;
    for (unsigned i = 0; i < d->nodes; i++) {
        const struct dump_node* s = &d->node[i]; struct sim_node* n = sim_node(address, s->nid); assert(n);
        n->caps = s->wcaps; n->pincaps = s->pincaps; n->config = s->config; n->amp_in = s->amp_in; n->amp_out = s->amp_out;
        n->pcm = s->pcm; n->devices = s->devices; n->conn_len = s->conns; memcpy(n->conn, s->conn, sizeof(n->conn));
    }
    if ((d->vendor >> 16) == 0x10EC) { c->coef[0x04] = 0x8000 | 0x0222; c->coef[0x07] = 0x0020 | 0x3000; }   /* bits Linux clears */
    use(saved);
}
/* A monitor's ELD (CEA-861-D, version 2): name, connection type, LPCM SAD (channels, 48 kHz?, 16-bit). */
static void monitor(unsigned controller, unsigned address, unsigned nid, const char* name, bool dp, unsigned channels, bool rate48) {
    struct sim_controller* saved = cur; use(&controllers[controller]);
    struct sim_node* n = sim_node(address, nid); assert(n);
    memset(n->eld, 0, sizeof(n->eld)); unsigned mnl = (unsigned)strlen(name);
    n->eld[0] = 2 << 3; n->eld[4] = (uint8_t)(3 << 5 | mnl); n->eld[5] = (uint8_t)(1 << 4 | (dp ? 1 : 0) << 2); n->eld[7] = 0x01;
    n->eld[16] = 0x10; n->eld[17] = 0xAC; memcpy(n->eld + 20, name, mnl);
    uint8_t* sad = n->eld + 20 + mnl; sad[0] = (uint8_t)(1 << 3 | (channels - 1)); sad[1] = rate48 ? 0x07 : 0x03; sad[2] = 0x07;
    n->eld_size = (20 + mnl + 3 + 3) & ~3U; n->eld[2] = (uint8_t)((n->eld_size - 4) / 4);
    n->eldv = true; n->eld_bad_byte = ~0U;
    if (amd(&sim[address])) { n->ati_spkalloc = 0x01 | (dp ? 0x200 : 0x100); n->ati_sad = sad[0] | (uint32_t)sad[1] << 8 | (uint32_t)sad[2] << 16; strcpy(n->ati_name, name); }
    jack(address, nid, true);
    use(saved);
}
static void reset_controllers(unsigned count) { memset(controllers, 0, sizeof(controllers)); controller_count = count; use(&controllers[0]); unsol_sent = 0; output_option = NULL; }
static void corpus(void) {
    /* Desktop ALC897 (Lenovo dump) alone, rear line-out plugged: analog route, Realtek coefficients, jack events. */
    reset_controllers(1); controllers[0].vendor = 0x1022; controllers[0].device = 0x15E3;
    load(0, 0, "alc897-lenovo-90rs001fus"); sim_node(0, 0x14)->sense = true;
    assert(probe() && logged("HDA 1022:15E3 codec 0 vendor 10ec0897 out pin 0x14 <- mix 0x0c <- dac 0x02 (line-out, jack present)"));
    assert(!(sim[0].coef[0x04] & 0x8000) && !(sim[0].coef[0x07] & 0x0020) && (sim[0].coef[0x04] & 0x0222) == 0x0222 && sim[0].coef_writes == 2);
    assert(logged("Realtek ALC897 coefficients set (PLL, EAPD)") && logged("jack 0x19 (front mic) empty\n"));
    /* capture: ADC 0x08 <- mixer 0x23 <- the front mic (empty jacks: front mic > rear mic > line-in), as Linux left it */
    assert(logged("codec 0 capture pin 0x19 -> mix 0x23 -> adc 0x08 (front mic, jack empty)") && !strcmp(engine.capture_name, "Realtek Capture"));
    assert(engine.controls == 3 && !strcmp(engine.control[0].name, "Capture Volume") && engine.control[0].max == 0x3F && engine.control[0].value[0] == 0x17);
    assert(engine.control[0].db_min == -1725 && engine.control[0].db_step == 75 && !strcmp(engine.control[1].name, "Capture Switch"));
    assert(!strcmp(engine.control[2].name, "Mic Boost Volume") && engine.control[2].max == 3 && engine.control[2].value[0] == 1 && engine.control[2].db_step == 1000);
    { struct sim_node *mix = sim_node(0, 0x23), *adc = sim_node(0, 0x08), *mic = sim_node(0, 0x19);
      assert(mix->in_amp[1] == 0x7100 && mix->in_amp[0] == 0x7080 && mix->in_amp[2] == 0x7280 && mix->in_amp[10] == 0x7A80);
      assert(adc->in_amp[0] == 0x5017 && adc->stream == 0x20 && adc->format == 0x0011 && mic->pinctl == 0x24 && mic->in_amp[0] == 0x7001);
      assert(!sim_node(0, 0x09)->streamed && !sim_node(0, 0x18)->pinctl); }
    assert(logged("jack 0x18 (rear mic) empty") && logged("jack 0x1a (rear line-in) empty") && hda.unsol && logged("jack events)"));
    assert(sim_node(0, 0x14)->pinctl == 0x40 && sim_node(0, 0x1b)->unsol && sim_node(0, 0x19)->unsol && sim_node(0, 0x14)->unsol);
    jack(0, 0x1b, true); jack(0, 0x19, true); engine.poll(clock_ns + 1000000ULL);
    assert(logged("jack 0x1b (front headphone) plugged") && logged("jack 0x19 (front mic) plugged") && !hda.automute);
    jack(0, 0x19, false); engine.poll(clock_ns + 1000000ULL); assert(logged("jack 0x19 (front mic) unplugged"));
    /* a mic plugged into the rear jack: recording starts from it; then a front headset mic takes over mid-recording */
    jack(0, 0x18, true); assert(engine.capture_start());
    assert(logged("codec 0 capture switched to pin 0x18 -> mix 0x23 -> adc 0x08 (rear mic, jack present)"));
    assert(sim_node(0, 0x23)->in_amp[0] == 0x7000 && sim_node(0, 0x23)->in_amp[1] == 0x7180 && sim_node(0, 0x18)->pinctl == 0x24 && sim_node(0, 0x18)->in_amp[0] == 0x7001);
    { uint64_t position; sim_capture(4800); assert(engine.capture_read(&position) && position == 4800 - 64);
      jack(0, 0x19, true); sim_capture(4800); clock_ns += 2000000000ULL; assert(engine.capture_read(&position) && position == 9600 - 64);
      assert(logged("capture switched to pin 0x19 -> mix 0x23 -> adc 0x08 (front mic, jack present)") && sim_node(0, 0x23)->in_amp[1] == 0x7100 && sim_node(0, 0x23)->in_amp[0] == 0x7080); }
    engine.control[2].value[0] = 3; assert(engine.control_put(2) && sim_node(0, 0x19)->in_amp[0] == 0x7003);   /* boost +30 dB */
    engine.control[1].value[0] = 0; assert(engine.control_put(1) && sim_node(0, 0x08)->in_amp[0] == 0x5097);    /* capture switch off */
    engine.capture_stop(); assert(!(regs[0x80] & 2));
    /* a re-probe into a scratch engine (as a resume after power loss does): the capture controls stay in the live
       engine and their values (switch off, boost +30 dB) are applied again; capture still starts */
    { struct sound_engine scratch;
      assert(controller_probe(&devices[0], MODE_ANALOG_CONNECTED, (struct codec*)(uintptr_t)arena_alloc(sizeof(struct codec)), &scratch) == 1);
      assert(hcap.engine == &engine && engine.controls == 3 && hcap.ok && scratch.capture_start == engine.capture_start);
      assert(sim_node(0, 0x08)->in_amp[0] == 0x5097 && sim_node(0, 0x19)->in_amp[0] == 0x7003 && sim_node(0, 0x19)->pinctl == 0x24);
      assert(engine.capture_start() && (regs[0x80] & 2)); engine.capture_stop(); }
    puts("corpus: ALC897 desktop passed");

    /* Laptop ALC897 (ASUS dump): speaker primary, headphone auto-mute driven by unsolicited responses. */
    reset_controllers(1); load(0, 0, "alc897-asus-x515ea");
    assert(probe() && logged("out pin 0x14 <- mix 0x0c <- dac 0x02 (speaker)") && logged("also pin 0x1b") && hda.automute);
    jack(0, 0x1b, true); engine.poll(clock_ns + 1000000ULL);
    assert(sim_node(0, 0x14)->pinctl == 0 && logged("headphone plugged: speakers muted"));
    assert(logged("codec 0 capture pin 0x1a -> mix 0x23 -> adc 0x08 (built-in mic)") && sim_node(0, 0x23)->in_amp[2] == 0x7200 && sim_node(0, 0x23)->in_amp[1] == 0x7180);
    assert(sim_node(0, 0x1a)->pinctl == 0x24 && engine.controls == 3);
    puts("corpus: ALC897 laptop passed");

    /* The user's PC: AMD analog HDA 1022:15E3 (MSI's ALC892), AMD HDMI 1002:1640 (aa01 rev 0x100800), NVIDIA 10de:2f80.
       a) nothing plugged into the analog jacks, a DisplayPort monitor on the NVIDIA card: HDMI/DP output chosen. */
    reset_controllers(3); controllers[0].vendor = 0x1022; controllers[0].device = 0x15E3;
    load(0, 0, "alc892-msi-ms7b79"); load(1, 0, "atihdmi-aa01-rev800-1002-1640"); load(2, 0, "nvhdmi-10de00ab-blackwell");
    monitor(2, 0, 0x05, "DELL U2723QE", true, 2, true);
    use(&controllers[2]); sim_node(0, 0x05)->devsel = 1;                                             /* DP-MST device 1 left selected */
    assert(probe() && cur == &controllers[2] && logged("analog outputs, no jack connected: HDMI/DP monitors first"));
    assert(logged("HDA 10DE:2F80 codec 0 vendor 10de00ab out pin 0x05 <- dac 0x08 (digital, DisplayPort monitor \"DELL U2723QE\", LPCM)"));
    struct sim_node* p5 = sim_node(0, 0x05); struct sim_node* c8 = sim_node(0, 0x08);
    assert(p5->pinctl == 0x40 && p5->devsel == 0 && p5->select == 0 && c8->stream == 0x10 && c8->digi1 == 1 && c8->channels == 1 && c8->format == 0x0011);
    assert(p5->dips == 9 && p5->dip[0] == 0x84 && p5->dip[1] == 0x1B && p5->dip[2] == 0x44 && p5->dip[4] == 0x01 && p5->dip_xmit == 0xC0);   /* NVIDIA DP: HDMI layout */
    { unsigned sum = 0; for (unsigned i = 0; i < 9; i++) sum += p5->dip[i]; assert((sum & 0xFF) == 0); }
    assert(p5->slot[0] == 0 && p5->slot[1] == 1 && p5->slot[2] == 0xF && p5->slot[7] == 0xF && strstr(engine.pcm_name, "Digital"));
    /* capture: the NVIDIA codec has no input path, so the ALC892 controller records (its front mic, then a rear mic) */
    assert(logged("HDA 1022:15E3 codec 0 capture pin 0x19 -> mix 0x23 -> adc 0x08 (front mic, jack empty)") && logged("HDA 1022:15E3 capture controller (playback on another controller)"));
    assert((controllers[0].s_command & 0x406) == 0x406 && !(controllers[1].s_command & 4));       /* AMD HDMI controller released */
    assert(engine.capture_start() && hda_ctl == &hda_states[0] && (controllers[0].s_regs[0x80] & 2) && !(controllers[2].s_regs[0x80] & 2));
    { uint64_t position; use(&controllers[0]); sim_captured = 0; sim_capture(4800); jack(0, 0x18, true); clock_ns += 2000000000ULL;
      assert(engine.capture_read(&position) && position == 4800 - 64 && logged("HDA 1022:15E3 codec 0 capture switched to pin 0x18 -> mix 0x23 -> adc 0x08 (rear mic, jack present)"));
      assert(cur == &controllers[0] && sim_node(0, 0x23)->in_amp[0] == 0x7000 && hda_ctl == &hda_states[0]);
      engine.capture_stop(); assert(!(controllers[0].s_regs[0x80] & 2)); use(&controllers[2]); }
    jack(0, 0x06, true); engine.poll(clock_ns + 1000000ULL); assert(logged("jack 0x06 (HDMI/DP) plugged"));   /* hot-plug reported */
    puts("corpus: NVIDIA DisplayPort monitor passed");

    /* b) rear line-out plugged: the analog route wins over the monitor. */
    reset_controllers(3); controllers[0].vendor = 0x1022; controllers[0].device = 0x15E3;
    load(0, 0, "alc892-msi-ms7b79"); load(1, 0, "atihdmi-aa01-rev800-1002-1640"); load(2, 0, "nvhdmi-10de00ab-blackwell");
    monitor(2, 0, 0x05, "DELL U2723QE", true, 2, true); use(&controllers[0]); sim_node(0, 0x14)->sense = true;
    assert(probe() && cur == &controllers[0] && logged("HDA 1022:15E3 codec 0 vendor 10ec0892 out pin 0x14 <- mix 0x0c <- dac 0x02 (line-out, jack present)"));
    assert(!(sim[0].coef[0x07] & 0x20));
    assert(logged("codec 0 capture pin 0x19 -> mix 0x23 -> adc 0x08 (front mic, jack empty)") && engine.control[0].db_min == -1600 && engine.control[0].db_step == 100);
    /* c) hda.output=hdmi: the monitor anyway. */
    output_option = "hda.output=hdmi";
    assert(probe() && cur == &controllers[2] && logged("DisplayPort monitor \"DELL U2723QE\""));
    output_option = "hda.output=analog"; sim_node(0, 0x05)->sense = true;
    use(&controllers[0]); sim_node(0, 0x14)->sense = false;
    assert(probe() && cur == &controllers[0] && logged("out pin 0x14"));                                     /* analog only, even unplugged */
    puts("corpus: output selection passed");

    /* d) A monitor on the AMD iGPU (Rembrandt 1002:1640, aa01 rev 0x100800): ELD emulated from the AMD vendor verbs,
          channel-wise slot mapping, channel allocation, ramp rate, no InfoFrame. */
    reset_controllers(3); controllers[0].vendor = 0x1022; controllers[0].device = 0x15E3;
    load(0, 0, "alc892-msi-ms7b79"); load(1, 0, "atihdmi-aa01-rev800-1002-1640"); load(2, 0, "nvhdmi-10de00ab-blackwell");
    monitor(1, 0, 0x05, "LG ULTRAGEAR", false, 8, true);
    assert(probe() && cur == &controllers[1] && logged("HDA 1002:1640 codec 0 vendor 1002aa01 out pin 0x05 <- dac 0x04 (digital, HDMI monitor \"LG ULTRAGEAR\", LPCM)"));
    struct sim_node *a5 = sim_node(0, 0x05), *a4 = sim_node(0, 0x04);
    assert(a5->ati_mode == 1 && a5->ati_mc[0] == 0x01 && a5->ati_mc[0x0E] == 0x11 && a5->ati_mc[1] == 0 && a5->ati_mc[0x0F] == 0 && a5->ati_written && !a5->ati_ca && !a5->ati_downmix);
    assert(a4->ati_ramp == 180 && a4->stream == 0x10 && a4->digi1 == 1 && a5->dips == 0 && a5->pinctl == 0x40);
    puts("corpus: AMD HDMI monitor passed");

    /* e) Two monitors on the NVIDIA card: both pins driven from the stream; a monitor without 48 kHz LPCM is skipped. */
    reset_controllers(3); controllers[0].vendor = 0x1022; controllers[0].device = 0x15E3;
    load(0, 0, "alc892-msi-ms7b79"); load(1, 0, "atihdmi-aa01-rev700-1002-1637"); load(2, 0, "nvhdmi-10de00ab-blackwell");
    monitor(2, 0, 0x05, "OLD 44K", false, 2, false); monitor(2, 0, 0x06, "SAMSUNG TV", false, 2, true); monitor(2, 0, 0x07, "DELL U2723QE", true, 2, true);
    assert(probe() && cur == &controllers[2] && logged("out pin 0x06 <- dac 0x08 (digital, HDMI monitor \"SAMSUNG TV\", LPCM)"));
    assert(logged("also pin 0x07 <- dac 0x08 (digital, DisplayPort monitor \"DELL U2723QE\", LPCM)") && !logged("pin 0x05 <-"));
    assert(sim_node(0, 0x06)->pinctl == 0x40 && sim_node(0, 0x06)->dip[1] == 0x01 && sim_node(0, 0x06)->dip[2] == 0x0A && !sim_node(0, 0x05)->pinctl);
    /* only the 44.1 kHz monitor connected: not a usable sink; the (unplugged) analog output is used instead */
    reset_controllers(3); controllers[0].vendor = 0x1022; controllers[0].device = 0x15E3;
    load(0, 0, "alc892-msi-ms7b79"); load(1, 0, "atihdmi-aa01-rev700-1002-1637"); load(2, 0, "nvhdmi-10de00ab-blackwell");
    monitor(2, 0, 0x07, "OLD 44K", false, 2, false);
    assert(probe() && cur == &controllers[0] && logged("out pin 0x14") && !logged("OLD 44K"));
    puts("corpus: multiple monitors passed");

    /* f) Hostile ELDs: oversized buffer, bad name length, SADs past the end, a byte without the valid flag: never used. */
    static const struct { unsigned what; } cases[] = {{0}, {1}, {2}, {3}, {4}};
    for (unsigned k = 0; k < sizeof(cases) / sizeof(*cases); k++) {
        reset_controllers(3); controllers[0].vendor = 0x1022; controllers[0].device = 0x15E3;
        load(0, 0, "alc892-msi-ms7b79"); load(1, 0, "atihdmi-aa01-rev800-1002-1640"); load(2, 0, "nvhdmi-10de00ab-blackwell");
        monitor(2, 0, 0x05, "EVIL", true, 2, true);
        use(&controllers[2]); struct sim_node* e = sim_node(0, 0x05);
        if (k == 0) e->eld_size = 300;                          /* larger than any ELD buffer */
        if (k == 1) { e->eld[4] = (uint8_t)(3 << 5 | 31); e->eld_size = 64; memset(e->eld + 24, 'A', 40); }   /* name length 31 (max 16) */
        if (k == 2) e->eld[5] = (uint8_t)(15 << 4 | 1 << 2);    /* 15 SADs in a 28-byte ELD */
        if (k == 3) e->eld_bad_byte = 6;                        /* ELDD answer without the valid flag */
        if (k == 4) e->eldv = false;                            /* monitor present, ELD not valid (stale buffer) */
        assert(probe() && cur == &controllers[0] && !logged("EVIL"));
    }
    puts("corpus: hostile ELDs passed");
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);                                     /* driver log visible up to a failed assert */
    ring = arena_alloc(SOUND_RING_FRAMES * SOUND_FRAME_BYTES); sound_capture_ring = arena_alloc(SOUND_RING_FRAMES * SOUND_FRAME_BYTES);

    /* QEMU hda-output through CORB/RIRB (with QEMU's RINTCNT pause), then the PCM stream engine. */
    memset(sim, 0, sizeof(sim)); qemu_output(0);
    assert(probe() && !hda.immediate && corb_processed == verbs_answered);
    check_route(0, 3, 0x40, 2);
    assert(sim_node(0, 2)->out_amp == 0xB04A);                         /* 0 dB = offset 0x4A, unmuted, both channels */
    assert(logged("HDA 8086:2668 codec 0 vendor 1af40012 out pin 0x03 <- dac 0x02 (line-out)\n") && logged("CORB/RIRB)"));
    assert(!strcmp(engine.id, "NexusHDA") && engine.cyclic && !strcmp(engine.longname, "NexusOS HD Audio 8086:2668 codec 1af40012 at 0xfeb00000"));
    assert(!strcmp(engine.components, "HDA:1af40012,10251234,00100101") && !strcmp(engine.pcm_name, "QEMU 0012 Analog"));
    uint64_t queued = 1000;
    assert(engine.start(&queued, 5000) && queued == 5000);
    unsigned sd = 0x80 + 4 * 0x20, entries = get16(sd + 0x0C) + 1U, covered = 0;
    assert(get32(sd + 0x08) == SOUND_RING_FRAMES * 4 && get16(sd + 0x12) == 0x0011 && regs[sd + 0x02] == 0x10 && (regs[sd] & 2) && entries >= 2 && entries <= 64);
    const uint32_t* bdl = (const uint32_t*)(uintptr_t)(get32(sd + 0x18) | (uint64_t)get32(sd + 0x1C) << 32);
    for (unsigned i = 0; i < entries; i++) {
        uint64_t address = bdl[4 * i] | (uint64_t)bdl[4 * i + 1] << 32;
        assert(address % 128 == 0 && bdl[4 * i + 2] % 128 == 0 && bdl[4 * i + 3] == 0);
        assert(address == ring + ((992 + covered) % SOUND_RING_FRAMES) * 4ULL);   /* rotated to 1000 & ~31 */
        covered += bdl[4 * i + 2] / 4;
    }
    assert(covered == SOUND_RING_FRAMES);
    uint64_t position = 0;
    put32(sd + 0x04, 4000); assert(engine.read(&position) && position == 992 + 1000);
    put32(sd + 0x04, 100); assert(engine.read(&position) && position == 992 + (SOUND_RING_FRAMES * 4ULL + 100) / 4);
    regs[sd] &= (uint8_t)~2; assert(!engine.read(&position) && logged("stream stopped"));
    engine.stop(); assert(!(regs[sd] & 2));
    assert(!engine.capture_start && !engine.controls);                 /* hda-output: no input converter */

    /* QEMU hda-duplex: line-in through ADC 4 on input stream descriptor 0 (tag 2), cyclic BDL over the capture ring. */
    memset(sim, 0, sizeof(sim)); qemu_duplex(0, false);
    assert(probe() && logged("codec 0 capture pin 0x05 -> adc 0x04 (line-in)") && !strcmp(engine.capture_name, "QEMU Capture"));
    assert(engine.controls == 2 && engine.control[0].max == 0x4A && engine.control[0].value[0] == 0x4A && engine.control[0].value[1] == 0x4A);
    assert(engine.control[0].db_min == -7400 && engine.control[0].db_step == 100 && engine.control[1].type == SOUND_CONTROL_BOOLEAN);
    assert(sim_node(0, 5)->pinctl == 0x20 && sim_node(0, 4)->stream == 0x20 && sim_node(0, 4)->format == 0x0011 && sim_node(0, 4)->in_amp[0] == 0x504A);
    check_route(0, 3, 0x40, 2);
    assert(engine.capture_start());
    { const unsigned in = 0x80;
      assert((regs[in] & 2) && regs[in + 2] == 0x20 && get16(in + 0x12) == 0x0011 && get32(in + 0x08) == SOUND_RING_FRAMES * 4 && get16(in + 0x0C) == 7);
      const uint32_t* cbdl = (const uint32_t*)(uintptr_t)(get32(in + 0x18) | (uint64_t)get32(in + 0x1C) << 32);
      assert((uint64_t)(uintptr_t)cbdl == hda.dma + 3584);
      for (unsigned i = 0; i < 8; i++) assert((cbdl[4 * i] | (uint64_t)cbdl[4 * i + 1] << 32) == sound_capture_ring + i * 65536ULL && cbdl[4 * i + 2] == 65536 && !cbdl[4 * i + 3]);
      uint64_t position = 0; sim_captured = 0;
      assert(engine.capture_read(&position) && position == 0);
      sim_capture(1000); assert(engine.capture_read(&position) && position == 1000 - 64);
      sim_capture(SOUND_RING_FRAMES - 500); assert(engine.capture_read(&position) && position == SOUND_RING_FRAMES + 500 - 64);
      sim_capture(500); assert(engine.capture_read(&position) && position == 1000 + SOUND_RING_FRAMES - 64);   /* past a lap */
      const int16_t* captured = (const int16_t*)(uintptr_t)sound_capture_ring;
      for (unsigned i = 0; i < SOUND_RING_FRAMES; i++) { uint64_t f = i < 1000 ? i + SOUND_RING_FRAMES : i; assert(captured[2 * i] == (int16_t)(uint16_t)f && captured[2 * i + 1] == (int16_t)(uint16_t)(f >> 16)); }
      engine.control[0].value[0] = 10; engine.control[0].value[1] = 20; assert(engine.control_put(0) && sim_node(0, 4)->in_amp[0] == 0x5014);
      engine.control[1].value[0] = 0; assert(engine.control_put(1) && sim_node(0, 4)->in_amp[0] == 0x5094);
      regs[in] &= (uint8_t)~2; assert(!engine.capture_read(&position) && logged("capture stream stopped"));
      engine.capture_stop(); assert(engine.capture_start() && (regs[in] & 2) && engine.capture_read(&position) && position == 0);
      engine.capture_stop(); assert(!(regs[in] & 2)); }
    memset(sim, 0, sizeof(sim)); qemu_duplex(0, true);                 /* hda-micro: a mic pin without presence detect */
    assert(probe() && logged("codec 0 capture pin 0x05 -> adc 0x04 (built-in mic)") && sim_node(0, 5)->pinctl == 0x20 && engine.controls == 2);

    /* Conexant-style laptop: built-in mic at boot (ADC select 2); a headset plugged mid-recording takes over (select 0,
       VREF 50 %); unplugged: back to the built-in mic. */
    memset(sim, 0, sizeof(sim)); cx_laptop(0);
    assert(probe() && logged("codec 0 capture pin 0x1d -> adc 0x14 (built-in mic)") && sim_node(0, 0x14)->select == 2 && sim_node(0, 0x1D)->pinctl == 0x20);
    assert(engine.controls == 2 && engine.control[0].db_min == -1500 && engine.control[0].db_step == 150 && sim_node(0, 0x14)->in_amp[2] == 0x520A);
    assert(sim_node(0, 0x14)->power == 0 && engine.capture_start());
    { uint64_t position; jack(0, 0x1A, true); engine.poll(clock_ns + 1000000ULL); assert(logged("jack 0x1a (mic) plugged"));
      sim_capture(480); clock_ns += 2000000000ULL; assert(engine.capture_read(&position) && position == 480 - 64);
      assert(logged("capture switched to pin 0x1a -> adc 0x14 (mic, jack present)") && sim_node(0, 0x14)->select == 0 && sim_node(0, 0x1A)->pinctl == 0x21);
      assert(sim_node(0, 0x14)->in_amp[0] == 0x500A);
      jack(0, 0x1A, false); sim_capture(480); clock_ns += 2000000000ULL; assert(engine.capture_read(&position) && position == 960 - 64);
      assert(sim_node(0, 0x14)->select == 2); engine.capture_stop(); }

    /* Immediate Command interface forced. */
    memset(sim, 0, sizeof(sim)); qemu_output(0); force_immediate = true;
    assert(probe() && hda.immediate && corb_processed == 0 && logged("immediate commands)"));
    check_route(0, 3, 0x40, 2); force_immediate = false;

    /* Laptop, headphone unplugged: speaker primary, headphone joins the speaker's mixer; loopback input muted. */
    memset(sim, 0, sizeof(sim)); alc269(0, false);
    assert(probe());
    assert(logged("codec 0 vendor 10ec0269 out pin 0x14 <- mix 0x0c <- dac 0x02 (speaker)\n"));
    assert(logged("codec 0 vendor 10ec0269 also pin 0x21 <- mix 0x0c <- dac 0x02 (headphone, jack empty)\n"));
    check_route(0, 0x14, 0x40, 0x02); check_route(0, 0x21, 0xC0, 0x02);
    assert(sim_node(0, 0x14)->selected && sim_node(0, 0x14)->select == 0 && sim_node(0, 0x21)->select == 0);
    assert(sim_node(0, 0x0C)->in_amp[0] == 0x7000 && sim_node(0, 0x0C)->in_amp[1] == 0x7180);   /* DAC in, aamix muted */
    assert(sim_node(0, 0x02)->out_amp == 0xB057 && sim_node(0, 0x02)->power == 0 && sim_node(0, 0x14)->out_amp == 0xB000);
    assert(sim_node(0, 0x14)->eapd == 2 && sim_node(0, 0x21)->eapd == 2);
    assert(!sim_node(0, 0x03)->streamed && !sim_node(0, 0x18)->pinctl && !sim_node(0, 0x1E)->pinctl);
    assert(engine.poll && hda.automute && !hda.hp_present);
    assert(hda.unsol && (sim_node(0, 0x21)->unsol & 0x80) && (get32(0x08) & 0x100));             /* jack events on */
    jack(0, 0x21, true); engine.poll(clock_ns + 1000000ULL);                                     /* plug: unsolicited response */
    assert(unsol_sent); assert(sim_node(0, 0x14)->pinctl == 0); assert(logged("headphone plugged: speakers muted")); assert(logged("jack 0x21 (front headphone) plugged"));
    unsigned senses = sim_node(0, 0x21)->senses; engine.poll(clock_ns);                          /* nothing pending: no verbs */
    assert(sim_node(0, 0x21)->senses == senses);
    jack(0, 0x21, false); engine.poll(clock_ns + 1000000ULL);                                    /* unplug */
    assert(sim_node(0, 0x14)->pinctl == 0x40 && logged("headphone unplugged: speakers on"));
    sim_node(0, 0x21)->sense = true; sim_node(0, 0x21)->unsol = 0;                               /* event lost: 2 s backstop poll */
    engine.poll(clock_ns + 1000000ULL); assert(sim_node(0, 0x14)->pinctl == 0x40);
    engine.poll(clock_ns + 2500000000ULL); assert(sim_node(0, 0x14)->pinctl == 0);

    /* Laptop, headphone plugged at boot: headphone primary, speaker muted from the start. */
    memset(sim, 0, sizeof(sim)); alc269(0, true);
    assert(probe() && logged("out pin 0x21 <- mix 0x0c <- dac 0x02 (headphone, jack present)") && logged("also pin 0x14"));
    assert(sim_node(0, 0x21)->pinctl == 0xC0 && sim_node(0, 0x14)->pinctl == 0 && hda.hp_present);

    /* Desktop: plugged front headphone primary through its selector, front line-out joins; surround, S/PDIF skipped. */
    memset(sim, 0, sizeof(sim)); alc892(0);
    assert(probe());
    assert(logged("out pin 0x1b <- mix 0x0c <- dac 0x02 (headphone, jack present)") && logged("also pin 0x14 <- mix 0x0c <- dac 0x02 (line-out, jack empty)"));
    assert(sim_node(0, 0x1B)->selected && sim_node(0, 0x1B)->select == 0 && sim_node(0, 0x1B)->pinctl == 0xC0 && sim_node(0, 0x14)->pinctl == 0x40);
    assert(!sim_node(0, 0x15)->pinctl && !sim_node(0, 0x16)->pinctl && !sim_node(0, 0x17)->pinctl && !sim_node(0, 0x1E)->pinctl);
    assert(!sim_node(0, 0x03)->streamed && !sim_node(0, 0x06)->streamed && !hda.automute);
    static struct codec parsed;                                          /* connection lists: long form, range, 2 reads */
    assert(codec_parse(&parsed, 0) && parsed.count == 0x25);
    const struct widget *aamix = &parsed.w[widget_index(&parsed, 0x0B)], *front = &parsed.w[widget_index(&parsed, 0x1B)];
    assert(aamix->conns == 5 && aamix->conn[0] == 0x18 && aamix->conn[1] == 0x19 && aamix->conn[3] == 0x1B && aamix->conn[4] == 0x1D);
    assert(front->conns == 5 && front->conn[3] == 0x0F && front->conn[4] == 0x26 && front->type == W_PIN && aamix->type == W_MIX);

    /* Selectors and one converter per pin: speaker on its selector's first DAC, headphone gets the other one. */
    memset(sim, 0, sizeof(sim)); idt(0);
    assert(probe());
    assert(logged("out pin 0x0d <- sel 0x14 <- dac 0x11 (speaker)") && logged("also pin 0x0a <- sel 0x13 <- dac 0x10 (headphone, jack empty)"));
    check_route(0, 0x0D, 0x40, 0x11); check_route(0, 0x0A, 0xC0, 0x10);
    assert(sim_node(0, 0x13)->selected && sim_node(0, 0x13)->select == 0 && sim_node(0, 0x14)->select == 0 && sim_node(0, 0x14)->out_amp == 0xB07F);

    /* HDMI-only codec: used only as a fallback, on the port with a monitor, with the digital converter set up. */
    memset(sim, 0, sizeof(sim)); hdmi(0);
    assert(probe() && logged("has digital outputs only, kept as a fallback") && logged("out pin 0x06 <- dac 0x02 (digital, jack present)"));
    struct sim_node *port = sim_node(0, 0x06), *converter = sim_node(0, 0x02);
    assert(port->pinctl == 0x40 && converter->digi1 == 1 && converter->channels == 1 && converter->stream == 0x10);
    assert(port->dips == 9 && port->dip[0] == 0x84 && port->dip_xmit == 0xC0);
    unsigned sum = 0; for (unsigned i = 0; i < port->dips; i++) sum += port->dip[i];
    assert((sum & 0xFF) == 0 && strstr(engine.pcm_name, "Digital") && !sim_node(0, 0x05)->pinctl);

    /* HDMI codec 0 + analog codec 2 on one link: the analog codec wins, no digital fallback pass. */
    memset(sim, 0, sizeof(sim)); hdmi(0); qemu_output(2);
    assert(probe() && logged("codec 2 vendor 1af40012 out pin 0x03 <- dac 0x02 (line-out)") && !sim_node(0, 0x06)->pinctl && hda.codec == 2);

    /* A codec that stops answering mid-parse: CORB times out, the immediate interface takes over, the codec is given
       up after three timeouts and the next codec is used. */
    memset(sim, 0, sizeof(sim)); qemu_output(0); qemu_output(1); sim[0].limited = true; sim[0].budget = 6;
    uint64_t started = clock_ns;
    assert(probe() && hda.immediate && logged("using immediate commands") && logged("codec 0 stopped answering, giving it up"));
    assert(logged("codec 0 did not answer while being parsed") && logged("codec 1 vendor 1af40012 out pin 0x03 <- dac 0x02 (line-out)") && hda.codec == 1);
    assert(clock_ns - started < 500000000ULL);                           /* bounded: a few 20 ms timeouts, not a hang */
    /* A codec that never answers at all. */
    memset(sim, 0, sizeof(sim)); qemu_output(0); qemu_output(1); sim[0].dead = true;
    assert(probe() && logged("codec 0 did not answer\n") && hda.codec == 1);

    /* No codec on the link: the driver gives up and releases the controller. */
    memset(sim, 0, sizeof(sim));
    assert(!probe() && logged("no codec attached (STATESTS=0), giving up") && !(pci_command & 4) && !(get32(0x08) & 1));
    corpus();
    puts("HDA CODEC TESTS PASSED");
    return 0;
}

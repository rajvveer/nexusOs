/* ============================================================================
 * NexusOS — USB OHCI Host Controller Driver (USB 1.1) — Phase 55
 * ============================================================================
 * Polled OHCI driver (PCI class 0C/03/10, MMIO BAR0). HCCA with a 32-entry
 * interrupt table, one control ED on which control and bulk TD chains run
 * synchronously, and a chain of interrupt EDs (one per interrupt-IN endpoint)
 * referenced by every interrupt-table entry, each with one TD kept armed
 * between polls. Data toggles for bulk/interrupt live in the ED's toggleCarry
 * bit and are written back to the endpoint after each transfer.
 * ============================================================================ */

#include "usb.h"
#include "pci.h"
#include "paging.h"
#include "string.h"

#define OHCI_MAX         2
#define OHCI_INTR_SLOTS  8
#define OHCI_TDS         8

/* Registers (dword index) */
#define R_REVISION       0
#define R_CONTROL        1
#define R_CMDSTATUS      2
#define R_INTSTATUS      3
#define R_INTENABLE      4
#define R_INTDISABLE     5
#define R_HCCA           6
#define R_PERIODCURED    7
#define R_CTRLHEADED     8
#define R_CTRLCURED      9
#define R_BULKHEADED     10
#define R_BULKCURED      11
#define R_DONEHEAD       12
#define R_FMINTERVAL     13
#define R_FMREMAINING    14
#define R_FMNUMBER       15
#define R_PERIODICSTART  16
#define R_LSTHRESH       17
#define R_RHDESCA        18
#define R_RHDESCB        19
#define R_RHSTATUS       20
#define R_RHPORT(p)      (21 + ((p) - 1))

#define CTL_CBSR3        3u
#define CTL_PLE          (1u << 2)
#define CTL_IE           (1u << 3)
#define CTL_CLE          (1u << 4)
#define CTL_BLE          (1u << 5)
#define CTL_HCFS_MASK    (3u << 6)
#define CTL_HCFS_OPER    (2u << 6)
#define CTL_IR           (1u << 8)

#define CS_HCR           (1u << 0)
#define CS_CLF           (1u << 1)
#define CS_BLF           (1u << 2)
#define CS_OCR           (1u << 3)

#define INT_WDH          (1u << 1)

#define RHS_LPSC         (1u << 16)
#define RHP_CCS          (1u << 0)
#define RHP_PES          (1u << 1)
#define RHP_PRS          (1u << 4)
#define RHP_PPS          (1u << 8)
#define RHP_LSDA         (1u << 9)
#define RHP_CSC          (1u << 16)
#define RHP_PESC         (1u << 17)
#define RHP_PSSC         (1u << 18)
#define RHP_OCIC         (1u << 19)
#define RHP_PRSC         (1u << 20)
#define RHP_CHANGE       (RHP_CSC | RHP_PESC | RHP_PSSC | RHP_OCIC | RHP_PRSC)

/* ED dword 0 */
#define ED_FA(a)         ((uint32_t)(a) & 0x7F)
#define ED_EN(e)         (((uint32_t)(e) & 0xF) << 7)
#define ED_D_TD          (0u << 11)
#define ED_S             (1u << 13)
#define ED_K             (1u << 14)
#define ED_MPS(m)        (((uint32_t)(m) & 0x7FF) << 16)
/* ED HeadP bits */
#define ED_HALTED        1u
#define ED_TOGGLE        2u
#define ED_PTR_MASK      0xFFFFFFF0u

/* TD dword 0 */
#define TD_R             (1u << 18)
#define TD_DP_SETUP      (0u << 19)
#define TD_DP_OUT        (1u << 19)
#define TD_DP_IN         (2u << 19)
#define TD_DI_NONE       (7u << 21)
#define TD_T_CARRY       (0u << 24)
#define TD_T_DATA0       (2u << 24)
#define TD_T_DATA1       (3u << 24)
#define TD_CC_INIT       (0xEu << 28)
#define TD_CC(f)         ((f) >> 28)

#define BARRIER() __asm__ volatile("" ::: "memory")

typedef struct { volatile uint32_t dw[4]; } __attribute__((aligned(16))) ohci_desc_t;

/* Interrupt-IN endpoint: its own ED in the periodic table + one armed TD. */
typedef struct {
    bool            in_use;
    bool            armed;
    usb_device_t*   dev;
    usb_endpoint_t* ep;
    uint32_t        gen;
    int             len;
    ohci_desc_t     ed;
    ohci_desc_t     td;
    ohci_desc_t     dummy;
    uint8_t         buf[64] __attribute__((aligned(16)));
} ohci_islot_t;

typedef struct {
    usb_hcd_t*         hc;
    uint32_t           base;
    volatile uint32_t* r;
    int                nports;
    uint8_t            hcca[256] __attribute__((aligned(256)));
    ohci_desc_t        ctrl_ed;
    ohci_desc_t        td[OHCI_TDS];
    ohci_desc_t        dummy;
    uint8_t            setup[8] __attribute__((aligned(16)));
    ohci_islot_t       islot[OHCI_INTR_SLOTS];
} ohci_t;

static ohci_t ohcis[OHCI_MAX];
static int    ohci_count = 0;

/* --------------------------------------------------------------------------
 * TD helpers
 * -------------------------------------------------------------------------- */
static void td_fill(ohci_desc_t* td, uint32_t flags, uint8_t* buf, int len) {
    td->dw[1] = (len > 0) ? (uint32_t)buf : 0;
    td->dw[3] = (len > 0) ? (uint32_t)buf + (uint32_t)len - 1 : 0;
    td->dw[2] = 0;
    BARRIER();
    td->dw[0] = TD_CC_INIT | TD_DI_NONE | flags;
}

/* Bytes moved by a retired TD (CBP == 0 means everything). */
static int td_xferred(ohci_desc_t* td, uint8_t* buf, int len) {
    if (len <= 0) return 0;
    uint32_t cbp = td->dw[1];
    if (cbp == 0) return len;
    int n = (int)(cbp - (uint32_t)buf);
    if (n < 0 || n > len) n = len;
    return n;
}

static void ohci_ack_wdh(ohci_t* o) {
    if (o->r[R_INTSTATUS] & INT_WDH) o->r[R_INTSTATUS] = INT_WDH;
}

/* Run td[0..n-1] on the control ED. 0 = all retired, -1 = error/timeout. */
static int ohci_run(ohci_t* o, uint32_t ed0, int toggle, int n, int timeout_ms) {
    ohci_desc_t* ed = &o->ctrl_ed;
    for (int i = 0; i < n; i++)
        o->td[i].dw[2] = (i + 1 < n) ? (uint32_t)&o->td[i + 1] : (uint32_t)&o->dummy;
    ed->dw[0] = ed0 | ED_K;
    BARRIER();
    ed->dw[1] = (uint32_t)&o->dummy;                 /* tail */
    ed->dw[2] = (uint32_t)&o->td[0] | (toggle ? ED_TOGGLE : 0);
    ed->dw[3] = 0;
    BARRIER();
    ed->dw[0] = ed0;                                 /* un-skip */
    o->r[R_CMDSTATUS] = CS_CLF;                      /* control list has work */

    int iters = timeout_ms * 50;                     /* 20 us steps */
    int rc = -1;
    while (iters-- > 0) {
        uint32_t h = ed->dw[2];
        if (h & ED_HALTED) { rc = -2; break; }
        if ((h & ED_PTR_MASK) == (uint32_t)&o->dummy) { rc = 0; break; }
        usb_delay_us(20);
    }
    ohci_ack_wdh(o);
    if (rc != 0) {
        if (rc == -1) usb_log("OHCI: transfer timeout\n");
        ed->dw[0] = ed0 | ED_K;                      /* park the ED, drop the chain */
        BARRIER();
        usb_delay_ms(2);
        ed->dw[2] = (uint32_t)&o->dummy;             /* also clears Halted */
        BARRIER();
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Control / bulk
 * -------------------------------------------------------------------------- */
static int ohci_control(usb_hcd_t* hc, usb_device_t* dev, const uint8_t* setup,
                        void* data, int len, int dir_in) {
    ohci_t* o = (ohci_t*)hc->priv;
    if (len > 4096) len = 4096;
    memcpy(o->setup, setup, 8);
    uint32_t mps0 = dev->max_packet0 ? dev->max_packet0 : 8;
    if (mps0 > 64) mps0 = 64;
    uint32_t ed0 = ED_FA(dev->address) | ED_EN(0) | ED_D_TD | ED_MPS(mps0) |
                   (dev->speed == USB_SPEED_LOW ? ED_S : 0);

    td_fill(&o->td[0], TD_DP_SETUP | TD_T_DATA0, o->setup, 8);
    int n = 1;
    if (len) {
        td_fill(&o->td[1], (dir_in ? (TD_DP_IN | TD_R) : TD_DP_OUT) | TD_T_DATA1, (uint8_t*)data, len);
        n = 2;
    }
    int status_in = (len == 0) || !dir_in;
    td_fill(&o->td[n], (status_in ? (TD_DP_IN | TD_R) : TD_DP_OUT) | TD_T_DATA1, NULL, 0);
    n++;

    if (ohci_run(o, ed0, 0, n, 500) < 0) return -1;
    for (int i = 0; i < n; i++) if (TD_CC(o->td[i].dw[0]) != 0) return -1;
    return len ? td_xferred(&o->td[1], (uint8_t*)data, len) : 0;
}

static int ohci_bulk(usb_hcd_t* hc, usb_device_t* dev, usb_endpoint_t* ep,
                     void* buf, int len, int dir_in) {
    ohci_t* o = (ohci_t*)hc->priv;
    int mps = ep->max_packet_size ? ep->max_packet_size : 64;
    uint32_t ed0 = ED_FA(dev->address) | ED_EN(ep->address & 0x0F) | ED_D_TD | ED_MPS(mps) |
                   (dev->speed == USB_SPEED_LOW ? ED_S : 0);
    uint32_t flags = (dir_in ? (TD_DP_IN | TD_R) : TD_DP_OUT) | TD_T_CARRY;

    int lens[OHCI_TDS];
    uint8_t* bufs[OHCI_TDS];
    int n = 0, remaining = len;
    uint8_t* p = (uint8_t*)buf;
    if (len == 0) { td_fill(&o->td[0], flags, NULL, 0); lens[0] = 0; bufs[0] = NULL; n = 1; }
    while (remaining > 0 && n < OHCI_TDS) {
        int chunk = remaining > 4096 ? 4096 : remaining;   /* <= 2 pages per TD */
        td_fill(&o->td[n], flags, p, chunk);
        lens[n] = chunk;
        bufs[n] = p;
        p += chunk;
        remaining -= chunk;
        n++;
    }
    if (remaining > 0) return -1;

    int rc = ohci_run(o, ed0, ep->toggle, n, 3000);
    ep->toggle = (o->ctrl_ed.dw[2] & ED_TOGGLE) ? 1 : 0;   /* toggle carry */
    if (rc < 0) return -1;
    int total = 0;
    for (int i = 0; i < n; i++) {
        if (TD_CC(o->td[i].dw[0]) != 0) return -1;
        total += td_xferred(&o->td[i], bufs[i], lens[i]);
    }
    return total;
}

/* --------------------------------------------------------------------------
 * Interrupt IN: periodic ED per endpoint, one TD armed between polls.
 * -------------------------------------------------------------------------- */
static uint32_t ohci_islot_ed0(ohci_islot_t* s) {
    usb_device_t* dev = s->dev;
    usb_endpoint_t* ep = s->ep;
    return ED_FA(dev->address) | ED_EN(ep->address & 0x0F) | ED_D_TD |
           ED_MPS(ep->max_packet_size ? ep->max_packet_size : 8) |
           (dev->speed == USB_SPEED_LOW ? ED_S : 0);
}

static ohci_islot_t* ohci_islot_get(ohci_t* o, usb_device_t* dev, usb_endpoint_t* ep) {
    ohci_islot_t* s = (ohci_islot_t*)ep->hc_priv;
    if (s && s >= &o->islot[0] && s < &o->islot[OHCI_INTR_SLOTS] &&
        s->in_use && s->dev == dev && s->gen == dev->gen && s->ep == ep)
        return s;
    for (int i = 0; i < OHCI_INTR_SLOTS; i++) {
        s = &o->islot[i];
        if (s->in_use) continue;
        s->in_use = true;
        s->armed = false;
        s->dev = dev;
        s->ep = ep;
        s->gen = dev->gen;
        s->ed.dw[0] = ohci_islot_ed0(s) | ED_K;
        s->ed.dw[1] = (uint32_t)&s->dummy;
        s->ed.dw[2] = (uint32_t)&s->dummy;
        ep->hc_priv = s;
        return s;
    }
    return NULL;
}

static void ohci_intr_arm(ohci_islot_t* s) {
    usb_endpoint_t* ep = s->ep;
    int len = ep->max_packet_size;
    if (len <= 0) len = 8;
    if (len > 64) len = 64;
    s->len = len;
    td_fill(&s->td, TD_DP_IN | TD_R | TD_T_CARRY, s->buf, len);
    s->td.dw[2] = (uint32_t)&s->dummy;
    uint32_t ed0 = ohci_islot_ed0(s);
    s->ed.dw[0] = ed0 | ED_K;
    BARRIER();
    s->ed.dw[1] = (uint32_t)&s->dummy;
    s->ed.dw[2] = (uint32_t)&s->td | (ep->toggle ? ED_TOGGLE : 0);
    BARRIER();
    s->ed.dw[0] = ed0;
    s->armed = true;
}

static int ohci_interrupt_in(usb_hcd_t* hc, usb_device_t* dev, usb_endpoint_t* ep,
                             void* buf, int len) {
    ohci_t* o = (ohci_t*)hc->priv;
    ohci_islot_t* s = ohci_islot_get(o, dev, ep);
    if (!s) return -1;
    ohci_ack_wdh(o);

    if (s->armed) {
        uint32_t h = s->ed.dw[2];
        if (h & ED_HALTED) {
            s->armed = false;
            s->ed.dw[0] |= ED_K;
            BARRIER();
            s->ed.dw[2] = (uint32_t)&s->dummy;
            BARRIER();
            return -1;                               /* re-armed on the next poll */
        }
        if ((h & ED_PTR_MASK) != (uint32_t)&s->dummy) return 0;   /* still pending */
        s->armed = false;
        if (TD_CC(s->td.dw[0]) != 0) return -1;
        int n = td_xferred(&s->td, s->buf, s->len);
        ep->toggle = (h & ED_TOGGLE) ? 1 : 0;
        if (n > len) n = len;
        if (n > 0) memcpy(buf, s->buf, n);
        ohci_intr_arm(s);
        return n;
    }
    ohci_intr_arm(s);
    return 0;
}

static void ohci_release_device(usb_hcd_t* hc, usb_device_t* dev) {
    ohci_t* o = (ohci_t*)hc->priv;
    for (int i = 0; i < OHCI_INTR_SLOTS; i++) {
        ohci_islot_t* s = &o->islot[i];
        if (s->in_use && s->dev == dev) {
            s->ed.dw[0] |= ED_K;
            BARRIER();
            s->ed.dw[2] = (uint32_t)&s->dummy;
            BARRIER();
            s->in_use = false;
            s->armed = false;
            s->dev = NULL;
            s->ep = NULL;
        }
    }
}

/* --------------------------------------------------------------------------
 * Root hub ports. Writes only ever carry the specific set/clear bits: bit 0
 * written as 1 means ClearPortEnable and bit 9 ClearPortPower.
 * -------------------------------------------------------------------------- */
static int ohci_port_status(usb_hcd_t* hc, int port, int* changed) {
    ohci_t* o = (ohci_t*)hc->priv;
    uint32_t v = o->r[R_RHPORT(port)];
    *changed = (v & RHP_CSC) ? 1 : 0;
    if (v & RHP_CHANGE) o->r[R_RHPORT(port)] = v & RHP_CHANGE;
    ohci_ack_wdh(o);
    return (v & RHP_CCS) ? 1 : 0;
}

static int ohci_port_reset(usb_hcd_t* hc, int port) {
    ohci_t* o = (ohci_t*)hc->priv;
    uint32_t v = o->r[R_RHPORT(port)];
    if (!(v & RHP_CCS)) return -1;

    o->r[R_RHPORT(port)] = RHP_PRS;
    int ok = 0;
    for (int i = 0; i < 200; i++) {
        usb_delay_ms(1);
        v = o->r[R_RHPORT(port)];
        if (v & RHP_PRSC) { ok = 1; break; }
    }
    o->r[R_RHPORT(port)] = RHP_PRSC | RHP_PESC | RHP_CSC;
    if (!ok) return -1;
    v = o->r[R_RHPORT(port)];
    if (!(v & RHP_PES)) {
        o->r[R_RHPORT(port)] = RHP_PES;
        usb_delay_ms(1);
        v = o->r[R_RHPORT(port)];
        if (!(v & RHP_PES)) return -1;
    }
    usb_delay_ms(10);
    return (v & RHP_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
}

/* --------------------------------------------------------------------------
 * Controller bring-up
 * -------------------------------------------------------------------------- */
static bool ohci_setup(ohci_t* o, pci_device_t* pci) {
    uint32_t bar0 = pci->bar[0];
    if (bar0 & 1) return false;
    o->base = bar0 & 0xFFFFFFF0u;
    if (!o->base) return false;

    uint32_t cmd = pci_read32(pci->bus, pci->device, pci->function, PCI_COMMAND);
    pci_write32(pci->bus, pci->device, pci->function, PCI_COMMAND, cmd | 0x6);
    paging_map_mmio(o->base, 0x1000);
    o->r = (volatile uint32_t*)o->base;

    /* Take the controller from SMM firmware if it holds it. */
    if (o->r[R_CONTROL] & CTL_IR) {
        o->r[R_CMDSTATUS] = CS_OCR;
        for (int i = 0; i < 500 && (o->r[R_CONTROL] & CTL_IR); i++) usb_delay_ms(1);
    }
    uint32_t fi = o->r[R_FMINTERVAL] & 0x3FFF;
    if (fi < 0x2000) fi = 0x2EDF;

    /* Descriptors: control ED idle, interrupt EDs chained and skipped. */
    o->ctrl_ed.dw[0] = ED_K;
    o->ctrl_ed.dw[1] = (uint32_t)&o->dummy;
    o->ctrl_ed.dw[2] = (uint32_t)&o->dummy;
    o->ctrl_ed.dw[3] = 0;
    for (int i = 0; i < OHCI_INTR_SLOTS; i++) {
        ohci_islot_t* s = &o->islot[i];
        s->in_use = false;
        s->armed = false;
        s->ed.dw[0] = ED_K;
        s->ed.dw[1] = (uint32_t)&s->dummy;
        s->ed.dw[2] = (uint32_t)&s->dummy;
        s->ed.dw[3] = (i + 1 < OHCI_INTR_SLOTS) ? (uint32_t)&o->islot[i + 1].ed : 0;
    }
    memset(o->hcca, 0, sizeof(o->hcca));
    volatile uint32_t* itable = (volatile uint32_t*)o->hcca;
    for (int i = 0; i < 32; i++) itable[i] = (uint32_t)&o->islot[0].ed;
    BARRIER();

    /* Software reset (lands in USBSUSPEND; must go operational within 2 ms). */
    o->r[R_CMDSTATUS] = CS_HCR;
    for (int i = 0; i < 1000 && (o->r[R_CMDSTATUS] & CS_HCR); i++) usb_delay_us(10);
    o->r[R_HCCA] = (uint32_t)o->hcca;
    o->r[R_CTRLHEADED] = (uint32_t)&o->ctrl_ed;
    o->r[R_CTRLCURED] = 0;
    o->r[R_BULKHEADED] = 0;
    o->r[R_BULKCURED] = 0;
    o->r[R_INTDISABLE] = 0xC000007Fu;                /* polled: no interrupts */
    o->r[R_INTSTATUS] = 0x4000007Fu;
    uint32_t fsmps = ((fi - 210) * 6) / 7;
    o->r[R_FMINTERVAL] = (fsmps << 16) | fi | ((o->r[R_FMINTERVAL] ^ (1u << 31)) & (1u << 31));
    o->r[R_PERIODICSTART] = (fi * 9) / 10;
    o->r[R_LSTHRESH] = 0x628;
    o->r[R_CONTROL] = CTL_HCFS_OPER | CTL_CLE | CTL_BLE | CTL_PLE | CTL_CBSR3;
    usb_delay_ms(2);

    /* Root hub: power everything, wait for power-good. */
    uint32_t rha = o->r[R_RHDESCA];
    o->nports = rha & 0xFF;
    if (o->nports == 0 || o->nports > 15) o->nports = 2;
    o->r[R_RHSTATUS] = RHS_LPSC;
    for (int p = 1; p <= o->nports; p++) o->r[R_RHPORT(p)] = RHP_PPS;
    usb_delay_ms(((rha >> 24) & 0xFF) * 2 + 20);

    usb_log("OHCI: mmio "); usb_log_hex(o->base); usb_log(" ports="); usb_log_dec(o->nports);
    usb_log(" rev="); usb_log_hex(o->r[R_REVISION] & 0xFF); usb_log("\n");
    return true;
}

void usb_ohci_init(void) {
    pci_device_t* devs = pci_get_devices();
    int n = pci_device_count();
    for (int i = 0; i < n && ohci_count < OHCI_MAX; i++) {
        pci_device_t* p = &devs[i];
        if (!p->active || p->class_code != PCI_CLASS_SERIAL ||
            p->subclass != PCI_SUBCLASS_USB || p->prog_if != PCI_PROGIF_OHCI)
            continue;
        usb_hcd_t* hc = usb_hcd_alloc();
        if (!hc) break;
        ohci_t* o = &ohcis[ohci_count];
        memset(o, 0, sizeof(*o));
        if (!ohci_setup(o, p)) continue;
        o->hc = hc;
        hc->name = "OHCI";
        hc->type = USB_HCD_OHCI;
        hc->nports = (uint8_t)o->nports;
        hc->base = o->base;
        hc->pci = p;
        hc->priv = o;
        hc->port_status = ohci_port_status;
        hc->port_reset = ohci_port_reset;
        hc->control = ohci_control;
        hc->interrupt_in = ohci_interrupt_in;
        hc->bulk = ohci_bulk;
        hc->release_device = ohci_release_device;
        ohci_count++;
        usb_hcd_register(hc);
    }
}

/* ============================================================================
 * NexusOS — USB EHCI Host Controller Driver (USB 2.0) — Phase 55
 * ============================================================================
 * Polled EHCI driver. Asynchronous schedule: a head QH (H bit) plus a pool of
 * QHs, one per (device, endpoint) for control and bulk transfers, all linked
 * permanently in a ring; qTDs are built per transfer and the QH overlay is
 * pointed at them. Periodic schedule: a chain of interrupt QHs (one per
 * interrupt-IN endpoint) referenced by every frame-list entry, each with one
 * qTD kept armed between polls.
 *
 * Port routing: EHCI owns high-speed devices only. After a port reset, a port
 * that does not come up enabled holds a full-speed device (a K-state line
 * status before reset means low speed); such ports are handed to the
 * companion UHCI/OHCI controller by setting PORTSC.PortOwner, and the
 * companion driver (initialized afterwards) enumerates the device.
 * ============================================================================ */

#include "usb.h"
#include "pci.h"
#include "paging.h"
#include "string.h"

#define EHCI_MAX         2
#define EHCI_QH_SLOTS    16     /* async: control + bulk endpoints */
#define EHCI_INTR_SLOTS  8      /* periodic: interrupt-IN endpoints */
#define EHCI_QTDS        8
#define EHCI_UNUSED_ADDR 127    /* device address of idle QHs (never assigned) */

/* Operational registers (dword index) */
#define OP_USBCMD        0
#define OP_USBSTS        1
#define OP_USBINTR       2
#define OP_FRINDEX       3
#define OP_CTRLDSSEG     4
#define OP_PERIODICBASE  5
#define OP_ASYNCADDR     6
#define OP_CONFIGFLAG    16
#define OP_PORTSC(p)     (17 + ((p) - 1))

#define CMD_RS           (1u << 0)
#define CMD_HCRESET      (1u << 1)
#define CMD_PSE          (1u << 4)
#define CMD_ASE          (1u << 5)
#define CMD_IAAD         (1u << 6)
#define CMD_ITC8         (8u << 16)
#define STS_HCHALTED     (1u << 12)

/* PORTSC */
#define PSC_CCS          (1u << 0)
#define PSC_CSC          (1u << 1)
#define PSC_PED          (1u << 2)
#define PSC_PEC          (1u << 3)
#define PSC_OCC          (1u << 5)
#define PSC_PR           (1u << 8)
#define PSC_LS_MASK      (3u << 10)
#define PSC_LS_K         (1u << 10)
#define PSC_PP           (1u << 12)
#define PSC_PO           (1u << 13)
#define PSC_CHANGE       (PSC_CSC | PSC_PEC | PSC_OCC)

/* Link pointers */
#define LINK_T           1u
#define LINK_QH          (1u << 1)

/* qTD token */
#define QTD_ACTIVE       (1u << 7)
#define QTD_HALTED       (1u << 6)
#define QTD_ERRMASK      0x7Cu           /* halted, dbuf, babble, xact, missed uframe */
#define QTD_PID_OUT      (0u << 8)
#define QTD_PID_IN       (1u << 8)
#define QTD_PID_SETUP    (2u << 8)
#define QTD_CERR3        (3u << 10)
#define QTD_LEN(n)       ((uint32_t)(n) << 16)
#define QTD_DT           (1u << 31)
#define QTD_REM(tok)     (((tok) >> 16) & 0x7FFF)
#define QTD_PID(tok)     (((tok) >> 8) & 3)

/* QH endpoint characteristics / capabilities */
#define QH_DTC           (1u << 14)
#define QH_H             (1u << 15)
#define QH_CTRL_EP       (1u << 27)
#define QH_MULT1         (1u << 30)

#define BARRIER() __asm__ volatile("" ::: "memory")

typedef struct { volatile uint32_t dw[12]; uint32_t pad[4]; } __attribute__((aligned(64))) ehci_qh_t;
typedef struct { volatile uint32_t dw[8]; } __attribute__((aligned(32))) ehci_qtd_t;

/* Async QH bound to one (device, endpoint). ep == NULL means endpoint 0. */
typedef struct {
    bool            in_use;
    usb_device_t*   dev;
    usb_endpoint_t* ep;
    uint32_t        gen;
    ehci_qh_t       qh;
} ehci_aslot_t;

/* Periodic QH for one interrupt-IN endpoint with its armed qTD. */
typedef struct {
    bool            in_use;
    bool            armed;
    usb_device_t*   dev;
    usb_endpoint_t* ep;
    uint32_t        gen;
    int             len;
    ehci_qh_t       qh;
    ehci_qtd_t      qtd;
    uint8_t         buf[64] __attribute__((aligned(64)));
} ehci_islot_t;

typedef struct {
    usb_hcd_t*         hc;
    uint32_t           base;
    volatile uint32_t* op;
    int                nports;
    bool               has_companion;
    uint32_t           periodic[1024] __attribute__((aligned(4096)));
    ehci_qh_t          head_qh;
    ehci_aslot_t       aslot[EHCI_QH_SLOTS];
    ehci_islot_t       islot[EHCI_INTR_SLOTS];
    ehci_qtd_t         qtd[EHCI_QTDS];
    uint8_t            setup[8] __attribute__((aligned(32)));
} ehci_t;

static ehci_t ehcis[EHCI_MAX];
static int    ehci_count = 0;

/* --------------------------------------------------------------------------
 * QH / qTD helpers
 * -------------------------------------------------------------------------- */
static uint32_t ehci_unused_epchar(void) {
    return EHCI_UNUSED_ADDR | (2u << 12) | (64u << 16);
}

static void qh_idle(ehci_qh_t* qh, uint32_t epchar) {
    qh->dw[1] = epchar;
    qh->dw[2] = QH_MULT1;
    qh->dw[3] = 0;
    qh->dw[4] = LINK_T;
    qh->dw[5] = LINK_T;
    qh->dw[6] = 0;
    for (int i = 7; i < 12; i++) qh->dw[i] = 0;
}

/* Bytes a qTD can cover from `buf` with its 5 buffer pointers. */
static int qtd_capacity(uint8_t* buf) {
    return (int)(0x1000 - ((uint32_t)buf & 0xFFF)) + 4 * 0x1000;
}

static void qtd_fill(ehci_qtd_t* q, uint32_t next, uint32_t alt, uint32_t pid,
                     int len, int dt, uint8_t* buf) {
    q->dw[0] = next;
    q->dw[1] = alt;
    for (int i = 3; i < 8; i++) q->dw[i] = 0;
    if (len > 0 && buf) {
        uint32_t a = (uint32_t)buf;
        q->dw[3] = a;
        int covered = (int)(0x1000 - (a & 0xFFF));
        uint32_t page = (a & ~0xFFFu) + 0x1000;
        for (int i = 4; i < 8 && covered < len; i++) {
            q->dw[i] = page;
            page += 0x1000;
            covered += 0x1000;
        }
    }
    BARRIER();
    q->dw[2] = QTD_ACTIVE | pid | QTD_CERR3 | QTD_LEN(len) | (dt ? QTD_DT : 0);
}

static uint32_t ehci_eps(usb_device_t* dev) {
    if (dev->speed == USB_SPEED_LOW) return 1;
    if (dev->speed == USB_SPEED_FULL) return 0;
    return 2;
}

/* Split-transaction fields for a LS/FS device behind a HS hub:
 * hub address bits 16-22, hub port bits 23-29. */
static uint32_t ehci_split(usb_device_t* dev) {
    if (dev->speed >= USB_SPEED_HIGH || dev->parent < 0) return 0;
    usb_device_t* devs = usb_get_devices();
    int p = dev->parent;
    uint32_t port = dev->port;
    for (int guard = 0; guard < USB_MAX_HUB_DEPTH + 1 && p >= 0 && p < USB_MAX_DEVICES; guard++) {
        usb_device_t* h = &devs[p];
        if (h->speed == USB_SPEED_HIGH && h->hcd == dev->hcd)
            return ((uint32_t)h->address << 16) | (port << 23);
        port = h->port;
        p = h->parent;
    }
    return 0;
}

static uint32_t ehci_epchar(usb_device_t* dev, uint32_t epnum, uint32_t mps, int is_ctrl) {
    uint32_t eps = ehci_eps(dev);
    uint32_t v = (dev->address & 0x7F) | ((epnum & 0xF) << 8) | (eps << 12) | QH_DTC | ((mps & 0x7FF) << 16);
    if (is_ctrl && eps != 2) v |= QH_CTRL_EP;
    return v;
}

static uint32_t ehci_epcap(usb_device_t* dev, int is_intr) {
    uint32_t v = QH_MULT1 | ehci_split(dev);
    if (is_intr) {
        v |= 0x01;                                   /* S-mask: microframe 0 */
        if (dev->speed < USB_SPEED_HIGH) v |= (0x1Cu << 8);   /* C-mask for splits */
    }
    return v;
}

/* --------------------------------------------------------------------------
 * Async QH slots
 * -------------------------------------------------------------------------- */
static ehci_aslot_t* ehci_aslot_get(ehci_t* e, usb_device_t* dev, usb_endpoint_t* ep) {
    ehci_aslot_t* s = ep ? (ehci_aslot_t*)ep->hc_priv : (ehci_aslot_t*)dev->hc_priv;
    if (s && s >= &e->aslot[0] && s < &e->aslot[EHCI_QH_SLOTS] &&
        s->in_use && s->dev == dev && s->gen == dev->gen && s->ep == ep)
        return s;
    for (int i = 0; i < EHCI_QH_SLOTS; i++) {
        s = &e->aslot[i];
        if (s->in_use) continue;
        s->in_use = true;
        s->dev = dev;
        s->ep = ep;
        s->gen = dev->gen;
        if (ep) ep->hc_priv = s; else dev->hc_priv = s;
        return s;
    }
    return NULL;
}

/* Run a qTD chain on an idle async QH. stop_on_short: a short IN packet ends
 * the transfer (bulk); control transfers continue to the status stage via
 * the alternate-next pointer instead. 0 ok, -1 error/timeout. */
static int ehci_run(ehci_t* e, ehci_qh_t* qh, uint32_t epchar, uint32_t epcap,
                    ehci_qtd_t* q, int n, int stop_on_short, int timeout_ms) {
    qh->dw[1] = epchar;
    qh->dw[2] = epcap;
    qh->dw[3] = 0;
    qh->dw[5] = LINK_T;
    qh->dw[6] = 0;
    for (int i = 7; i < 12; i++) qh->dw[i] = 0;
    BARRIER();
    qh->dw[4] = (uint32_t)&q[0];

    int iters = timeout_ms * 50;                     /* 20 us steps */
    int done = 0, err = 0;
    for (;;) {
        int shortp = 0;
        done = 0; err = 0;
        for (int i = 0; i < n; i++) {
            uint32_t tok = q[i].dw[2];
            if (tok & QTD_ACTIVE) { if (shortp && stop_on_short) done = 1; break; }
            if (tok & QTD_ERRMASK) { err = 1; done = 1; break; }
            if (QTD_REM(tok) && QTD_PID(tok) == 1) shortp = 1;
            if (i == n - 1) done = 1;
        }
        if (done) break;
        if (--iters <= 0) break;
        usb_delay_us(20);
    }
    (void)e;
    if (!done) {
        usb_log("EHCI: transfer timeout\n");
        qh->dw[6] = QTD_HALTED;                      /* park the QH */
        qh->dw[4] = LINK_T;
        BARRIER();
        usb_delay_ms(2);
        qh->dw[6] = 0;
        return -1;
    }
    if (err) {
        qh->dw[6] = 0;                               /* clear the halt */
        qh->dw[4] = LINK_T;
        BARRIER();
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Control / bulk
 * -------------------------------------------------------------------------- */
static int ehci_control(usb_hcd_t* hc, usb_device_t* dev, const uint8_t* setup,
                        void* data, int len, int dir_in) {
    ehci_t* e = (ehci_t*)hc->priv;
    ehci_aslot_t* s = ehci_aslot_get(e, dev, NULL);
    if (!s) return -1;
    if (len > 16384) len = 16384;

    memcpy(e->setup, setup, 8);
    uint32_t mps0 = dev->max_packet0 ? dev->max_packet0 : 8;
    if (mps0 > 64) mps0 = 64;
    uint32_t epchar = ehci_epchar(dev, 0, mps0, 1);
    uint32_t epcap  = ehci_epcap(dev, 0);

    qtd_fill(&e->qtd[0], (uint32_t)&e->qtd[1], LINK_T, QTD_PID_SETUP, 8, 0, e->setup);
    int si = 1;
    if (len) {
        qtd_fill(&e->qtd[1], (uint32_t)&e->qtd[2], (uint32_t)&e->qtd[2],
                 dir_in ? QTD_PID_IN : QTD_PID_OUT, len, 1, (uint8_t*)data);
        si = 2;
    }
    int status_in = (len == 0) || !dir_in;
    qtd_fill(&e->qtd[si], LINK_T, LINK_T, status_in ? QTD_PID_IN : QTD_PID_OUT, 0, 1, NULL);

    if (ehci_run(e, &s->qh, epchar, epcap, e->qtd, si + 1, 0, 500) < 0) return -1;
    if (!len) return 0;
    return len - (int)QTD_REM(e->qtd[1].dw[2]);
}

static int ehci_bulk(usb_hcd_t* hc, usb_device_t* dev, usb_endpoint_t* ep,
                     void* buf, int len, int dir_in) {
    ehci_t* e = (ehci_t*)hc->priv;
    ehci_aslot_t* s = ehci_aslot_get(e, dev, ep);
    if (!s) return -1;
    int mps = ep->max_packet_size ? ep->max_packet_size : 64;
    uint32_t pid = dir_in ? QTD_PID_IN : QTD_PID_OUT;
    uint32_t epchar = ehci_epchar(dev, ep->address & 0x0F, (uint32_t)mps, 0);
    uint32_t epcap  = ehci_epcap(dev, 0);

    int lens[EHCI_QTDS];
    int n = 0, pkts = 0, remaining = len;
    uint8_t* p = (uint8_t*)buf;
    if (len == 0) {
        qtd_fill(&e->qtd[0], LINK_T, LINK_T, pid, 0, ep->toggle, NULL);
        lens[0] = 0;
        n = 1;
    }
    while (remaining > 0 && n < EHCI_QTDS) {
        int chunk = remaining;
        int cap = qtd_capacity(p);
        if (chunk > cap) chunk = cap;
        if (chunk > 16384) chunk = 16384;
        qtd_fill(&e->qtd[n], LINK_T, LINK_T, pid, chunk, (ep->toggle ^ (pkts & 1)) & 1, p);
        if (n > 0) e->qtd[n - 1].dw[0] = (uint32_t)&e->qtd[n];
        lens[n] = chunk;
        pkts += (chunk + mps - 1) / mps;
        p += chunk;
        remaining -= chunk;
        n++;
    }
    if (remaining > 0) return -1;                    /* transfer too large */

    int rc = ehci_run(e, &s->qh, epchar, epcap, e->qtd, n, 1, 3000);

    int total = 0, donepkts = 0;
    for (int i = 0; i < n; i++) {
        uint32_t tok = e->qtd[i].dw[2];
        if (tok & QTD_ACTIVE) break;
        if (tok & QTD_ERRMASK) break;
        int rem = (int)QTD_REM(tok);
        int x = lens[i] - rem;
        if (x < 0) x = 0;
        total += x;
        donepkts += x ? (x + mps - 1) / mps : 1;
        if (rem) break;
    }
    ep->toggle ^= (uint8_t)(donepkts & 1);
    if (rc < 0) return -1;
    return total;
}

/* --------------------------------------------------------------------------
 * Interrupt IN: periodic QH per endpoint, one qTD armed between polls.
 * -------------------------------------------------------------------------- */
static ehci_islot_t* ehci_islot_get(ehci_t* e, usb_device_t* dev, usb_endpoint_t* ep) {
    ehci_islot_t* s = (ehci_islot_t*)ep->hc_priv;
    if (s && s >= &e->islot[0] && s < &e->islot[EHCI_INTR_SLOTS] &&
        s->in_use && s->dev == dev && s->gen == dev->gen && s->ep == ep)
        return s;
    for (int i = 0; i < EHCI_INTR_SLOTS; i++) {
        s = &e->islot[i];
        if (s->in_use) continue;
        s->in_use = true;
        s->armed = false;
        s->dev = dev;
        s->ep = ep;
        s->gen = dev->gen;
        qh_idle(&s->qh, ehci_epchar(dev, ep->address & 0x0F, ep->max_packet_size, 0));
        s->qh.dw[2] = ehci_epcap(dev, 1);
        ep->hc_priv = s;
        return s;
    }
    return NULL;
}

static void ehci_intr_arm(ehci_islot_t* s) {
    usb_endpoint_t* ep = s->ep;
    int len = ep->max_packet_size;
    if (len <= 0) len = 8;
    if (len > 64) len = 64;
    s->len = len;
    qtd_fill(&s->qtd, LINK_T, LINK_T, QTD_PID_IN, len, ep->toggle, s->buf);
    s->qh.dw[1] = ehci_epchar(s->dev, ep->address & 0x0F, ep->max_packet_size, 0);
    s->qh.dw[5] = LINK_T;
    s->qh.dw[6] = 0;
    BARRIER();
    s->qh.dw[4] = (uint32_t)&s->qtd;
    s->armed = true;
}

static int ehci_interrupt_in(usb_hcd_t* hc, usb_device_t* dev, usb_endpoint_t* ep,
                             void* buf, int len) {
    ehci_t* e = (ehci_t*)hc->priv;
    ehci_islot_t* s = ehci_islot_get(e, dev, ep);
    if (!s) return -1;

    if (s->armed) {
        uint32_t tok = s->qtd.dw[2];
        if (tok & QTD_ACTIVE) return 0;
        s->armed = false;
        if (tok & QTD_ERRMASK) {
            s->qh.dw[6] = 0;
            s->qh.dw[4] = LINK_T;
            BARRIER();
            return -1;                               /* re-armed on the next poll */
        }
        int n = s->len - (int)QTD_REM(tok);
        if (n < 0) n = 0;
        ep->toggle ^= 1;
        if (n > len) n = len;
        if (n > 0) memcpy(buf, s->buf, n);
        ehci_intr_arm(s);
        return n;
    }
    ehci_intr_arm(s);
    return 0;
}

static void ehci_release_device(usb_hcd_t* hc, usb_device_t* dev) {
    ehci_t* e = (ehci_t*)hc->priv;
    for (int i = 0; i < EHCI_QH_SLOTS; i++) {
        ehci_aslot_t* s = &e->aslot[i];
        if (s->in_use && s->dev == dev) {
            qh_idle(&s->qh, ehci_unused_epchar());
            BARRIER();
            s->in_use = false;
            s->dev = NULL;
            s->ep = NULL;
        }
    }
    for (int i = 0; i < EHCI_INTR_SLOTS; i++) {
        ehci_islot_t* s = &e->islot[i];
        if (s->in_use && s->dev == dev) {
            qh_idle(&s->qh, ehci_unused_epchar());
            BARRIER();
            s->in_use = false;
            s->armed = false;
            s->dev = NULL;
            s->ep = NULL;
        }
    }
    dev->hc_priv = NULL;
}

/* --------------------------------------------------------------------------
 * Root hub ports + companion handoff
 * -------------------------------------------------------------------------- */
static void ehci_handoff(ehci_t* e, int port, uint32_t v) {
    if (!e->has_companion) {
        usb_log("EHCI: port "); usb_log_dec(port);
        usb_log(": full/low-speed device but no companion controller\n");
        return;
    }
    e->op[OP_PORTSC(port)] = (v & ~(PSC_CHANGE | PSC_PR | PSC_PED)) | PSC_PO;
    usb_log("EHCI: port "); usb_log_dec(port); usb_log(" handed to companion controller\n");
}

static int ehci_port_status(usb_hcd_t* hc, int port, int* changed) {
    ehci_t* e = (ehci_t*)hc->priv;
    uint32_t v = e->op[OP_PORTSC(port)];
    if (v & PSC_PO) { *changed = 0; return 0; }    /* companion-owned: never touch */
    *changed = (v & PSC_CSC) ? 1 : 0;
    if (v & PSC_CHANGE) e->op[OP_PORTSC(port)] = v;  /* W1C change bits, rest as read */
    return (v & PSC_CCS) ? 1 : 0;
}

static int ehci_port_reset(usb_hcd_t* hc, int port) {
    ehci_t* e = (ehci_t*)hc->priv;
    uint32_t v = e->op[OP_PORTSC(port)];
    if (!(v & PSC_CCS) || (v & PSC_PO)) return -1;

    if ((v & PSC_LS_MASK) == PSC_LS_K) {             /* K-state: low-speed device */
        ehci_handoff(e, port, v);
        return -1;
    }
    e->op[OP_PORTSC(port)] = (v & ~(PSC_PED | PSC_CHANGE)) | PSC_PR;
    usb_delay_ms(50);
    v = e->op[OP_PORTSC(port)];
    e->op[OP_PORTSC(port)] = v & ~(PSC_PR | PSC_CHANGE);
    for (int i = 0; i < 20 && (e->op[OP_PORTSC(port)] & PSC_PR); i++) usb_delay_ms(1);
    usb_delay_ms(2);
    v = e->op[OP_PORTSC(port)];
    if (!(v & PSC_CCS)) return -1;
    if (v & PSC_PED) {
        usb_delay_ms(10);
        return USB_SPEED_HIGH;
    }
    ehci_handoff(e, port, v);                        /* full-speed device */
    return -1;
}

/* --------------------------------------------------------------------------
 * Controller bring-up
 * -------------------------------------------------------------------------- */
static bool ehci_setup(ehci_t* e, pci_device_t* pci) {
    uint32_t bar0 = pci->bar[0];
    if (bar0 & 1) return false;
    e->base = bar0 & 0xFFFFFFF0u;
    if (!e->base) return false;

    uint32_t cmd = pci_read32(pci->bus, pci->device, pci->function, PCI_COMMAND);
    pci_write32(pci->bus, pci->device, pci->function, PCI_COMMAND, cmd | 0x6);
    paging_map_mmio(e->base, 0x1000);

    volatile uint32_t* cap = (volatile uint32_t*)e->base;
    uint32_t caplen = cap[0] & 0xFF;
    uint32_t hcs = cap[1];
    uint32_t hcc = cap[2];
    e->op = (volatile uint32_t*)(e->base + caplen);
    e->nports = hcs & 0xF;
    e->has_companion = ((hcs >> 12) & 0xF) != 0;

    /* BIOS -> OS handoff through the EECP legacy-support capability. */
    uint32_t eecp = (hcc >> 8) & 0xFF;
    if (eecp >= 0x40) {
        uint32_t legsup = pci_read32(pci->bus, pci->device, pci->function, (uint8_t)eecp);
        if (legsup & (1u << 16)) {
            pci_write32(pci->bus, pci->device, pci->function, (uint8_t)eecp, legsup | (1u << 24));
            for (int i = 0; i < 1000; i++) {
                usb_delay_ms(1);
                if (!(pci_read32(pci->bus, pci->device, pci->function, (uint8_t)eecp) & (1u << 16))) break;
            }
        }
        pci_write32(pci->bus, pci->device, pci->function, (uint8_t)(eecp + 4), 0);   /* no SMIs */
    }

    /* Halt, then reset. */
    if (!(e->op[OP_USBSTS] & STS_HCHALTED)) {
        e->op[OP_USBCMD] &= ~CMD_RS;
        for (int i = 0; i < 100 && !(e->op[OP_USBSTS] & STS_HCHALTED); i++) usb_delay_ms(1);
    }
    e->op[OP_USBCMD] = CMD_HCRESET;
    {
        int i;
        for (i = 0; i < 1000 && (e->op[OP_USBCMD] & CMD_HCRESET); i++) usb_delay_ms(1);
        if (i >= 1000) { usb_log("EHCI: reset timeout\n"); return false; }
    }

    /* Async ring: head -> aslot[0] -> ... -> aslot[N-1] -> head. */
    qh_idle(&e->head_qh, ehci_unused_epchar());
    e->head_qh.dw[1] |= QH_H;
    e->head_qh.dw[6] = QTD_HALTED;                   /* never executes */
    e->head_qh.dw[0] = (uint32_t)&e->aslot[0].qh | LINK_QH;
    for (int i = 0; i < EHCI_QH_SLOTS; i++) {
        ehci_aslot_t* s = &e->aslot[i];
        s->in_use = false;
        qh_idle(&s->qh, ehci_unused_epchar());
        s->qh.dw[0] = (i + 1 < EHCI_QH_SLOTS) ? ((uint32_t)&e->aslot[i + 1].qh | LINK_QH)
                                              : ((uint32_t)&e->head_qh | LINK_QH);
    }
    /* Periodic: every frame -> islot[0] -> ... -> islot[N-1] -> end. */
    for (int i = 0; i < EHCI_INTR_SLOTS; i++) {
        ehci_islot_t* s = &e->islot[i];
        s->in_use = false;
        s->armed = false;
        qh_idle(&s->qh, ehci_unused_epchar());
        s->qh.dw[0] = (i + 1 < EHCI_INTR_SLOTS) ? ((uint32_t)&e->islot[i + 1].qh | LINK_QH) : LINK_T;
    }
    for (int i = 0; i < 1024; i++) e->periodic[i] = (uint32_t)&e->islot[0].qh | LINK_QH;
    BARRIER();

    if (hcc & 1) e->op[OP_CTRLDSSEG] = 0;            /* 64-bit capable: high dword 0 */
    e->op[OP_PERIODICBASE] = (uint32_t)e->periodic;
    e->op[OP_ASYNCADDR] = (uint32_t)&e->head_qh;
    e->op[OP_USBINTR] = 0;                           /* polled */
    e->op[OP_USBSTS] = 0x3F;                         /* clear status */
    e->op[OP_USBCMD] = CMD_RS | CMD_PSE | CMD_ASE | CMD_ITC8;
    for (int i = 0; i < 100 && (e->op[OP_USBSTS] & STS_HCHALTED); i++) usb_delay_ms(1);
    e->op[OP_CONFIGFLAG] = 1;                        /* route all ports to EHCI */
    usb_delay_ms(50);

    usb_log("EHCI: mmio "); usb_log_hex(e->base); usb_log(" ports="); usb_log_dec(e->nports);
    usb_log(e->has_companion ? " companions=yes\n" : " companions=no\n");
    return true;
}

void usb_ehci_init(void) {
    pci_device_t* devs = pci_get_devices();
    int n = pci_device_count();
    for (int i = 0; i < n && ehci_count < EHCI_MAX; i++) {
        pci_device_t* p = &devs[i];
        if (!p->active || p->class_code != PCI_CLASS_SERIAL ||
            p->subclass != PCI_SUBCLASS_USB || p->prog_if != PCI_PROGIF_EHCI)
            continue;
        usb_hcd_t* hc = usb_hcd_alloc();
        if (!hc) break;
        ehci_t* e = &ehcis[ehci_count];
        memset(e, 0, sizeof(*e));
        if (!ehci_setup(e, p)) continue;
        e->hc = hc;
        hc->name = "EHCI";
        hc->type = USB_HCD_EHCI;
        hc->nports = (uint8_t)e->nports;
        hc->base = e->base;
        hc->pci = p;
        hc->priv = e;
        hc->port_status = ehci_port_status;
        hc->port_reset = ehci_port_reset;
        hc->control = ehci_control;
        hc->interrupt_in = ehci_interrupt_in;
        hc->bulk = ehci_bulk;
        hc->release_device = ehci_release_device;
        ehci_count++;
        usb_hcd_register(hc);
    }
}

/* ============================================================================
 * NexusOS — USB xHCI Host Controller Driver (USB 3) — Phase 55
 * ============================================================================
 * Polled xHCI driver (no interrupts, 32-bit DMA). Capability/operational/
 * runtime/doorbell register blocks, BIOS->OS handoff through the extended
 * capabilities, HC reset, DCBAA (+ scratchpad buffers when asked for), one
 * command ring, one event ring (single-segment ERST), per-device slot with
 * input/output contexts and one transfer ring per endpoint.
 *
 * Enumeration hooks used by the core: enable_device (Enable Slot + Address
 * Device), update_mps0 (Evaluate Context), configure (Configure Endpoint),
 * release_device (Disable Slot). Transfers: control = Setup(IDT)+Data+Status
 * TRBs; bulk = Normal TRBs split at 64 KB boundaries; interrupt IN = one
 * Normal TRB kept armed between polls. Events are consumed by a single
 * dispatcher that routes Transfer Events to the owning ring by slot/DCI.
 * Devices behind hubs get the route string and (for LS/FS behind a HS hub)
 * the TT hub slot/port in the slot context.
 * ============================================================================ */

#include "usb.h"
#include "pci.h"
#include "paging.h"
#include "string.h"

#define XHCI_MAX         2
#define XHCI_CMD_TRBS    32
#define XHCI_EVT_TRBS    64
#define XHCI_XFER_TRBS   32
#define XHCI_EPS         (USB_MAX_ENDPOINTS + 1)      /* [0] = EP0 */
#define XHCI_SCRATCH_MAX 32
#define XHCI_MAX_PORTS   32

/* TRB types */
#define TRB_NORMAL        1
#define TRB_SETUP         2
#define TRB_DATA          3
#define TRB_STATUS        4
#define TRB_LINK          6
#define TRB_ENABLE_SLOT   9
#define TRB_DISABLE_SLOT  10
#define TRB_ADDRESS_DEV   11
#define TRB_CONFIG_EP     12
#define TRB_EVAL_CTX      13
#define TRB_RESET_EP      14
#define TRB_STOP_EP       15
#define TRB_SET_TR_DEQ    16
#define TRB_EV_TRANSFER   32
#define TRB_EV_CMD        33
#define TRB_EV_PORT       34

/* TRB control bits */
#define TRB_C        (1u << 0)
#define TRB_TC       (1u << 1)
#define TRB_ISP      (1u << 2)
#define TRB_CH       (1u << 4)
#define TRB_IOC      (1u << 5)
#define TRB_IDT      (1u << 6)
#define TRB_DIR_IN   (1u << 16)
#define TRB_TYPE(t)  ((uint32_t)(t) << 10)

/* Completion codes */
#define CC_SUCCESS   1
#define CC_STALL     6
#define CC_SHORT     13

/* Operational registers (dword index from op base) */
#define OP_USBCMD     0
#define OP_USBSTS     1
#define OP_PAGESIZE   2
#define OP_CRCR_LO    6
#define OP_CRCR_HI    7
#define OP_DCBAAP_LO  12
#define OP_DCBAAP_HI  13
#define OP_CONFIG     14
#define OP_PORTSC(p)  (256 + 4 * ((p) - 1))

#define CMD_RS        (1u << 0)
#define CMD_HCRST     (1u << 1)
#define STS_HCH       (1u << 0)
#define STS_CNR       (1u << 11)

/* Runtime registers, interrupter 0 (dword index from runtime base) */
#define RT_IMAN       8
#define RT_IMOD       9
#define RT_ERSTSZ     10
#define RT_ERSTBA_LO  12
#define RT_ERSTBA_HI  13
#define RT_ERDP_LO    14
#define RT_ERDP_HI    15

/* PORTSC */
#define PSC_CCS       (1u << 0)
#define PSC_PED       (1u << 1)
#define PSC_PR        (1u << 4)
#define PSC_PP        (1u << 9)
#define PSC_SPEED(v)  (((v) >> 10) & 0xF)
#define PSC_CSC       (1u << 17)
#define PSC_PEC       (1u << 18)
#define PSC_WRC       (1u << 19)
#define PSC_OCC       (1u << 20)
#define PSC_PRC       (1u << 21)
#define PSC_PLC       (1u << 22)
#define PSC_CEC       (1u << 23)
#define PSC_CHANGE    (PSC_CSC | PSC_PEC | PSC_WRC | PSC_OCC | PSC_PRC | PSC_PLC | PSC_CEC)
#define PSC_PRESERVE  (PSC_PP | (3u << 14) | (7u << 25))   /* PP, PIC, wake enables */

#define BARRIER() __asm__ volatile("" ::: "memory")

typedef struct { volatile uint32_t dw[4]; } __attribute__((aligned(16))) xhci_trb_t;

/* A transfer ring + the bookkeeping for the TD in flight on it. */
typedef struct {
    xhci_trb_t trb[XHCI_XFER_TRBS] __attribute__((aligned(64)));
    uint32_t enq;
    uint32_t pcs;
    volatile int      done;        /* completion event for want_trb seen */
    volatile int      pending;     /* a TD is queued */
    uint32_t          want_trb;    /* address of the TRB whose event ends the TD */
    volatile uint32_t cc;          /* completion code */
    volatile uint32_t resid;       /* accumulated residual bytes */
    uint8_t  dci;                  /* endpoint context index (0 = unused) */
    int      buf_len;              /* interrupt-IN arm length */
    uint8_t  buf[64] __attribute__((aligned(64)));
} xhci_ring_t;

struct xhci;
typedef struct {
    uint8_t      ctx_out[4096] __attribute__((aligned(4096)));  /* device context */
    uint8_t      ctx_in[4096]  __attribute__((aligned(4096)));  /* input context */
    xhci_ring_t  ep[XHCI_EPS];                                  /* [0]=EP0, [1+i]=endpoints[i] */
    uint8_t      ring_of_dci[32];                               /* 0xFF = none */
    bool         in_use;
    uint8_t      slot;
    struct xhci* x;
} xhci_dev_t;

typedef struct xhci {
    usb_hcd_t*         hc;
    uint32_t           base;
    volatile uint32_t* cap;
    volatile uint32_t* op;
    volatile uint32_t* rt;
    volatile uint32_t* db;
    int                csz;        /* context size: 32 or 64 */
    int                max_slots;
    int                nports;
    uint8_t            port_usb3[XHCI_MAX_PORTS + 1];
    uint64_t           dcbaa[256]                 __attribute__((aligned(4096)));
    xhci_trb_t         cmd_ring[XHCI_CMD_TRBS]    __attribute__((aligned(64)));
    xhci_trb_t         evt_ring[XHCI_EVT_TRBS]    __attribute__((aligned(64)));
    uint64_t           erst[8]                    __attribute__((aligned(64)));
    uint64_t           scratch_arr[XHCI_SCRATCH_MAX] __attribute__((aligned(64)));
    uint32_t           cmd_enq, cmd_pcs;
    uint32_t           evt_deq, evt_ccs;
    volatile int       cmd_done;
    volatile uint32_t  cmd_cc, cmd_slot, cmd_trb;
    xhci_dev_t*        slot_dev[256];
} xhci_t;

/* All in .bss: zeroed at boot, identity-mapped, below 32 MB (DMA-able). */
static xhci_t     xhcis[XHCI_MAX];
static int        xhci_count = 0;
static xhci_dev_t xdevs[USB_MAX_DEVICES];
static uint8_t    scratch_pages[XHCI_SCRATCH_MAX][4096] __attribute__((aligned(4096)));
static int        scratch_used = 0;

static inline volatile uint32_t* xctx(xhci_t* x, uint8_t* base, int idx) {
    return (volatile uint32_t*)(base + idx * x->csz);
}

/* --------------------------------------------------------------------------
 * Rings
 * -------------------------------------------------------------------------- */
static void ring_init(xhci_ring_t* r) {
    memset((void*)r->trb, 0, sizeof(r->trb));
    r->enq = 0;
    r->pcs = 1;
    r->done = 0;
    r->pending = 0;
    r->want_trb = 0;
    r->cc = 0;
    r->resid = 0;
    xhci_trb_t* l = &r->trb[XHCI_XFER_TRBS - 1];
    l->dw[0] = (uint32_t)&r->trb[0];
    l->dw[1] = 0;
    l->dw[2] = 0;
    l->dw[3] = TRB_TYPE(TRB_LINK) | TRB_TC;        /* cycle 0: not yet valid */
}

static xhci_trb_t* ring_enq(xhci_ring_t* r, uint32_t p0, uint32_t p1, uint32_t st, uint32_t ctrl) {
    xhci_trb_t* t = &r->trb[r->enq];
    t->dw[0] = p0;
    t->dw[1] = p1;
    t->dw[2] = st;
    BARRIER();
    t->dw[3] = (ctrl & ~TRB_C) | r->pcs;           /* cycle bit written last */
    BARRIER();
    if (++r->enq == XHCI_XFER_TRBS - 1) {
        xhci_trb_t* l = &r->trb[XHCI_XFER_TRBS - 1];
        l->dw[0] = (uint32_t)&r->trb[0];
        l->dw[1] = 0;
        l->dw[2] = 0;
        BARRIER();
        l->dw[3] = TRB_TYPE(TRB_LINK) | TRB_TC | r->pcs;
        BARRIER();
        r->enq = 0;
        r->pcs ^= 1;
    }
    return t;
}

/* Queue a data buffer as one or more TRBs (split at 64 KB boundaries).
 * `first_type` is TRB_DATA for a control data stage, else TRB_NORMAL.
 * Returns the last TRB. */
static xhci_trb_t* ring_queue_buf(xhci_ring_t* r, uint32_t first_type, uint8_t* buf, int len,
                                  int dir_in, uint32_t last_flags, int mps) {
    xhci_trb_t* last = NULL;
    int first = 1;
    if (mps <= 0) mps = 8;
    while (len > 0) {
        uint32_t a = (uint32_t)buf;
        uint32_t upto = (a | 0xFFFFu) + 1;          /* next 64 KB boundary */
        int chunk = (int)(upto - a);
        if (chunk > len) chunk = len;
        int is_last = (chunk == len);
        uint32_t td_size = 0;
        if (!is_last) {
            td_size = (uint32_t)((len - chunk + mps - 1) / mps);
            if (td_size > 31) td_size = 31;
        }
        uint32_t type = first ? first_type : TRB_NORMAL;
        uint32_t ctrl = TRB_TYPE(type) | TRB_ISP | (is_last ? last_flags : TRB_CH);
        if (type == TRB_DATA && dir_in) ctrl |= TRB_DIR_IN;
        last = ring_enq(r, a, 0, (uint32_t)chunk | (td_size << 17), ctrl);
        buf += chunk;
        len -= chunk;
        first = 0;
    }
    return last;
}

/* --------------------------------------------------------------------------
 * Event ring
 * -------------------------------------------------------------------------- */
static void xhci_handle_events(xhci_t* x) {
    for (int guard = 0; guard < XHCI_EVT_TRBS * 2; guard++) {
        xhci_trb_t* e = &x->evt_ring[x->evt_deq];
        uint32_t c = e->dw[3];
        if ((c & TRB_C) != x->evt_ccs) break;        /* no more events */
        BARRIER();
        uint32_t type = (c >> 10) & 0x3F;
        if (type == TRB_EV_CMD) {
            x->cmd_trb  = e->dw[0];
            x->cmd_cc   = e->dw[2] >> 24;
            x->cmd_slot = c >> 24;
            x->cmd_done = 1;
        } else if (type == TRB_EV_TRANSFER) {
            uint32_t slot = c >> 24;
            uint32_t dci  = (c >> 16) & 0x1F;
            uint32_t cc   = e->dw[2] >> 24;
            uint32_t resid = e->dw[2] & 0xFFFFFF;
            xhci_dev_t* xd = (slot < 256) ? x->slot_dev[slot] : NULL;
            if (xd && xd->in_use && xd->ring_of_dci[dci] < XHCI_EPS) {
                xhci_ring_t* r = &xd->ep[xd->ring_of_dci[dci]];
                if (r->pending) {
                    r->resid += resid;
                    if (e->dw[0] == r->want_trb || (cc != CC_SUCCESS && cc != CC_SHORT)) {
                        r->cc = cc;
                        r->done = 1;
                        r->pending = 0;
                    }
                }
            }
        }
        /* TRB_EV_PORT and others: ignored, usb_poll() reads PORTSC directly. */
        if (++x->evt_deq >= XHCI_EVT_TRBS) { x->evt_deq = 0; x->evt_ccs ^= 1; }
        uint32_t erdp = (uint32_t)&x->evt_ring[x->evt_deq];
        x->rt[RT_ERDP_HI] = 0;
        x->rt[RT_ERDP_LO] = erdp | (1u << 3);        /* EHB: clear busy */
    }
}

/* --------------------------------------------------------------------------
 * Commands
 * -------------------------------------------------------------------------- */
static int xhci_cmd(xhci_t* x, uint32_t p0, uint32_t p1, uint32_t ctrl, uint32_t* slot_out) {
    xhci_trb_t* t = &x->cmd_ring[x->cmd_enq];
    t->dw[0] = p0;
    t->dw[1] = p1;
    t->dw[2] = 0;
    BARRIER();
    t->dw[3] = (ctrl & ~TRB_C) | x->cmd_pcs;
    BARRIER();
    uint32_t addr = (uint32_t)t;
    if (++x->cmd_enq == XHCI_CMD_TRBS - 1) {
        xhci_trb_t* l = &x->cmd_ring[XHCI_CMD_TRBS - 1];
        l->dw[0] = (uint32_t)&x->cmd_ring[0];
        l->dw[1] = 0;
        l->dw[2] = 0;
        BARRIER();
        l->dw[3] = TRB_TYPE(TRB_LINK) | TRB_TC | x->cmd_pcs;
        BARRIER();
        x->cmd_enq = 0;
        x->cmd_pcs ^= 1;
    }
    x->cmd_done = 0;
    x->db[0] = 0;                                     /* ring the command doorbell */

    for (int i = 0; i < 100000; i++) {                /* up to ~1 s */
        xhci_handle_events(x);
        if (x->cmd_done && x->cmd_trb == addr) break;
        usb_delay_us(10);
    }
    if (!(x->cmd_done && x->cmd_trb == addr)) {
        usb_log("xHCI: command "); usb_log_dec((ctrl >> 10) & 0x3F); usb_log(" timed out\n");
        return -1;
    }
    if (slot_out) *slot_out = x->cmd_slot;
    if (x->cmd_cc != CC_SUCCESS) {
        usb_log("xHCI: command "); usb_log_dec((ctrl >> 10) & 0x3F);
        usb_log(" failed cc="); usb_log_dec(x->cmd_cc); usb_log("\n");
        return -1;
    }
    return 0;
}

/* After a STALL / transaction error the endpoint is halted: Reset Endpoint,
 * then move its dequeue pointer to our enqueue pointer so we continue there. */
static void xhci_ep_recover(xhci_dev_t* xd, xhci_ring_t* r) {
    xhci_t* x = xd->x;
    uint32_t ep_slot = ((uint32_t)r->dci << 16) | ((uint32_t)xd->slot << 24);
    xhci_cmd(x, 0, 0, TRB_TYPE(TRB_RESET_EP) | ep_slot, NULL);
    xhci_cmd(x, (uint32_t)&r->trb[r->enq] | (r->pcs & 1), 0, TRB_TYPE(TRB_SET_TR_DEQ) | ep_slot, NULL);
    r->pending = 0;
    r->done = 0;
}

/* Wait for the TD in flight on `r`. Returns 0 ok (cc success/short), -1. */
static int xhci_wait_td(xhci_t* x, xhci_dev_t* xd, xhci_ring_t* r, int timeout_ms) {
    int iters = timeout_ms * 100;                    /* 10 us steps */
    while (iters-- > 0) {
        xhci_handle_events(x);
        if (r->done) break;
        usb_delay_us(10);
    }
    if (!r->done) {
        usb_log("xHCI: transfer timeout slot "); usb_log_dec(xd->slot);
        usb_log(" dci "); usb_log_dec(r->dci); usb_log("\n");
        /* Stop the endpoint and re-sync the dequeue pointer. */
        uint32_t ep_slot = ((uint32_t)r->dci << 16) | ((uint32_t)xd->slot << 24);
        xhci_cmd(x, 0, 0, TRB_TYPE(TRB_STOP_EP) | ep_slot, NULL);
        xhci_cmd(x, (uint32_t)&r->trb[r->enq] | (r->pcs & 1), 0, TRB_TYPE(TRB_SET_TR_DEQ) | ep_slot, NULL);
        r->pending = 0;
        return -1;
    }
    r->done = 0;
    if (r->cc != CC_SUCCESS && r->cc != CC_SHORT) {
        usb_log("xHCI: transfer error cc="); usb_log_dec(r->cc);
        usb_log(" slot "); usb_log_dec(xd->slot); usb_log(" dci "); usb_log_dec(r->dci); usb_log("\n");
        xhci_ep_recover(xd, r);
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Transfers
 * -------------------------------------------------------------------------- */
static int xhci_control(usb_hcd_t* hc, usb_device_t* dev, const uint8_t* setup,
                        void* data, int len, int dir_in) {
    xhci_t* x = (xhci_t*)hc->priv;
    xhci_dev_t* xd = (xhci_dev_t*)dev->hc_priv;
    if (!xd || !xd->in_use) return -1;
    xhci_ring_t* r = &xd->ep[0];

    r->done = 0;
    r->resid = 0;
    r->cc = 0;
    uint32_t s0 = (uint32_t)setup[0] | ((uint32_t)setup[1] << 8) | ((uint32_t)setup[2] << 16) | ((uint32_t)setup[3] << 24);
    uint32_t s1 = (uint32_t)setup[4] | ((uint32_t)setup[5] << 8) | ((uint32_t)setup[6] << 16) | ((uint32_t)setup[7] << 24);
    uint32_t trt = len ? (dir_in ? 3u : 2u) : 0u;
    ring_enq(r, s0, s1, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16));
    if (len) ring_queue_buf(r, TRB_DATA, (uint8_t*)data, len, dir_in, 0, dev->max_packet0);
    int status_in = (len == 0) || !dir_in;
    xhci_trb_t* st = ring_enq(r, 0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | (status_in ? TRB_DIR_IN : 0));
    r->want_trb = (uint32_t)st;
    r->pending = 1;
    BARRIER();
    x->db[xd->slot] = 1;                              /* EP0 doorbell */

    if (xhci_wait_td(x, xd, r, 1000) < 0) return -1;
    int got = len - (int)r->resid;
    if (got < 0) got = 0;
    return got;
}

static int xhci_ring_index(usb_device_t* dev, usb_endpoint_t* ep) {
    int i = (int)(ep - dev->endpoints);
    if (i < 0 || i >= USB_MAX_ENDPOINTS) return -1;
    return 1 + i;
}

static int xhci_bulk(usb_hcd_t* hc, usb_device_t* dev, usb_endpoint_t* ep,
                     void* buf, int len, int dir_in) {
    xhci_t* x = (xhci_t*)hc->priv;
    xhci_dev_t* xd = (xhci_dev_t*)dev->hc_priv;
    int ri = xhci_ring_index(dev, ep);
    if (!xd || !xd->in_use || ri < 0) return -1;
    xhci_ring_t* r = &xd->ep[ri];
    if (!r->dci) return -1;
    (void)dir_in;                                     /* direction is in the endpoint context */

    r->done = 0;
    r->resid = 0;
    r->cc = 0;
    xhci_trb_t* last;
    if (len == 0) last = ring_enq(r, 0, 0, 0, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    else last = ring_queue_buf(r, TRB_NORMAL, (uint8_t*)buf, len, dir_in, TRB_IOC, ep->max_packet_size);
    r->want_trb = (uint32_t)last;
    r->pending = 1;
    BARRIER();
    x->db[xd->slot] = r->dci;

    if (xhci_wait_td(x, xd, r, 3000) < 0) return -1;
    int got = len - (int)r->resid;
    if (got < 0) got = 0;
    return got;
}

static void xhci_intr_arm(xhci_t* x, xhci_dev_t* xd, xhci_ring_t* r) {
    r->resid = 0;
    r->done = 0;
    r->cc = 0;
    xhci_trb_t* t = ring_enq(r, (uint32_t)r->buf, 0, (uint32_t)r->buf_len,
                             TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    r->want_trb = (uint32_t)t;
    r->pending = 1;
    BARRIER();
    x->db[xd->slot] = r->dci;
}

static int xhci_interrupt_in(usb_hcd_t* hc, usb_device_t* dev, usb_endpoint_t* ep,
                             void* buf, int len) {
    xhci_t* x = (xhci_t*)hc->priv;
    xhci_dev_t* xd = (xhci_dev_t*)dev->hc_priv;
    int ri = xhci_ring_index(dev, ep);
    if (!xd || !xd->in_use || ri < 0) return -1;
    xhci_ring_t* r = &xd->ep[ri];
    if (!r->dci) return -1;

    xhci_handle_events(x);
    if (r->done) {
        r->done = 0;
        if (r->cc != CC_SUCCESS && r->cc != CC_SHORT) {
            xhci_ep_recover(xd, r);
            return -1;                                /* re-armed on the next poll */
        }
        int n = r->buf_len - (int)r->resid;
        if (n < 0) n = 0;
        if (n > len) n = len;
        if (n > 0) memcpy(buf, r->buf, n);
        xhci_intr_arm(x, xd, r);                      /* keep one TRB in flight */
        return n;
    }
    if (!r->pending) xhci_intr_arm(x, xd, r);
    return 0;
}

/* --------------------------------------------------------------------------
 * Device lifecycle: slot + contexts
 * -------------------------------------------------------------------------- */
static uint32_t xhci_speed_id(uint8_t speed) {
    switch (speed) {
        case USB_SPEED_LOW:   return 2;
        case USB_SPEED_FULL:  return 1;
        case USB_SPEED_HIGH:  return 3;
        default:              return 4;              /* SuperSpeed */
    }
}

/* TT fields for a low/full-speed device behind a high-speed hub:
 * dw2 = TT hub slot id | TT port number << 8 */
static uint32_t xhci_tt_fields(usb_device_t* dev) {
    if (dev->speed >= USB_SPEED_HIGH || dev->parent < 0) return 0;
    usb_device_t* devs = usb_get_devices();
    int p = dev->parent;
    uint32_t port = dev->port;
    for (int guard = 0; guard < USB_MAX_HUB_DEPTH + 1 && p >= 0 && p < USB_MAX_DEVICES; guard++) {
        usb_device_t* h = &devs[p];
        if (h->speed == USB_SPEED_HIGH && h->hcd == dev->hcd)
            return (uint32_t)h->slot_id | (port << 8);
        port = h->port;
        p = h->parent;
    }
    return 0;
}

static int xhci_enable_device(usb_hcd_t* hc, usb_device_t* dev) {
    xhci_t* x = (xhci_t*)hc->priv;

    xhci_dev_t* xd = NULL;
    for (int i = 0; i < USB_MAX_DEVICES; i++) if (!xdevs[i].in_use) { xd = &xdevs[i]; break; }
    if (!xd) { usb_log("xHCI: no free device structures\n"); return -1; }
    memset(xd, 0, sizeof(*xd));
    for (int i = 0; i < 32; i++) xd->ring_of_dci[i] = 0xFF;
    xd->x = x;

    uint32_t slot = 0;
    if (xhci_cmd(x, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT), &slot) < 0 || slot == 0 || slot > 255) {
        usb_log("xHCI: Enable Slot failed\n");
        return -1;
    }
    xd->slot = (uint8_t)slot;
    xd->in_use = true;
    x->slot_dev[slot] = xd;
    x->dcbaa[slot] = (uint32_t)xd->ctx_out;

    ring_init(&xd->ep[0]);
    xd->ep[0].dci = 1;
    xd->ring_of_dci[1] = 0;

    /* Input context: add slot + EP0. */
    volatile uint32_t* icc = xctx(x, xd->ctx_in, 0);
    icc[0] = 0;
    icc[1] = 0x3;
    volatile uint32_t* sc = xctx(x, xd->ctx_in, 1);
    sc[0] = (dev->route & 0xFFFFF) | (xhci_speed_id(dev->speed) << 20) | (1u << 27);
    sc[1] = ((uint32_t)dev->root_port << 16);
    sc[2] = xhci_tt_fields(dev);
    sc[3] = 0;
    volatile uint32_t* ec = xctx(x, xd->ctx_in, 2);   /* EP0 = DCI 1 */
    ec[0] = 0;
    ec[1] = (4u << 3) | (3u << 1) | ((uint32_t)dev->max_packet0 << 16);
    ec[2] = (uint32_t)xd->ep[0].trb | 1;             /* TR dequeue pointer, DCS=1 */
    ec[3] = 0;
    ec[4] = 8;                                       /* average TRB length */
    BARRIER();

    if (xhci_cmd(x, (uint32_t)xd->ctx_in, 0, TRB_TYPE(TRB_ADDRESS_DEV) | (slot << 24), NULL) < 0) {
        usb_log("xHCI: Address Device failed\n");
        xhci_cmd(x, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | (slot << 24), NULL);
        x->dcbaa[slot] = 0;
        x->slot_dev[slot] = NULL;
        xd->in_use = false;
        return -1;
    }

    volatile uint32_t* osc = xctx(x, xd->ctx_out, 0);
    uint32_t addr = osc[3] & 0xFF;
    dev->slot_id = (uint8_t)slot;
    dev->address = addr ? (uint8_t)addr : (uint8_t)slot;
    dev->hc_priv = xd;
    usb_log("xHCI: slot "); usb_log_dec(slot); usb_log(" addressed (route ");
    usb_log_hex(dev->route); usb_log(", root port "); usb_log_dec(dev->root_port); usb_log(")\n");
    return 0;
}

static int xhci_update_mps0(usb_hcd_t* hc, usb_device_t* dev) {
    xhci_t* x = (xhci_t*)hc->priv;
    xhci_dev_t* xd = (xhci_dev_t*)dev->hc_priv;
    if (!xd) return -1;
    volatile uint32_t* ec = xctx(x, xd->ctx_in, 2);
    if (((ec[1] >> 16) & 0xFFFF) == dev->max_packet0) return 0;    /* unchanged */
    volatile uint32_t* icc = xctx(x, xd->ctx_in, 0);
    icc[0] = 0;
    icc[1] = 0x2;                                    /* EP0 only */
    ec[1] = (ec[1] & 0xFFFF) | ((uint32_t)dev->max_packet0 << 16);
    BARRIER();
    return xhci_cmd(x, (uint32_t)xd->ctx_in, 0, TRB_TYPE(TRB_EVAL_CTX) | ((uint32_t)xd->slot << 24), NULL);
}

static int xhci_configure(usb_hcd_t* hc, usb_device_t* dev) {
    xhci_t* x = (xhci_t*)hc->priv;
    xhci_dev_t* xd = (xhci_dev_t*)dev->hc_priv;
    if (!xd) return -1;

    volatile uint32_t* icc = xctx(x, xd->ctx_in, 0);
    uint32_t add = 1;                                /* slot context */
    uint32_t max_dci = 1;

    for (int i = 0; i < dev->num_endpoints && i < USB_MAX_ENDPOINTS; i++) {
        usb_endpoint_t* ep = &dev->endpoints[i];
        uint32_t epnum = ep->address & 0x0F;
        int in = (ep->address & USB_EP_DIR_IN) != 0;
        uint32_t dci = epnum * 2 + (in ? 1 : 0);
        if (dci < 2 || dci > 31) continue;
        uint32_t xt = ep->attributes & USB_EP_XFER_MASK;
        uint32_t type;
        switch (xt) {
            case USB_EP_XFER_BULK: type = in ? 6 : 2; break;
            case USB_EP_XFER_INT:  type = in ? 7 : 3; break;
            case USB_EP_XFER_ISOC: type = in ? 5 : 1; break;
            default:               type = 4; break;
        }
        uint32_t mps = ep->max_packet_size & 0x7FF;
        if (mps == 0) mps = 8;
        uint32_t ival = 0;
        if (xt == USB_EP_XFER_INT || xt == USB_EP_XFER_ISOC) {
            if (dev->speed >= USB_SPEED_HIGH) {
                ival = ep->interval ? (uint32_t)ep->interval - 1 : 0;
                if (ival > 15) ival = 15;
            } else {
                /* bInterval in ms -> 2^ival * 125 us */
                uint32_t uf = (ep->interval ? ep->interval : 1u) * 8u;
                ival = 3;
                while (ival < 10 && (1u << (ival + 1)) <= uf) ival++;
            }
        }

        xhci_ring_t* r = &xd->ep[1 + i];
        ring_init(r);
        r->dci = (uint8_t)dci;
        r->buf_len = (mps > 64) ? 64 : (int)mps;
        xd->ring_of_dci[dci] = (uint8_t)(1 + i);
        ep->hc_priv = r;

        volatile uint32_t* ec = xctx(x, xd->ctx_in, 1 + dci);
        ec[0] = ival << 16;
        ec[1] = (type << 3) | ((xt == USB_EP_XFER_ISOC) ? 0 : (3u << 1)) | (mps << 16);
        ec[2] = (uint32_t)r->trb | 1;
        ec[3] = 0;
        ec[4] = mps | ((xt == USB_EP_XFER_INT) ? (mps << 16) : 0);
        add |= 1u << dci;
        if (dci > max_dci) max_dci = dci;
    }

    icc[0] = 0;
    icc[1] = add;
    volatile uint32_t* sc = xctx(x, xd->ctx_in, 1);
    sc[0] = (sc[0] & 0x07FFFFFFu) | (max_dci << 27);
    if (dev->is_hub) {
        sc[0] |= (1u << 26);                         /* Hub */
        if (dev->protocol == 2) sc[0] |= (1u << 25); /* multi-TT */
        sc[1] = (sc[1] & 0x00FFFFFFu) | ((uint32_t)dev->hub_nports << 24);
        sc[2] = (sc[2] & ~(3u << 16)) | (((uint32_t)(dev->hub_char >> 5) & 3u) << 16);  /* TT think time */
    }
    BARRIER();
    return xhci_cmd(x, (uint32_t)xd->ctx_in, 0, TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t)xd->slot << 24), NULL);
}

static void xhci_release_device(usb_hcd_t* hc, usb_device_t* dev) {
    xhci_t* x = (xhci_t*)hc->priv;
    xhci_dev_t* xd = (xhci_dev_t*)dev->hc_priv;
    if (!xd) return;
    if (xd->in_use && xd->slot) {
        xhci_cmd(x, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | ((uint32_t)xd->slot << 24), NULL);
        x->dcbaa[xd->slot] = 0;
        x->slot_dev[xd->slot] = NULL;
    }
    xd->in_use = false;
    xd->slot = 0;
    dev->hc_priv = NULL;
    for (int i = 0; i < USB_MAX_ENDPOINTS; i++) dev->endpoints[i].hc_priv = NULL;
}

/* --------------------------------------------------------------------------
 * Root hub ports
 * -------------------------------------------------------------------------- */
static int xhci_port_status(usb_hcd_t* hc, int port, int* changed) {
    xhci_t* x = (xhci_t*)hc->priv;
    uint32_t v = x->op[OP_PORTSC(port)];
    *changed = (v & PSC_CSC) ? 1 : 0;
    if (v & PSC_CHANGE) x->op[OP_PORTSC(port)] = (v & PSC_PRESERVE) | (v & PSC_CHANGE);
    xhci_handle_events(x);                           /* drain port-change events */
    return (v & PSC_CCS) ? 1 : 0;
}

static int xhci_port_reset(usb_hcd_t* hc, int port) {
    xhci_t* x = (xhci_t*)hc->priv;
    uint32_t v = x->op[OP_PORTSC(port)];
    if (!(v & PSC_CCS)) return -1;

    if (!(v & PSC_PED)) {
        /* USB2 ports (and a USB3 port whose link is not up) need a reset. */
        x->op[OP_PORTSC(port)] = (v & PSC_PRESERVE) | PSC_PR;
        int ok = 0;
        for (int i = 0; i < 500; i++) {
            usb_delay_ms(1);
            v = x->op[OP_PORTSC(port)];
            if (v & PSC_PRC) { ok = 1; break; }
            if (!(v & PSC_CCS)) break;
        }
        v = x->op[OP_PORTSC(port)];
        x->op[OP_PORTSC(port)] = (v & PSC_PRESERVE) | (v & PSC_CHANGE);
        if (!ok || !(v & PSC_PED)) {
            usb_log("xHCI: port "); usb_log_dec(port); usb_log(" reset did not enable\n");
            return -1;
        }
    }
    int speed;
    switch (PSC_SPEED(v)) {
        case 1:  speed = USB_SPEED_FULL;  break;
        case 2:  speed = USB_SPEED_LOW;   break;
        case 3:  speed = USB_SPEED_HIGH;  break;
        case 4:
        case 5:  speed = USB_SPEED_SUPER; break;
        default: return -1;
    }
    usb_delay_ms(10);                                /* reset recovery */
    return speed;
}

/* --------------------------------------------------------------------------
 * Controller bring-up
 * -------------------------------------------------------------------------- */
static bool xhci_setup(xhci_t* x, pci_device_t* pci) {
    uint32_t bar0 = pci->bar[0];
    if (bar0 & 1) return false;                      /* must be memory space */
    if (((bar0 >> 1) & 3) == 2 && pci->bar[1] != 0) {
        usb_log("xHCI: 64-bit BAR above 4 GB, unsupported\n");
        return false;
    }
    x->base = bar0 & 0xFFFFFFF0u;
    if (!x->base) return false;

    uint32_t cmd = pci_read32(pci->bus, pci->device, pci->function, PCI_COMMAND);
    pci_write32(pci->bus, pci->device, pci->function, PCI_COMMAND, cmd | 0x6);   /* mem + bus master */
    paging_map_mmio(x->base, 0x10000);

    x->cap = (volatile uint32_t*)x->base;
    uint32_t caplen = x->cap[0] & 0xFF;
    uint32_t hcs1 = x->cap[1];
    uint32_t hcs2 = x->cap[2];
    uint32_t hcc1 = x->cap[4];
    uint32_t dboff = x->cap[5] & ~3u;
    uint32_t rtsoff = x->cap[6] & ~0x1Fu;
    x->op = (volatile uint32_t*)(x->base + caplen);
    x->rt = (volatile uint32_t*)(x->base + rtsoff);
    x->db = (volatile uint32_t*)(x->base + dboff);
    x->csz = (hcc1 & (1u << 2)) ? 64 : 32;
    x->max_slots = hcs1 & 0xFF;
    if (x->max_slots > 255) x->max_slots = 255;
    x->nports = (hcs1 >> 24) & 0xFF;
    if (x->nports > XHCI_MAX_PORTS) x->nports = XHCI_MAX_PORTS;

    /* Extended capabilities: BIOS/OS handoff + supported-protocol port map. */
    uint32_t xecp = (hcc1 >> 16) & 0xFFFF;
    if (xecp) {
        uint32_t p = x->base + xecp * 4;
        for (int guard = 0; guard < 64; guard++) {
            volatile uint32_t* c = (volatile uint32_t*)p;
            uint32_t v = c[0];
            uint32_t id = v & 0xFF;
            uint32_t next = (v >> 8) & 0xFF;
            if (id == 1) {                           /* USB Legacy Support */
                c[0] = v | (1u << 24);               /* HC OS Owned */
                for (int i = 0; i < 1000 && (c[0] & (1u << 16)); i++) usb_delay_ms(1);
                if (c[0] & (1u << 16)) usb_log("xHCI: BIOS did not release ownership\n");
                c[1] = c[1] & ~0xE011u;              /* disable SMI sources */
            } else if (id == 2) {                    /* Supported Protocol */
                uint32_t major = v >> 24;
                uint32_t off = c[2] & 0xFF;
                uint32_t cnt = (c[2] >> 8) & 0xFF;
                for (uint32_t i = 0; i < cnt; i++) {
                    uint32_t pn = off + i;
                    if (pn >= 1 && pn <= XHCI_MAX_PORTS) x->port_usb3[pn] = (major == 3);
                }
            }
            if (!next) break;
            p += next * 4;
        }
    }

    /* Stop, then reset the controller. */
    if (x->op[OP_USBCMD] & CMD_RS) {
        x->op[OP_USBCMD] &= ~CMD_RS;
        for (int i = 0; i < 100 && !(x->op[OP_USBSTS] & STS_HCH); i++) usb_delay_ms(1);
    }
    x->op[OP_USBCMD] = CMD_HCRST;
    {
        int i;
        for (i = 0; i < 1000; i++) {
            usb_delay_ms(1);
            if (!(x->op[OP_USBCMD] & CMD_HCRST) && !(x->op[OP_USBSTS] & STS_CNR)) break;
        }
        if (i >= 1000) { usb_log("xHCI: reset timeout\n"); return false; }
    }

    /* Device slots + DCBAA. */
    x->op[OP_CONFIG] = (uint32_t)x->max_slots;
    memset((void*)x->dcbaa, 0, sizeof(x->dcbaa));
    for (int i = 0; i < 256; i++) x->slot_dev[i] = NULL;

    /* Scratchpad buffers if the HC asks for them. */
    uint32_t nsp = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);
    if (nsp) {
        uint32_t avail = XHCI_SCRATCH_MAX - scratch_used;
        uint32_t give = nsp > avail ? avail : nsp;
        for (uint32_t i = 0; i < give; i++) x->scratch_arr[i] = (uint32_t)scratch_pages[scratch_used + i];
        scratch_used += (int)give;
        x->dcbaa[0] = (uint32_t)x->scratch_arr;
        if (give < nsp) { usb_log("xHCI: wants "); usb_log_dec(nsp); usb_log(" scratchpads, only "); usb_log_dec(give); usb_log(" available\n"); }
    }
    x->op[OP_DCBAAP_LO] = (uint32_t)x->dcbaa;
    x->op[OP_DCBAAP_HI] = 0;

    /* Command ring. */
    memset((void*)x->cmd_ring, 0, sizeof(x->cmd_ring));
    x->cmd_enq = 0;
    x->cmd_pcs = 1;
    {
        xhci_trb_t* l = &x->cmd_ring[XHCI_CMD_TRBS - 1];
        l->dw[0] = (uint32_t)&x->cmd_ring[0];
        l->dw[3] = TRB_TYPE(TRB_LINK) | TRB_TC;
    }
    x->op[OP_CRCR_LO] = (uint32_t)x->cmd_ring | 1;  /* RCS = 1 */
    x->op[OP_CRCR_HI] = 0;

    /* Event ring: one segment. */
    memset((void*)x->evt_ring, 0, sizeof(x->evt_ring));
    x->evt_deq = 0;
    x->evt_ccs = 1;
    x->erst[0] = (uint32_t)x->evt_ring;
    x->erst[1] = XHCI_EVT_TRBS;
    x->rt[RT_ERSTSZ] = 1;
    x->rt[RT_ERDP_HI] = 0;
    x->rt[RT_ERDP_LO] = (uint32_t)x->evt_ring;
    x->rt[RT_ERSTBA_LO] = (uint32_t)x->erst;
    x->rt[RT_ERSTBA_HI] = 0;
    x->rt[RT_IMAN] = 0;                              /* polled: no interrupt enable */

    /* Run. */
    x->op[OP_USBCMD] |= CMD_RS;
    for (int i = 0; i < 100 && (x->op[OP_USBSTS] & STS_HCH); i++) usb_delay_ms(1);
    usb_delay_ms(20);                                /* let the ports settle */

    usb_log("xHCI: mmio "); usb_log_hex(x->base); usb_log(" ports="); usb_log_dec(x->nports);
    usb_log(" slots="); usb_log_dec(x->max_slots); usb_log(" csz="); usb_log_dec(x->csz);
    usb_log(" scratch="); usb_log_dec(nsp); usb_log("\n");
    return true;
}

void usb_xhci_init(void) {
    pci_device_t* devs = pci_get_devices();
    int n = pci_device_count();
    for (int i = 0; i < n && xhci_count < XHCI_MAX; i++) {
        pci_device_t* p = &devs[i];
        if (!p->active || p->class_code != PCI_CLASS_SERIAL ||
            p->subclass != PCI_SUBCLASS_USB || p->prog_if != PCI_PROGIF_XHCI)
            continue;
        usb_hcd_t* hc = usb_hcd_alloc();
        if (!hc) break;
        xhci_t* x = &xhcis[xhci_count];
        memset(x, 0, sizeof(*x));
        if (!xhci_setup(x, p)) continue;
        x->hc = hc;
        hc->name = "xHCI";
        hc->type = USB_HCD_XHCI;
        hc->nports = (uint8_t)x->nports;
        hc->base = x->base;
        hc->pci = p;
        hc->priv = x;
        hc->port_status = xhci_port_status;
        hc->port_reset = xhci_port_reset;
        hc->control = xhci_control;
        hc->interrupt_in = xhci_interrupt_in;
        hc->bulk = xhci_bulk;
        hc->enable_device = xhci_enable_device;
        hc->update_mps0 = xhci_update_mps0;
        hc->configure = xhci_configure;
        hc->release_device = xhci_release_device;
        xhci_count++;
        usb_hcd_register(hc);
    }
}

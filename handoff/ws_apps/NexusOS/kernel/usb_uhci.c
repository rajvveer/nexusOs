/* ============================================================================
 * NexusOS — USB UHCI Host Controller Driver — Phase 54 / Phase 55
 * ============================================================================
 * Polled UHCI (USB 1.1) driver: PIIX3/PIIX4 and ICH companion controllers
 * (PCI class 0C/03/00, I/O BAR4). Frame list -> 8 interrupt QHs -> control QH.
 * Control and bulk transfers run synchronously on the control QH; interrupt
 * IN endpoints get their own QH with one TD that stays armed between polls,
 * so polling an idle endpoint costs nothing (no 4 ms spin per poll any more).
 * Several controllers can coexist (the ICH9 layout has up to 6 UHCIs).
 * Phase 55: moved out of usb.c and registered as a usb_hcd_t.
 * ============================================================================ */

#include "usb.h"
#include "pci.h"
#include "port.h"
#include "string.h"

#define UHCI_MAX        4
#define UHCI_INTR_SLOTS 8
#define TD_POOL         32

/* UHCI I/O register offsets */
#define UHCI_CMD        0x00
#define UHCI_STS        0x02
#define UHCI_INTR       0x04
#define UHCI_FRNUM      0x06
#define UHCI_FRBASEADD  0x08
#define UHCI_SOFMOD     0x0C
#define UHCI_PORTSC1    0x10

/* UHCI command bits */
#define UHCI_CMD_RUN     0x01
#define UHCI_CMD_HCRESET 0x02
#define UHCI_CMD_GRESET  0x04
#define UHCI_CMD_MAXP    0x80

/* UHCI port status bits */
#define UHCI_PORT_CONNECTED    0x0001
#define UHCI_PORT_CONNECT_CHG  0x0002
#define UHCI_PORT_ENABLED      0x0004
#define UHCI_PORT_ENABLE_CHG   0x0008
#define UHCI_PORT_LOW_SPEED    0x0100
#define UHCI_PORT_RESET        0x0200
#define UHCI_PORT_SUSPEND      0x1000

/* Transfer-descriptor control/status bits (dword 1) */
#define TD_CTRL_ACTIVE   (1u << 23)
#define TD_CTRL_STALLED  (1u << 22)
#define TD_CTRL_DBUF     (1u << 21)
#define TD_CTRL_BABBLE   (1u << 20)
#define TD_CTRL_NAK      (1u << 19)
#define TD_CTRL_CRCTO    (1u << 18)
#define TD_CTRL_BITSTUFF (1u << 17)
#define TD_CTRL_IOC      (1u << 24)
#define TD_CTRL_LS       (1u << 26)
#define TD_CTRL_CERR3    (3u << 27)
#define TD_CTRL_SPD      (1u << 29)
#define TD_CTRL_ERRMASK  (TD_CTRL_STALLED | TD_CTRL_DBUF | TD_CTRL_BABBLE | TD_CTRL_CRCTO | TD_CTRL_BITSTUFF)
#define TD_ACTLEN_MASK   0x7FF

/* Token PIDs */
#define PID_SETUP  0x2D
#define PID_IN     0x69
#define PID_OUT    0xE1

/* Link pointer bits */
#define LP_TERMINATE  0x1
#define LP_QH         0x2
#define LP_DEPTH      0x4

#define BARRIER() __asm__ volatile("" ::: "memory")

typedef struct __attribute__((packed, aligned(16))) {
    volatile uint32_t link;
    volatile uint32_t ctrl;
    volatile uint32_t token;
    volatile uint32_t buffer;
} uhci_td_t;

typedef struct __attribute__((packed, aligned(16))) {
    volatile uint32_t head;     /* horizontal link (next QH) */
    volatile uint32_t element;  /* vertical link (first TD) */
    uint32_t pad[2];
} uhci_qh_t;

/* One interrupt-IN endpoint: its own QH in the frame list + one armed TD. */
typedef struct {
    bool            in_use;
    bool            armed;
    usb_device_t*   dev;
    usb_endpoint_t* ep;
    uint32_t        gen;
    int             len;
    uhci_qh_t       qh;
    uhci_td_t       td;
    uint8_t         buf[64] __attribute__((aligned(16)));
} uhci_intr_t;

typedef struct {
    usb_hcd_t*  hc;
    uint16_t    io;
    uint32_t    frame_list[1024] __attribute__((aligned(4096)));
    uhci_qh_t   ctrl_qh;
    uhci_td_t   td[TD_POOL];
    uint8_t     setup[8] __attribute__((aligned(16)));
    uhci_intr_t intr[UHCI_INTR_SLOTS];
} uhci_t;

/* .bss (zeroed at boot, identity-mapped, below 32 MB => DMA-able) */
static uhci_t uhcis[UHCI_MAX];
static int    uhci_count = 0;

/* --------------------------------------------------------------------------
 * TD helpers
 * -------------------------------------------------------------------------- */
static void td_fill(uhci_td_t* td, uint32_t next_phys, int is_last,
                    uint8_t pid, uint8_t addr, uint8_t ep, int toggle,
                    int ls, uint32_t buf, int len) {
    td->link = is_last ? LP_TERMINATE : (next_phys | LP_DEPTH);   /* depth-first */
    uint32_t maxlen = (len == 0) ? 0x7FF : (uint32_t)(len - 1);
    td->token = (uint32_t)pid
              | ((uint32_t)addr << 8)
              | ((uint32_t)(ep & 0x0F) << 15)
              | ((uint32_t)(toggle & 1) << 19)
              | (maxlen << 21);
    td->buffer = buf;
    BARRIER();
    uint32_t ctrl = TD_CTRL_ACTIVE | TD_CTRL_CERR3;
    if (ls) ctrl |= TD_CTRL_LS;
    td->ctrl = ctrl;
}

static int td_actlen(uhci_td_t* td) {
    uint32_t a = td->ctrl & TD_ACTLEN_MASK;
    return (a == 0x7FF) ? 0 : (int)(a + 1);
}

/* Run a TD chain on the control QH; poll until it retires. 0 ok, -1 error. */
static int uhci_run(uhci_t* u, uhci_td_t* first, int ntd, int timeout_ms) {
    BARRIER();
    u->ctrl_qh.element = (uint32_t)first;

    port_word_out(u->io + UHCI_STS, 0x3F);
    uint16_t cmd = port_word_in(u->io + UHCI_CMD);
    if (!(cmd & UHCI_CMD_RUN)) port_word_out(u->io + UHCI_CMD, cmd | UHCI_CMD_RUN);

    uhci_td_t* last = &first[ntd - 1];
    int iters = timeout_ms * 50;                 /* 20 us steps */
    int rc = 0;
    while (1) {
        if (!(last->ctrl & TD_CTRL_ACTIVE)) break;
        int err = 0;
        for (int i = 0; i < ntd; i++)
            if (first[i].ctrl & TD_CTRL_ERRMASK) { err = 1; break; }
        if (err) break;
        if (--iters <= 0) { rc = -1; break; }
        usb_delay_us(20);
    }

    u->ctrl_qh.element = LP_TERMINATE;
    BARRIER();

    if (rc == 0) {
        for (int i = 0; i < ntd; i++) {
            uint32_t c = first[i].ctrl;
            if (c & TD_CTRL_ERRMASK) { rc = -1; break; }
            if (c & TD_CTRL_ACTIVE)  { rc = -1; break; }
        }
    }
    return rc;
}

/* --------------------------------------------------------------------------
 * Control transfer
 * -------------------------------------------------------------------------- */
static int uhci_control(usb_hcd_t* hc, usb_device_t* dev, const uint8_t* setup,
                        void* data, int len, int dir_in) {
    uhci_t* u = (uhci_t*)hc->priv;
    int ls = (dev->speed == USB_SPEED_LOW);
    int mps = dev->max_packet0 ? dev->max_packet0 : 8;
    if (mps > 64) mps = 64;

    memcpy(u->setup, setup, 8);

    int ntd = 0, toggle = 0;
    uhci_td_t* td = &u->td[ntd];
    td_fill(td, (uint32_t)&u->td[ntd + 1], 0, PID_SETUP, dev->address, 0, 0, ls,
            (uint32_t)u->setup, 8);
    ntd++;

    uint8_t* p = (uint8_t*)data;
    int remaining = len;
    uint8_t data_pid = dir_in ? PID_IN : PID_OUT;
    while (remaining > 0 && ntd < TD_POOL - 1) {
        int chunk = remaining > mps ? mps : remaining;
        toggle ^= 1;
        td = &u->td[ntd];
        td_fill(td, (uint32_t)&u->td[ntd + 1], 0, data_pid, dev->address, 0, toggle, ls,
                (uint32_t)p, chunk);
        if (dir_in) td->ctrl |= TD_CTRL_SPD;    /* short IN packet is fine */
        ntd++;
        p += chunk;
        remaining -= chunk;
    }

    /* STATUS stage: IN unless the data stage was IN. */
    int status_in = (len == 0) || !dir_in;
    td = &u->td[ntd];
    td_fill(td, 0, 1, status_in ? PID_IN : PID_OUT, dev->address, 0, 1, ls, 0, 0);
    ntd++;

    int rc = uhci_run(u, u->td, ntd, 500);
    /* A short IN data stage leaves the following TDs un-executed (SPD); count
     * what arrived and accept the result if nothing errored. */
    int got = 0;
    int err = 0;
    for (int i = 0; i < ntd; i++) if (u->td[i].ctrl & TD_CTRL_ERRMASK) err = 1;
    if (err) return -1;
    for (int i = 1; i < ntd - 1; i++) got += td_actlen(&u->td[i]);
    if (rc != 0) {
        /* Timed out: tolerate only the short-packet case (status TD inactive
         * is impossible then, so require the setup TD done + a short data TD). */
        if (u->td[0].ctrl & TD_CTRL_ACTIVE) return -1;
        int shortpkt = 0;
        for (int i = 1; i < ntd - 1; i++)
            if (!(u->td[i].ctrl & TD_CTRL_ACTIVE) && td_actlen(&u->td[i]) < mps) shortpkt = 1;
        if (!shortpkt) return -1;
    }
    return got;
}

/* --------------------------------------------------------------------------
 * Bulk transfer (synchronous, on the control QH)
 * -------------------------------------------------------------------------- */
static int uhci_bulk(usb_hcd_t* hc, usb_device_t* dev, usb_endpoint_t* ep,
                     void* buf, int len, int dir_in) {
    uhci_t* u = (uhci_t*)hc->priv;
    int ls = (dev->speed == USB_SPEED_LOW);
    int mps = ep->max_packet_size ? ep->max_packet_size : 64;
    if (mps > 64) mps = 64;
    uint8_t pid = dir_in ? PID_IN : PID_OUT;
    uint8_t* p = (uint8_t*)buf;
    int remaining = len;
    int total = 0;

    do {
        int ntd = 0;
        while (remaining > 0 && ntd < TD_POOL) {
            int chunk = remaining > mps ? mps : remaining;
            uhci_td_t* td = &u->td[ntd];
            int is_last = (remaining - chunk == 0) || (ntd == TD_POOL - 1);
            td_fill(td, (uint32_t)&u->td[ntd + 1], is_last, pid, dev->address,
                    ep->address & 0x0F, ep->toggle, ls, (uint32_t)p, chunk);
            if (dir_in) td->ctrl |= TD_CTRL_SPD;
            ep->toggle ^= 1;
            ntd++;
            p += chunk;
            remaining -= chunk;
            if (is_last) break;
        }
        if (ntd == 0 && len == 0) {           /* zero-length packet */
            td_fill(&u->td[0], 0, 1, pid, dev->address, ep->address & 0x0F,
                    ep->toggle, ls, 0, 0);
            ep->toggle ^= 1;
            ntd = 1;
        }
        if (ntd == 0) break;
        int rc = uhci_run(u, u->td, ntd, 2000);
        int err = 0, done = 0, shortpkt = 0;
        for (int i = 0; i < ntd; i++) {
            uint32_t c = u->td[i].ctrl;
            if (c & TD_CTRL_ERRMASK) { err = 1; break; }
            if (c & TD_CTRL_ACTIVE) break;     /* not executed (after a short packet) */
            int n = td_actlen(&u->td[i]);
            total += n;
            done++;
            if (dir_in && n < mps) { shortpkt = 1; break; }
        }
        if (err) return -1;
        if (shortpkt) {
            /* TDs after the short one did not run: fix up the toggle. */
            ep->toggle ^= (uint8_t)((ntd - done) & 1);
            return total;
        }
        if (rc != 0) return -1;
        if (len == 0) break;
    } while (remaining > 0);
    return total;
}

/* --------------------------------------------------------------------------
 * Interrupt IN: one QH per endpoint in the frame list with an armed TD.
 * -------------------------------------------------------------------------- */
static uhci_intr_t* uhci_intr_slot(uhci_t* u, usb_device_t* dev, usb_endpoint_t* ep) {
    uhci_intr_t* s = (uhci_intr_t*)ep->hc_priv;
    if (s && s >= &u->intr[0] && s < &u->intr[UHCI_INTR_SLOTS] &&
        s->in_use && s->dev == dev && s->gen == dev->gen && s->ep == ep)
        return s;
    for (int i = 0; i < UHCI_INTR_SLOTS; i++) {
        s = &u->intr[i];
        if (s->in_use) continue;
        s->in_use = true;
        s->armed = false;
        s->dev = dev;
        s->ep = ep;
        s->gen = dev->gen;
        s->qh.element = LP_TERMINATE;
        ep->hc_priv = s;
        return s;
    }
    return NULL;
}

static void uhci_intr_arm(uhci_intr_t* s) {
    usb_endpoint_t* ep = s->ep;
    usb_device_t* dev = s->dev;
    int len = ep->max_packet_size;
    if (len <= 0) len = 8;
    if (len > 64) len = 64;
    s->len = len;
    td_fill(&s->td, 0, 1, PID_IN, dev->address, ep->address & 0x0F, ep->toggle,
            dev->speed == USB_SPEED_LOW, (uint32_t)s->buf, len);
    s->td.ctrl |= TD_CTRL_SPD;
    BARRIER();
    s->qh.element = (uint32_t)&s->td;
    s->armed = true;
}

static int uhci_interrupt_in(usb_hcd_t* hc, usb_device_t* dev, usb_endpoint_t* ep,
                             void* buf, int len) {
    uhci_t* u = (uhci_t*)hc->priv;
    uhci_intr_t* s = uhci_intr_slot(u, dev, ep);
    if (!s) return -1;

    if (s->armed) {
        uint32_t c = s->td.ctrl;
        if (c & TD_CTRL_ACTIVE) return 0;             /* still waiting (NAKs) */
        s->armed = false;
        s->qh.element = LP_TERMINATE;
        BARRIER();
        if (c & TD_CTRL_ERRMASK) return -1;          /* re-armed on next poll */
        int n = td_actlen(&s->td);
        ep->toggle ^= 1;
        if (n > len) n = len;
        if (n > 0) memcpy(buf, s->buf, n);
        uhci_intr_arm(s);                             /* keep one in flight */
        return n;
    }
    uhci_intr_arm(s);
    return 0;
}

static void uhci_release_device(usb_hcd_t* hc, usb_device_t* dev) {
    uhci_t* u = (uhci_t*)hc->priv;
    for (int i = 0; i < UHCI_INTR_SLOTS; i++) {
        uhci_intr_t* s = &u->intr[i];
        if (s->in_use && s->dev == dev) {
            s->qh.element = LP_TERMINATE;
            BARRIER();
            s->in_use = false;
            s->armed = false;
            s->dev = NULL;
            s->ep = NULL;
        }
    }
}

/* --------------------------------------------------------------------------
 * Root hub ports
 * -------------------------------------------------------------------------- */
static int uhci_port_status(usb_hcd_t* hc, int port, int* changed) {
    uhci_t* u = (uhci_t*)hc->priv;
    uint16_t base = u->io + UHCI_PORTSC1 + 2 * (port - 1);
    uint16_t sts = port_word_in(base);
    *changed = (sts & UHCI_PORT_CONNECT_CHG) ? 1 : 0;
    if (sts & (UHCI_PORT_CONNECT_CHG | UHCI_PORT_ENABLE_CHG)) {
        /* Write-1-to-clear the change bits, keep enable/suspend as they are. */
        port_word_out(base, sts & (UHCI_PORT_CONNECT_CHG | UHCI_PORT_ENABLE_CHG |
                                   UHCI_PORT_ENABLED | UHCI_PORT_SUSPEND));
    }
    return (sts & UHCI_PORT_CONNECTED) ? 1 : 0;
}

static int uhci_port_reset(usb_hcd_t* hc, int port) {
    uhci_t* u = (uhci_t*)hc->priv;
    uint16_t base = u->io + UHCI_PORTSC1 + 2 * (port - 1);
    uint16_t sts = port_word_in(base);
    if (!(sts & UHCI_PORT_CONNECTED)) return -1;

    int low_speed = (sts & UHCI_PORT_LOW_SPEED) ? 1 : 0;

    /* Drive reset for 50 ms. */
    port_word_out(base, (sts & 0x13F5) | UHCI_PORT_RESET);
    usb_delay_ms(50);
    port_word_out(base, port_word_in(base) & ~UHCI_PORT_RESET);
    usb_delay_ms(2);

    /* Enable the port, clearing change bits (write-1-to-clear). */
    for (int tries = 0; tries < 10; tries++) {
        usb_delay_ms(5);
        sts = port_word_in(base);
        if (!(sts & UHCI_PORT_CONNECTED)) return -1;
        if (sts & UHCI_PORT_ENABLED) break;
        port_word_out(base, UHCI_PORT_ENABLED | UHCI_PORT_CONNECT_CHG | UHCI_PORT_ENABLE_CHG);
    }
    sts = port_word_in(base);
    if (!(sts & UHCI_PORT_ENABLED)) return -1;
    port_word_out(base, sts & (UHCI_PORT_ENABLED | UHCI_PORT_CONNECT_CHG | UHCI_PORT_ENABLE_CHG));
    usb_delay_ms(10);                            /* post-reset recovery */
    return low_speed ? USB_SPEED_LOW : USB_SPEED_FULL;
}

/* --------------------------------------------------------------------------
 * Controller bring-up
 * -------------------------------------------------------------------------- */
static bool uhci_setup(uhci_t* u, pci_device_t* pci) {
    uint32_t bar4 = pci->bar[4];
    if (!(bar4 & 0x01)) return false;             /* must be I/O space */
    u->io = (uint16_t)(bar4 & 0xFFFC);

    /* Enable I/O space + bus mastering. */
    uint32_t cmd = pci_read32(pci->bus, pci->device, pci->function, PCI_COMMAND);
    pci_write32(pci->bus, pci->device, pci->function, PCI_COMMAND, cmd | 0x5);

    /* Disable BIOS legacy / SMI trapping (USB_LEGKEY @ PCI cfg 0xC0). */
    pci_write32(pci->bus, pci->device, pci->function, 0xC0, 0x8F00);

    /* Schedule: every frame -> intr QH chain -> control QH -> end. */
    u->ctrl_qh.head = LP_TERMINATE;
    u->ctrl_qh.element = LP_TERMINATE;
    for (int i = 0; i < UHCI_INTR_SLOTS; i++) {
        uhci_intr_t* s = &u->intr[i];
        s->in_use = false;
        s->armed = false;
        s->qh.element = LP_TERMINATE;
        s->qh.head = (i + 1 < UHCI_INTR_SLOTS) ? ((uint32_t)&u->intr[i + 1].qh | LP_QH)
                                               : ((uint32_t)&u->ctrl_qh | LP_QH);
    }
    uint32_t first = (uint32_t)&u->intr[0].qh | LP_QH;
    for (int i = 0; i < 1024; i++) u->frame_list[i] = first;
    BARRIER();

    /* Global reset, then host-controller reset. */
    port_word_out(u->io + UHCI_CMD, UHCI_CMD_GRESET);
    usb_delay_ms(15);
    port_word_out(u->io + UHCI_CMD, 0);
    port_word_out(u->io + UHCI_CMD, UHCI_CMD_HCRESET);
    int timeout = 100;
    while ((port_word_in(u->io + UHCI_CMD) & UHCI_CMD_HCRESET) && --timeout > 0)
        usb_delay_ms(1);

    port_word_out(u->io + UHCI_INTR, 0);          /* polled: no interrupts */
    port_word_out(u->io + UHCI_FRNUM, 0);
    port_dword_out(u->io + UHCI_FRBASEADD, (uint32_t)u->frame_list);
    port_word_out(u->io + UHCI_SOFMOD, 0x40);
    port_word_out(u->io + UHCI_STS, 0x3F);
    port_word_out(u->io + UHCI_CMD, UHCI_CMD_RUN | UHCI_CMD_MAXP);
    usb_delay_ms(2);
    return true;
}

void usb_uhci_init(void) {
    pci_device_t* devs = pci_get_devices();
    int n = pci_device_count();
    for (int i = 0; i < n && uhci_count < UHCI_MAX; i++) {
        pci_device_t* p = &devs[i];
        if (!p->active || p->class_code != PCI_CLASS_SERIAL ||
            p->subclass != PCI_SUBCLASS_USB || p->prog_if != PCI_PROGIF_UHCI)
            continue;
        usb_hcd_t* hc = usb_hcd_alloc();
        if (!hc) break;
        uhci_t* u = &uhcis[uhci_count];
        memset(u, 0, sizeof(*u));
        if (!uhci_setup(u, p)) continue;
        u->hc = hc;
        hc->name = "UHCI";
        hc->type = USB_HCD_UHCI;
        hc->nports = 2;
        hc->base = u->io;
        hc->pci = p;
        hc->priv = u;
        hc->port_status = uhci_port_status;
        hc->port_reset = uhci_port_reset;
        hc->control = uhci_control;
        hc->interrupt_in = uhci_interrupt_in;
        hc->bulk = uhci_bulk;
        hc->release_device = uhci_release_device;
        uhci_count++;
        usb_hcd_register(hc);
    }
}

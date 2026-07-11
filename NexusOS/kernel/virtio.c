/* ============================================================================
 * NexusOS — VirtIO PCI Transport (Implementation) — Phase 41
 * ============================================================================
 * Implements the VirtIO 1.0 "modern" PCI transport:
 *
 *   1. Walk the PCI capability list for vendor caps (ID 0x09); each one names
 *      a config structure (common / notify / ISR / device) as BAR + offset.
 *   2. Identity-map those MMIO windows (the BARs live above the 16 MB
 *      identity-mapped region, so pages are added on demand, cache-disabled).
 *   3. Feature negotiation through the two 32-bit select/value windows —
 *      no 64-bit arithmetic anywhere (no libgcc is linked).
 *   4. Split virtqueues allocated from the identity-mapped heap, driven in
 *      polled mode: place a descriptor chain, ring the doorbell, spin on the
 *      used ring with a tick timeout + spin cap so a dead device can't hang
 *      the kernel (same defensive pattern as the AC'97 driver).
 * ============================================================================ */

#include "virtio.h"
#include "paging.h"
#include "heap.h"
#include "string.h"

extern volatile uint32_t system_ticks;
#define TICK_MS 55

/* x86 stores are totally ordered and QEMU/TCG completes the request during
 * the doorbell MMIO exit, so a compiler barrier is all the fencing we need. */
#define vmb() __asm__ volatile("" ::: "memory")

/* PTE bit 4 = PCD: cache-disable for MMIO pages */
#define PAGE_NOCACHE 0x10

/* --------------------------------------------------------------------------
 * virtio_map: identity-map an MMIO window and return it as a pointer
 * -------------------------------------------------------------------------- */
static void* virtio_map(uint32_t phys, uint32_t len) {
    uint32_t first = phys & 0xFFFFF000;
    uint32_t last  = (phys + len - 1) & 0xFFFFF000;
    for (uint32_t p = first; ; p += PAGE_SIZE) {
        paging_map_page(p, p, PAGE_WRITABLE | PAGE_NOCACHE);
        if (p == last) break;
    }
    return (void*)phys;
}

/* --------------------------------------------------------------------------
 * virtio_pci_setup: find the vendor caps, map config windows
 * -------------------------------------------------------------------------- */
bool virtio_pci_setup(virtio_dev_t* vd, pci_device_t* pci) {
    memset(vd, 0, sizeof(*vd));
    vd->pci = pci;

    /* Enable memory space + bus mastering. Also set INTx-disable (bit 10):
     * this driver polls, and virtio's level-triggered INTx would otherwise
     * storm the PIC forever once the first request completes (the ISR is
     * never read), freezing the kernel to a crawl. */
    uint32_t cmd = pci_read32(pci->bus, pci->device, pci->function, PCI_COMMAND);
    cmd |= (1 << 1) | (1 << 2) | (1 << 10);
    pci_write32(pci->bus, pci->device, pci->function, PCI_COMMAND, cmd);

    uint8_t cap = pci_read8(pci->bus, pci->device, pci->function, 0x34) & 0xFC;
    int guard = 0;
    while (cap && guard++ < 48) {
        uint8_t id   = pci_read8(pci->bus, pci->device, pci->function, cap);
        uint8_t next = pci_read8(pci->bus, pci->device, pci->function, cap + 1) & 0xFC;
        if (id == 0x09) {   /* vendor-specific = virtio structure descriptor */
            uint8_t  cfg_type = pci_read8 (pci->bus, pci->device, pci->function, cap + 3);
            uint8_t  bar      = pci_read8 (pci->bus, pci->device, pci->function, cap + 4);
            uint32_t off      = pci_read32(pci->bus, pci->device, pci->function, cap + 8);
            uint32_t len      = pci_read32(pci->bus, pci->device, pci->function, cap + 12);

            if (bar < 6 && len != 0 && !(pci->bar[bar] & 1)) {  /* memory BAR only */
                uint32_t base = (pci->bar[bar] & 0xFFFFFFF0) + off;
                switch (cfg_type) {
                    case VIRTIO_PCI_CAP_COMMON_CFG:
                        vd->common = (volatile virtio_common_cfg_t*)virtio_map(base, len);
                        break;
                    case VIRTIO_PCI_CAP_NOTIFY_CFG:
                        vd->notify_base = (uint8_t*)virtio_map(base, len);
                        vd->notify_off_multiplier =
                            pci_read32(pci->bus, pci->device, pci->function, cap + 16);
                        break;
                    case VIRTIO_PCI_CAP_ISR_CFG:
                        vd->isr = (volatile uint8_t*)virtio_map(base, len);
                        break;
                    case VIRTIO_PCI_CAP_DEVICE_CFG:
                        vd->device_cfg = (volatile uint8_t*)virtio_map(base, len);
                        break;
                }
            }
        }
        cap = next;
    }

    if (!vd->common || !vd->notify_base) return false;
    vd->present = true;
    return true;
}

/* --------------------------------------------------------------------------
 * virtio_negotiate: reset → ACK → DRIVER → features → FEATURES_OK
 * -------------------------------------------------------------------------- */
bool virtio_negotiate(virtio_dev_t* vd, uint32_t want_lo, uint32_t want_hi) {
    volatile virtio_common_cfg_t* c = vd->common;

    c->device_status = 0;                              /* reset */
    for (int i = 0; i < 100000 && c->device_status != 0; i++) vmb();

    c->device_status = VIRTIO_STATUS_ACKNOWLEDGE;
    c->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;

    c->device_feature_select = 0; vmb();
    vd->dev_features_lo = c->device_feature;
    c->device_feature_select = 1; vmb();
    vd->dev_features_hi = c->device_feature;

    /* VERSION_1 is mandatory for the modern transport. */
    if (!(vd->dev_features_hi & VIRTIO_F_VERSION_1_HI)) {
        c->device_status = VIRTIO_STATUS_FAILED;
        return false;
    }
    vd->features_lo = want_lo & vd->dev_features_lo;
    vd->features_hi = (want_hi & vd->dev_features_hi) | VIRTIO_F_VERSION_1_HI;

    c->driver_feature_select = 0; vmb();
    c->driver_feature = vd->features_lo;
    c->driver_feature_select = 1; vmb();
    c->driver_feature = vd->features_hi;

    c->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                       VIRTIO_STATUS_FEATURES_OK;
    if (!(c->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        c->device_status = VIRTIO_STATUS_FAILED;       /* device rejected them */
        return false;
    }
    return true;
}

/* --------------------------------------------------------------------------
 * virtio_queue_init: allocate split rings and hand them to the device
 * -------------------------------------------------------------------------- */
bool virtio_queue_init(virtio_dev_t* vd, virtq_t* q, uint16_t index, uint16_t max_size) {
    volatile virtio_common_cfg_t* c = vd->common;

    c->queue_select = index; vmb();
    uint16_t qs = c->queue_size;
    if (qs == 0) return false;
    if (qs > max_size) {            /* shrink to keep heap use modest */
        c->queue_size = max_size; vmb();
        qs = c->queue_size;
    }

    /* One block: descriptors, avail ring, used ring (4-byte aligned). */
    uint32_t desc_sz  = sizeof(virtq_desc_t) * qs;
    uint32_t avail_sz = 4 + 2 * qs;
    uint32_t used_off = (desc_sz + avail_sz + 3) & ~3u;
    uint32_t used_sz  = 4 + sizeof(virtq_used_elem_t) * qs;

    uint8_t* mem = (uint8_t*)kmalloc_aligned(used_off + used_sz, 4096);
    if (!mem) return false;
    memset(mem, 0, used_off + used_sz);

    q->index     = index;
    q->size      = qs;
    q->desc      = (virtq_desc_t*)mem;
    q->avail     = (volatile virtq_avail_t*)(mem + desc_sz);
    q->used      = (volatile virtq_used_t*)(mem + used_off);
    q->last_used = 0;
    q->avail->flags = 1;     /* VIRTQ_AVAIL_F_NO_INTERRUPT — we poll */
    q->notify    = (volatile uint16_t*)
        (vd->notify_base + c->queue_notify_off * vd->notify_off_multiplier);

    /* Heap is identity-mapped (<16MB): virtual address == physical address. */
    c->queue_desc_lo   = (uint32_t)q->desc;  c->queue_desc_hi   = 0;
    c->queue_driver_lo = (uint32_t)q->avail; c->queue_driver_hi = 0;
    c->queue_device_lo = (uint32_t)q->used;  c->queue_device_hi = 0;
    c->queue_enable = 1;
    return true;
}

void virtio_driver_ok(virtio_dev_t* vd) {
    vd->common->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                                VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK;
}

/* --------------------------------------------------------------------------
 * virtio_run: one synchronous request through a queue (polled)
 * -------------------------------------------------------------------------- */
int virtio_run(virtio_dev_t* vd, virtq_t* q,
               const void* out, uint32_t out_len, void* in, uint32_t in_len) {
    if (!q->size || !out || out_len == 0) return -1;

    q->desc[0].addr_lo = (uint32_t)out;
    q->desc[0].addr_hi = 0;
    q->desc[0].len     = out_len;
    if (in && in_len) {
        q->desc[0].flags = VIRTQ_DESC_F_NEXT;
        q->desc[0].next  = 1;
        q->desc[1].addr_lo = (uint32_t)in;
        q->desc[1].addr_hi = 0;
        q->desc[1].len     = in_len;
        q->desc[1].flags   = VIRTQ_DESC_F_WRITE;
        q->desc[1].next    = 0;
    } else {
        q->desc[0].flags = 0;
        q->desc[0].next  = 0;
    }

    q->avail->ring[q->avail->idx % q->size] = 0;   /* chain head = desc 0 */
    vmb();
    q->avail->idx++;
    vmb();
    *q->notify = q->index;                         /* ring the doorbell */

    /* Poll the used ring: ~1s tick timeout plus a spin hard-cap. */
    uint32_t start = system_ticks, spins = 0;
    int result = -1;
    while (q->used->idx == q->last_used) {
        if ((system_ticks - start) > (1000 / TICK_MS) + 2) goto done;
        if (++spins > 50000000u) goto done;
        vmb();
    }
    result = (int)q->used->ring[q->last_used % q->size].len;
    q->last_used++;
done:
    /* Ack any latched interrupt status (a read clears it) so INTx can never
     * be left asserted, whatever the device thinks of our masking. */
    if (vd->isr) (void)*vd->isr;
    return result;
}

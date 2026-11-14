#include <hal/core.h>
/*
 * kernel/drivers/usb/xhci.c
 * xHCI (USB 3.x) host controller driver — polled, boot-input oriented.
 *
 * Covers USB low/full/high/super-speed devices directly (no companion
 * controllers), which is why it is the primary target: it is what UTM "USB
 * 3.0" and `-device qemu-xhci` expose. Implements usb_hcd_ops for the USB core.
 *
 * Model: all rings are polled. The event ring is consumed synchronously after
 * each command/transfer; no MSI/IRQ is wired (input is serviced from
 * usb_hid_poll()). Memory for rings/contexts is page-allocated (pmm_alloc_page
 * returns a kernel VA; the controller is programmed with virt_to_phys()).
 *
 * NOTE(ASTRA-VIOLATION): calls hal_vmm_map_device() directly (amd64). The
 * aarch64 ECAM phase adds the per-arch implementation behind the platform
 * contract; tracked with the PCI-ECAM work.
 */

#include <drivers/usb/usb.h>
#include <kernel/driver.h>
#include <kernel/hal.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/string.h>


/* --- Capability registers (offset from MMIO base) --- */
#define XHCI_CAPLENGTH   0x00 /* u8 */
#define XHCI_HCSPARAMS1  0x04
#define XHCI_HCSPARAMS2  0x08
#define XHCI_HCCPARAMS1  0x10
#define XHCI_DBOFF       0x14
#define XHCI_RTSOFF      0x18

/* --- Operational registers (offset from op base = base + CAPLENGTH) --- */
#define XHCI_USBCMD   0x00
#define XHCI_USBSTS   0x04
#define XHCI_CRCR     0x18 /* 64-bit */
#define XHCI_DCBAAP   0x30 /* 64-bit */
#define XHCI_CONFIG   0x38
#define XHCI_PORTSC(p) (UINT32_C(0x400) + UINT32_C(0x10) * (p))

#define USBCMD_RUN   (1u << 0)
#define USBCMD_HCRST (1u << 1)
#define USBSTS_HCH   (1u << 0)
#define USBSTS_CNR   (1u << 11)

#define PORTSC_CCS (1u << 0)  /* current connect status */
#define PORTSC_PED (1u << 1)  /* port enabled */
#define PORTSC_PR  (1u << 4)  /* port reset */
#define PORTSC_PRC (1u << 21) /* port reset change */
#define PORTSC_SPEED_SHIFT 10
#define PORTSC_SPEED_MASK UINT32_C(0xF)

/* --- Runtime interrupter 0 (offset from rt base = base + RTSOFF) --- */
#define XHCI_IR0       0x20
#define IR_IMAN        0x00
#define IR_ERSTSZ      0x08
#define IR_ERSTBA      0x10 /* 64-bit */
#define IR_ERDP        0x18 /* 64-bit */

/* --- TRB types --- */
#define TRB_NORMAL      UINT32_C(1)
#define TRB_SETUP       UINT32_C(2)
#define TRB_DATA        UINT32_C(3)
#define TRB_STATUS      UINT32_C(4)
#define TRB_LINK        UINT32_C(6)
#define TRB_ENABLE_SLOT UINT32_C(9)
#define TRB_ADDRESS_DEV UINT32_C(11)
#define TRB_CONFIG_EP   UINT32_C(12)
#define TRB_EV_TRANSFER UINT32_C(32)
#define TRB_EV_CMD_COMP UINT32_C(33)
#define TRB_EV_PORTSC   UINT32_C(34)

#define TRB_CYCLE   (1u << 0)
#define TRB_TC      (1u << 1)  /* toggle cycle (link) */
#define TRB_IOC     (1u << 5)
#define TRB_IDT     (1u << 6)  /* immediate data (setup) */

#define CC_SUCCESS 1

#define RING_TRBS UINT32_C(256)

struct xhci_trb {
    uint64_t param;
    uint32_t status;
    uint32_t control;
} __attribute__((packed));

struct xhci_ring {
    struct xhci_trb *trbs; /* VA */
    uint64_t phys;
    uint32_t enqueue;
    uint8_t cycle;
};

struct xhci_dev {
    uint8_t slot_id;
    void *devctx;   /* VA */
    void *inputctx; /* VA */
    struct xhci_ring ep0;
    struct xhci_ring ep_in; /* interrupt IN */
    uint8_t ep_in_dci;
    void *bounce;   /* one page bounce buffer (VA) */
    int pending;    /* an interrupt TRB is queued and awaiting completion */
};

struct xhci {
    volatile uint8_t *base; /* VA == phys (identity) */
    volatile uint8_t *op;
    volatile uint8_t *rt;
    volatile uint32_t *db;
    uint8_t max_slots;
    uint8_t max_ports;
    uint32_t ctxsz; /* 32 or 64 */

    struct xhci_ring cmd;
    struct xhci_trb *evt; /* event ring segment VA */
    uint64_t evt_phys;
    uint32_t evt_deq;
    uint8_t evt_cycle;
    void *dcbaa; /* VA */
    void *ctrl_bounce; /* control-transfer data bounce page (VA) */

    struct xhci_dev devs[8];
    size_t dev_count;
    struct usb_hcd hcd;
};

static struct xhci xhci_ctrls[2];
static size_t xhci_count;

/* --- MMIO accessors --- */
static inline uint32_t rd32(volatile uint8_t *p, uint32_t off) {
    return *(volatile uint32_t *)(p + off);
}
static inline void wr32(volatile uint8_t *p, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(p + off) = v;
}
static inline void wr64(volatile uint8_t *p, uint32_t off, uint64_t v) {
    *(volatile uint32_t *)(p + off) = (uint32_t)v;
    *(volatile uint32_t *)(p + off + 4) = (uint32_t)(v >> 32);
}

static void *alloc_zeroed_page(uint64_t *phys_out) {
    if (phys_out) *phys_out = 0;
    void *p = pmm_alloc_page_dma();
    if (!p) return NULL;
    memset(p, 0, PAGE_SIZE);
    if (phys_out) *phys_out = virt_to_phys(p);
    return p;
}

static void ring_init(struct xhci_ring *r) {
    uint64_t phys;
    r->trbs = (struct xhci_trb *)alloc_zeroed_page(&phys);
    r->phys = phys;
    r->enqueue = 0;
    r->cycle = 1;
    /* Link TRB at the last slot, back to the start, toggling cycle. */
    struct xhci_trb *link = &r->trbs[RING_TRBS - 1];
    link->param = phys;
    link->status = 0;
    link->control = (TRB_LINK << 10) | TRB_TC | r->cycle;
}

/* Push a TRB (param/status/control without cycle) onto a ring; returns the
 * physical address of the slot used (for matching the completion event). */
static uint64_t ring_push(struct xhci_ring *r, uint64_t param, uint32_t status,
                          uint32_t control) {
    struct xhci_trb *t = &r->trbs[r->enqueue];
    uint64_t slot_phys =
        r->phys + (uint64_t)r->enqueue * (uint64_t)sizeof(struct xhci_trb);
    t->param = param;
    t->status = status;
    t->control = control | r->cycle;
    r->enqueue++;
    if (r->enqueue == RING_TRBS - 1) {
        /* hit the Link TRB: update its cycle and wrap. */
        struct xhci_trb *link = &r->trbs[RING_TRBS - 1];
        link->control = (TRB_LINK << 10) | TRB_TC | r->cycle;
        r->enqueue = 0;
        r->cycle ^= 1;
    }
    return slot_phys;
}

/* Poll the event ring for the next event. Returns 1 and fills *evt on success,
 * 0 on timeout. Advances ERDP. */
static int evt_wait(struct xhci *x, struct xhci_trb *out) {
    for (long spin = 0; spin < 20000000L; spin++) {
        struct xhci_trb *e = &x->evt[x->evt_deq];
        if ((e->control & TRB_CYCLE) == x->evt_cycle) {
            *out = *e;
            x->evt_deq++;
            if (x->evt_deq == RING_TRBS) {
                x->evt_deq = 0;
                x->evt_cycle ^= 1;
            }
            uint64_t erdp =
                x->evt_phys +
                (uint64_t)x->evt_deq * (uint64_t)sizeof(struct xhci_trb);
            wr64(x->rt, XHCI_IR0 + IR_ERDP, erdp | (1u << 3)); /* clear EHB */
            return 1;
        }
    }
    return 0;
}

/* Issue a command TRB and wait for its Command Completion Event.
 * Returns completion code; *slot_out gets the event's slot id. */
static int cmd_exec(struct xhci *x, uint64_t param, uint32_t control,
                    uint8_t *slot_out) {
    ring_push(&x->cmd, param, 0U, control);
    x->db[0] = 0U; /* ring command doorbell */
    struct xhci_trb ev;
    /* Port-status-change and stray transfer events can interleave on the single
     * event ring; only a Command Completion Event answers a command. */
    for (uint32_t i = 0U; i < UINT32_C(64); ++i) {
        if (!evt_wait(x, &ev)) return -1;
        if (((ev.control >> 10U) & UINT32_C(0x3F)) != TRB_EV_CMD_COMP)
            continue;
        if (slot_out)
            *slot_out = (uint8_t)((ev.control >> 24U) & UINT32_C(0xFF));
        return (int)((ev.status >> 24U) & UINT32_C(0xFF));
    }
    return -1;
}

/* --- Context helpers --- */
static inline uint8_t *ctx_at(struct xhci *x, void *base, uint32_t index) {
    return (uint8_t *)base + (size_t)index * (size_t)x->ctxsz;
}

static uint16_t speed_to_maxpkt0(uint8_t speed) {
    switch (speed) {
    case USB_SPEED_LOW:  return 8;
    case USB_SPEED_SUPER: return 512;
    default: return 64; /* full / high */
    }
}

/* --- usb_hcd_ops --- */

static int xhci_num_ports(struct usb_hcd *hcd) {
    struct xhci *x = hcd->priv;
    return x->max_ports;
}

static int xhci_port_connected(struct usb_hcd *hcd, int port) {
    struct xhci *x = hcd->priv;
    if (port < 0 || (uint32_t)port >= (uint32_t)x->max_ports) return 0;
    uint32_t port_index = (uint32_t)port;
    uint32_t sc = rd32(x->op, XHCI_PORTSC(port_index));
    return (sc & PORTSC_CCS) ? 1 : 0;
}

static uint8_t xhci_port_speed(struct xhci *x, uint32_t port_index) {
    uint32_t sc = rd32(x->op, XHCI_PORTSC(port_index));
    uint32_t psv = (sc >> PORTSC_SPEED_SHIFT) & PORTSC_SPEED_MASK;
    /* QEMU xHCI PSI default: 1=full,2=low,3=high,4=super */
    switch (psv) {
    case 2: return USB_SPEED_LOW;
    case 1: return USB_SPEED_FULL;
    case 3: return USB_SPEED_HIGH;
    case 4: return USB_SPEED_SUPER;
    default: return USB_SPEED_FULL;
    }
}

/*
 * Enable Slot + Address Device using the device's topology (speed/root_port/
 * route/tier/parent). Shared by root-port enumeration and hub-child addressing;
 * the only difference between a root device and one behind a hub is the route
 * string and the TT fields, both derived from udev here.
 */
static int xhci_setup_slot(struct xhci *x, struct usb_device *udev) {
    if (x->dev_count >= sizeof(x->devs) / sizeof(x->devs[0])) return -1;

    uint8_t slot_id = 0U;
    int cc = cmd_exec(x, 0U, TRB_ENABLE_SLOT << 10U, &slot_id);
    if (cc != CC_SUCCESS || slot_id == 0) {
        pr_warn("xHCI: Enable Slot failed (cc=%d)\n", cc);
        return -1;
    }

    struct xhci_dev *xd = &x->devs[x->dev_count];
    memset(xd, 0, sizeof(*xd));
    xd->slot_id = slot_id;

    uint64_t devctx_phys, inputctx_phys;
    xd->devctx = alloc_zeroed_page(&devctx_phys);
    xd->inputctx = alloc_zeroed_page(&inputctx_phys);
    xd->bounce = alloc_zeroed_page(NULL);
    ring_init(&xd->ep0);
    if (!xd->devctx || !xd->inputctx || !xd->bounce) return -1;

    /* DCBAA[slot] = device context phys. */
    ((uint64_t *)x->dcbaa)[slot_id] = devctx_phys;

    uint16_t mp0 =
        udev->max_packet0 ? udev->max_packet0 : speed_to_maxpkt0(udev->speed);

    /* Input control context: add slot (A0) + EP0 (A1). */
    uint32_t *icc = (uint32_t *)ctx_at(x, xd->inputctx, 0);
    icc[1] = (1u << 0) | (1u << 1);

    /* Slot context: route string + speed; root-hub port; TT for a low/full
     * speed device behind a high-speed hub (split transactions). */
    uint32_t *slot = (uint32_t *)ctx_at(x, xd->inputctx, 1);
    slot[0] = (1u << 27) | ((uint32_t)udev->speed << 20) | (udev->route & 0xFFFFF);
    slot[1] = ((uint32_t)udev->root_port) << 16;
    if (udev->parent && udev->speed != USB_SPEED_HIGH && udev->speed != USB_SPEED_SUPER) {
        struct xhci_dev *pd = udev->parent->hcd_priv;
        if (pd)
            slot[2] = (uint32_t)pd->slot_id |
                      ((uint32_t)udev->hub_port << 8U);
    }

    /* EP0 context. */
    uint32_t *ep0 = (uint32_t *)ctx_at(x, xd->inputctx, 2);
    ep0[1] = (4u << 3) | (3u << 1) | ((uint32_t)mp0 << 16U);
    *(uint64_t *)&ep0[2] = xd->ep0.phys | xd->ep0.cycle;

    cc = cmd_exec(x, inputctx_phys,
                  (TRB_ADDRESS_DEV << 10U) |
                      ((uint32_t)slot_id << 24U),
                  NULL);
    if (cc != CC_SUCCESS) {
        pr_warn("xHCI: Address Device failed (cc=%d)\n", cc);
        return -1;
    }

    udev->addr = slot_id;
    udev->max_packet0 = mp0;
    udev->hcd_priv = xd;
    x->dev_count++;
    return 0;
}

static int xhci_enumerate_port(struct usb_hcd *hcd, int port,
                               struct usb_device *udev) {
    struct xhci *x = hcd->priv;
    if (port < 0 || (uint32_t)port >= (uint32_t)x->max_ports) return -1;
    uint32_t port_index = (uint32_t)port;

    /* Reset the root port (USB2 needs it; USB3 is usually already enabled). */
    uint32_t sc = rd32(x->op, XHCI_PORTSC(port_index));
    if (!(sc & PORTSC_PED)) {
        wr32(x->op, XHCI_PORTSC(port_index),
             (sc & ~UINT32_C(0x80FE0000)) | PORTSC_PR);
        for (long s = 0; s < 20000000L; s++) {
            sc = rd32(x->op, XHCI_PORTSC(port_index));
            if (sc & (PORTSC_PRC | PORTSC_PED)) break;
        }
        wr32(x->op, XHCI_PORTSC(port_index),
             (rd32(x->op, XHCI_PORTSC(port_index)) &
              ~UINT32_C(0x80FE0000)) |
                 PORTSC_PRC);
        for (long s = 0; s < 2000000L; s++) { /* settle */ }
    }
    sc = rd32(x->op, XHCI_PORTSC(port_index));
    pr_debug("xHCI: port %d after reset PORTSC=0x%08x\n", port, sc);
    if (!(sc & PORTSC_PED)) return -1;

    udev->speed = xhci_port_speed(x, port_index);
    return xhci_setup_slot(x, udev);
}

/* Address a device reached through an external hub (topology already filled). */
static int xhci_address_dev(struct usb_hcd *hcd, struct usb_device *udev) {
    return xhci_setup_slot(hcd->priv, udev);
}

/* Control transfer on EP0. */
static int xhci_control(struct usb_hcd *hcd, struct usb_device *udev,
                        const struct usb_setup *setup, void *data, int len) {
    struct xhci *x = hcd->priv;
    struct xhci_dev *xd = udev->hcd_priv;
    if (!xd || len < 0 || (size_t)len > PAGE_SIZE) return -1;
    if (len > 0 && data == NULL) return -1;

    uint32_t transfer_len = (uint32_t)len;
    int dir_in = (setup->bmRequestType & USB_DIR_IN) != 0;

    /* Setup Stage: immediate data = the 8 setup bytes. */
    uint64_t setup_param;
    memcpy(&setup_param, setup, 8U);
    uint32_t trt =
        transfer_len == 0U ? 0U : (dir_in ? UINT32_C(3) : UINT32_C(2));
    ring_push(&xd->ep0, setup_param, UINT32_C(8),
              (TRB_SETUP << 10U) | TRB_IDT | (trt << 16U));

    /* Data Stage. */
    if (transfer_len > 0U) {
        size_t copy_len = (size_t)transfer_len;
        if (dir_in)
            memset(xd->bounce, 0, copy_len);
        else
            memcpy(xd->bounce, data, copy_len);
        uint32_t ctl =
            (TRB_DATA << 10U) | (dir_in ? (UINT32_C(1) << 16U) : 0U);
        ring_push(&xd->ep0, virt_to_phys(xd->bounce), transfer_len, ctl);
    }

    /* Status Stage (IOC so we get a transfer event). */
    uint32_t sdir =
        (transfer_len > 0U && dir_in) ? 0U : (UINT32_C(1) << 16U);
    ring_push(&xd->ep0, 0U, 0U, (TRB_STATUS << 10U) | sdir | TRB_IOC);

    x->db[xd->slot_id] = UINT32_C(1); /* DB target = EP0 (DCI 1) */

    struct xhci_trb ev;
    int got = 0;
    uint32_t residual = 0U;
    /* Wait for the transfer event from the Status Stage. */
    for (uint32_t tries = 0U; tries < UINT32_C(4); ++tries) {
        if (!evt_wait(x, &ev)) break;
        if (((ev.control >> 10U) & UINT32_C(0x3F)) == TRB_EV_TRANSFER) {
            residual = ev.status & UINT32_C(0x00FFFFFF);
            uint32_t cc = (ev.status >> 24U) & UINT32_C(0xFF);
            if (cc != UINT32_C(CC_SUCCESS) && cc != UINT32_C(13))
                return -1;
            got = 1;
            break;
        }
    }
    if (!got || residual > transfer_len) return -1;

    uint32_t transferred = transfer_len - residual;
    if (dir_in && data && transferred > 0U)
        memcpy(data, xd->bounce, (size_t)transferred);
    return (int)transferred;
}

/* Configure an interrupt IN endpoint and queue the first transfer. */
static int xhci_intr_open(struct usb_hcd *hcd, struct usb_device *udev,
                          uint8_t ep_addr, int max_packet, int interval) {
    struct xhci *x = hcd->priv;
    struct xhci_dev *xd = udev->hcd_priv;
    if (!xd || max_packet <= 0 || (size_t)max_packet > PAGE_SIZE ||
        interval < 0 || interval > (int)UINT8_MAX)
        return -1;

    uint8_t ep_num = (uint8_t)(ep_addr & UINT8_C(0x0F));
    uint8_t dci = (uint8_t)((uint32_t)ep_num * 2U + 1U); /* IN endpoint */
    uint32_t dci32 = (uint32_t)dci;
    uint32_t max_packet32 = (uint32_t)max_packet;
    xd->ep_in_dci = dci;
    ring_init(&xd->ep_in);

    /* Input context: A0 (slot) + A(dci). */
    memset(xd->inputctx, 0, PAGE_SIZE);
    uint32_t *icc = (uint32_t *)ctx_at(x, xd->inputctx, 0U);
    icc[1] = (UINT32_C(1) << 0U) | (UINT32_C(1) << dci32);

    uint32_t *slot = (uint32_t *)ctx_at(x, xd->inputctx, 1U);
    slot[0] = (dci32 << 27U) | ((uint32_t)udev->speed << 20U);
    slot[1] = ((uint32_t)udev->root_port) << 16U;

    uint32_t *ep = (uint32_t *)ctx_at(x, xd->inputctx, dci32 + 1U);
    uint32_t ival = interval > 0 ? (uint32_t)interval : UINT32_C(8);
    ep[0] = ival << 16U;
    ep[1] = (UINT32_C(7) << 3U) /* interrupt IN */ |
            (UINT32_C(3) << 1U) | (max_packet32 << 16U);
    *(uint64_t *)&ep[2] = xd->ep_in.phys | xd->ep_in.cycle;
    ep[4] = max_packet32; /* avg TRB length */

    int cc = cmd_exec(x, virt_to_phys(xd->inputctx),
                      (TRB_CONFIG_EP << 10U) |
                          ((uint32_t)xd->slot_id << 24U),
                      NULL);
    if (cc != CC_SUCCESS) {
        pr_warn("xHCI: Configure Endpoint failed (cc=%d)\n", cc);
        return -1;
    }

    /* Queue the first interrupt IN transfer. */
    memset(xd->bounce, 0, (size_t)max_packet32);
    ring_push(&xd->ep_in, virt_to_phys(xd->bounce), max_packet32,
              (TRB_NORMAL << 10U) | TRB_IOC);
    x->db[xd->slot_id] = dci32;
    xd->pending = 1;
    return 0;
}

static int xhci_intr_poll(struct usb_hcd *hcd, struct usb_device *udev,
                          uint8_t ep_addr, void *buf, int len) {
    struct xhci *x = hcd->priv;
    struct xhci_dev *xd = udev->hcd_priv;
    (void)ep_addr;
    if (!xd || !xd->pending || len <= 0 || buf == NULL) return 0;

    /* Non-blocking: is there an event at the ring head? The event ring is shared
     * by every endpoint, so only consume it if it is a Transfer Event for THIS
     * device's slot and interrupt EP; otherwise leave it for the owner. */
    struct xhci_trb *e = &x->evt[x->evt_deq];
    if ((e->control & TRB_CYCLE) != x->evt_cycle) return 0;
    uint32_t type = (e->control >> 10U) & UINT32_C(0x3F);
    if (type != TRB_EV_TRANSFER) {
        struct xhci_trb junk;
        (void)evt_wait(x, &junk); /* drain port-status/other events */
        return 0;
    }
    uint8_t ev_slot =
        (uint8_t)((e->control >> 24U) & UINT32_C(0xFF));
    uint8_t ev_dci =
        (uint8_t)((e->control >> 16U) & UINT32_C(0x1F));
    if (ev_slot != xd->slot_id || ev_dci != xd->ep_in_dci)
        return 0; /* belongs to another device; its own poll will take it */

    struct xhci_trb ev;
    if (!evt_wait(x, &ev)) return 0;
    uint32_t residual = ev.status & UINT32_C(0x00FFFFFF);
    uint32_t packet = (uint32_t)udev->hid_ep_max;
    if (residual > packet) return -1;
    uint32_t got = packet - residual;
    uint32_t caller_capacity = (uint32_t)len;
    if (got > caller_capacity) got = caller_capacity;
    if (got > 0U) memcpy(buf, xd->bounce, (size_t)got);

    /* Re-queue another interrupt transfer. */
    memset(xd->bounce, 0, (size_t)packet);
    ring_push(&xd->ep_in, virt_to_phys(xd->bounce), packet,
              (TRB_NORMAL << 10U) | TRB_IOC);
    x->db[xd->slot_id] = (uint32_t)xd->ep_in_dci;
    return (int)got;
}

static struct usb_hcd_ops xhci_ops = {
    .num_ports = xhci_num_ports,
    .port_connected = xhci_port_connected,
    .enumerate_port = xhci_enumerate_port,
    .address_dev = xhci_address_dev,
    .control = xhci_control,
    .intr_open = xhci_intr_open,
    .intr_poll = xhci_intr_poll,
};

static int xhci_reset_and_start(struct xhci *x) {
    /* Halt. */
    uint32_t cmd = rd32(x->op, XHCI_USBCMD);
    wr32(x->op, XHCI_USBCMD, cmd & ~USBCMD_RUN);
    for (long s = 0; s < 5000000L; s++)
        if (rd32(x->op, XHCI_USBSTS) & USBSTS_HCH) break;

    /* Reset. */
    wr32(x->op, XHCI_USBCMD, USBCMD_HCRST);
    for (long s = 0; s < 5000000L; s++)
        if (!(rd32(x->op, XHCI_USBCMD) & USBCMD_HCRST)) break;
    for (long s = 0; s < 5000000L; s++)
        if (!(rd32(x->op, XHCI_USBSTS) & USBSTS_CNR)) break;

    /* Max device slots enabled. */
    wr32(x->op, XHCI_CONFIG, (uint32_t)x->max_slots);

    /* DCBAA. */
    uint64_t dcbaa_phys;
    x->dcbaa = alloc_zeroed_page(&dcbaa_phys);
    if (!x->dcbaa) return -1;
    wr64(x->op, XHCI_DCBAAP, dcbaa_phys);

    /* Command ring. */
    ring_init(&x->cmd);
    wr64(x->op, XHCI_CRCR, x->cmd.phys | 1 /* RCS */);

    /* Event ring + ERST (single segment). */
    x->evt = (struct xhci_trb *)alloc_zeroed_page(&x->evt_phys);
    x->evt_deq = 0;
    x->evt_cycle = 1;
    uint64_t erst_phys;
    uint32_t *erst = (uint32_t *)alloc_zeroed_page(&erst_phys);
    if (!x->evt || !erst) return -1;
    erst[0] = (uint32_t)x->evt_phys;
    erst[1] = (uint32_t)(x->evt_phys >> 32);
    erst[2] = RING_TRBS; /* segment size */
    wr32(x->rt, XHCI_IR0 + IR_ERSTSZ, UINT32_C(1));
    wr64(x->rt, XHCI_IR0 + IR_ERDP, x->evt_phys);
    wr64(x->rt, XHCI_IR0 + IR_ERSTBA, erst_phys);

    x->ctrl_bounce = alloc_zeroed_page(NULL);

    /* Run. */
    wr32(x->op, XHCI_USBCMD, USBCMD_RUN);
    for (long s = 0; s < 5000000L; s++)
        if (!(rd32(x->op, XHCI_USBSTS) & USBSTS_HCH)) break;

    /* Power all root ports (PP, bit 9) so connect status (CCS) becomes valid,
     * then let it settle before enumeration reads PORTSC. */
    for (uint32_t p = 0U; p < (uint32_t)x->max_ports; ++p) {
        uint32_t sc = rd32(x->op, XHCI_PORTSC(p));
        if (!(sc & (UINT32_C(1) << 9U)))
            wr32(x->op, XHCI_PORTSC(p),
                 (sc & ~UINT32_C(0x80FE0000)) |
                     (UINT32_C(1) << 9U));
    }
    for (long s = 0; s < 3000000L; s++) { /* settle */ }
    return 0;
}

static int xhci_probe(struct hal_device *dev) {
    if (xhci_count >= sizeof(xhci_ctrls) / sizeof(xhci_ctrls[0])) return -1;
    struct xhci *x = &xhci_ctrls[xhci_count];
    memset(x, 0, sizeof(*x));

    uint64_t base = dev->base;
    hal_vmm_map_device(base, 0x20000);
    x->base = (volatile uint8_t *)phys_to_virt(base);

    uint8_t caplen = *(volatile uint8_t *)(x->base + XHCI_CAPLENGTH);
    uint32_t hcs1 = rd32(x->base, XHCI_HCSPARAMS1);
    uint32_t hcc1 = rd32(x->base, XHCI_HCCPARAMS1);
    x->max_slots = (uint8_t)(hcs1 & UINT32_C(0xFF));
    x->max_ports =
        (uint8_t)((hcs1 >> 24U) & UINT32_C(0xFF));
    x->ctxsz =
        (hcc1 & (UINT32_C(1) << 2U)) ? UINT32_C(64) : UINT32_C(32);

    x->op = x->base + caplen;
    x->rt = x->base + (rd32(x->base, XHCI_RTSOFF) & ~0x1Fu);
    x->db = (volatile uint32_t *)(x->base + (rd32(x->base, XHCI_DBOFF) & ~0x3u));

    pr_info("xHCI: %s base=0x%lx slots=%u ports=%u ctxsz=%u\n",
            dev->name, base, (unsigned)x->max_slots,
            (unsigned)x->max_ports, (unsigned)x->ctxsz);

    if (xhci_reset_and_start(x) < 0) {
        pr_warn("%s", "xHCI: controller init failed\n");
        return -1;
    }

    x->hcd.name = "xHCI";
    x->hcd.ops = &xhci_ops;
    x->hcd.priv = x;
    xhci_count++;

    usb_register_hcd(&x->hcd);
    return 0;
}

static struct device_driver xhci_driver = {
    .name = "xhci",
    .vendor = DRV_ANY_ID,
    .device = DRV_ANY_ID,
    .class_code = 0x0C,  /* Serial bus controller */
    .subclass = 0x03,    /* USB */
    .prog_if = 0x30,
    .flags = DEVICE_DRIVER_DMA,     /* xHCI */
    .probe = xhci_probe,
};

void xhci_register(void) {
    driver_register(&xhci_driver);
}

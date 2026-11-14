#include <hal/core.h>
/*
 * kernel/drivers/usb/ehci.c
 * EHCI (USB 2.0) host controller driver — polled.
 *
 * EHCI only drives high-speed devices directly; low/full-speed devices (USB
 * boot keyboards/mice) are handed to a companion UHCI/OHCI by setting the port
 * owner. So this driver's two jobs are: (1) claim the ports, reset each one and
 * route non-high-speed devices to the companion (which our UHCI driver then
 * enumerates), and (2) run an async (control) + periodic (interrupt) schedule
 * for any genuine high-speed device. Polled, like xHCI/UHCI.
 */

#include <drivers/usb/usb.h>
#include <kernel/driver.h>
#include <kernel/hal.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/string.h>


/* Capability registers (from MMIO base). */
#define EHCI_CAPLENGTH 0x00 /* u8 */
#define EHCI_HCSPARAMS 0x04

/* Operational registers (from base + CAPLENGTH). */
#define EHCI_USBCMD    0x00
#define EHCI_USBSTS    0x04
#define EHCI_USBINTR   0x08
#define EHCI_FRINDEX   0x0C
#define EHCI_CTRLDSSEG 0x10
#define EHCI_PERIODICLB 0x14
#define EHCI_ASYNCLB   0x18
#define EHCI_CONFIGFLAG 0x40
#define EHCI_PORTSC(n) (UINT32_C(0x44) + UINT32_C(4) * (n))

#define CMD_RUN     (1u << 0)
#define CMD_HCRESET (1u << 1)
#define CMD_PSE     (1u << 4)
#define CMD_ASE     (1u << 5)
#define STS_HALTED  (1u << 12)

#define PORT_CCS   (1u << 0)
#define PORT_CSC   (1u << 1)
#define PORT_PED   (1u << 2)
#define PORT_PEDC  (1u << 3)
#define PORT_RESET (1u << 8)
#define PORT_POWER (1u << 12)
#define PORT_OWNER (1u << 13)
#define PORT_LS_KSTATE (1u << 10) /* line status bit: low-speed (K-state) */

/* qTD token. */
#define QTD_ACTIVE (1u << 7)
#define QTD_HALTED (1u << 6)
#define QTD_PID_OUT   0
#define QTD_PID_IN    1
#define QTD_PID_SETUP 2
#define QTD_IOC    (1u << 15)
#define QTD_T      1u /* terminate */

/* Link pointer type. */
#define LP_QH (1u << 1)
#define LP_T  1u

/* Exactly 32 bytes: qTDs are packed in a pool and the hardware requires each to
 * be 32-byte aligned, so the struct size must stay a multiple of 32. */
struct ehci_qtd {
    volatile uint32_t next;
    volatile uint32_t alt_next;
    volatile uint32_t token;
    volatile uint32_t buf[5];
} __attribute__((packed));

struct ehci_qh {
    volatile uint32_t horiz;
    volatile uint32_t ep_char;   /* addr, ep, EPS, max packet, H, DTC */
    volatile uint32_t ep_cap;    /* mult, hub addr/port, s/c-mask */
    volatile uint32_t cur_qtd;
    /* qTD overlay */
    volatile uint32_t ov_next;
    volatile uint32_t ov_alt;
    volatile uint32_t ov_token;
    volatile uint32_t ov_buf[5];
    uint32_t pad[4]; /* pad to 64B (48B used) */
} __attribute__((packed));

struct ehci_ep {
    struct ehci_qh *qh;
    uint32_t qh_phys;
    struct ehci_qtd *qtd;
    uint32_t qtd_phys;
    void *buf;
    uint32_t buf_phys;
    uint8_t toggle;
    uint32_t maxlen;
    int active;
};

struct ehci {
    volatile uint8_t *base;
    volatile uint8_t *op;
    uint8_t nports;
    uint32_t *framelist;
    uint32_t framelist_phys;
    struct ehci_qh *async; /* async head QH (H=1) */
    uint32_t async_phys;

    struct ehci_qh *ctrl_qh;     /* reused per control transfer */
    uint32_t ctrl_qh_phys;
    struct ehci_qtd *ctrl_qtd;   /* pool */
    uint32_t ctrl_qtd_phys;
    void *ctrl_buf;
    uint32_t ctrl_buf_phys;

    struct ehci_ep eps[4];
    size_t ep_count;
    uint8_t next_addr;
    struct usb_hcd hcd;
};

static struct ehci ehci_ctrls[2];
static size_t ehci_count;

static inline uint32_t rd32(volatile uint8_t *p, uint32_t o) { return *(volatile uint32_t *)(p + o); }
static inline void wr32(volatile uint8_t *p, uint32_t o, uint32_t v) { *(volatile uint32_t *)(p + o) = v; }

static void *page_va(uint32_t *phys_out) {
    if (phys_out) *phys_out = 0U;
    void *p = pmm_alloc_page_dma();
    if (!p) return NULL;
    uint64_t phys = virt_to_phys(p);
    if (phys > UINT32_MAX) {
        pmm_free_page(p);
        return NULL;
    }
    memset(p, 0, PAGE_SIZE);
    if (phys_out) *phys_out = (uint32_t)phys;
    return p;
}

static int ehci_address(struct usb_hcd *hcd, struct usb_device *dev);

static int qtd_phys_at(uint32_t base, size_t index, uint32_t *out) {
    if (!out) return -1;
    size_t offset = index * sizeof(struct ehci_qtd);
    if (index != 0U && offset / index != sizeof(struct ehci_qtd))
        return -1;
    uint64_t phys = (uint64_t)base + (uint64_t)offset;
    if (phys > UINT32_MAX) return -1;
    *out = (uint32_t)phys;
    return 0;
}

/* Fill a qTD: pid, data toggle, length, buffer phys, IOC. */
static void qtd_fill(struct ehci_qtd *t, uint32_t next, uint32_t pid,
                     uint8_t toggle, uint32_t len, uint32_t buf, int ioc) {
    t->next = next;
    t->alt_next = LP_T;
    uint32_t tok = QTD_ACTIVE | (pid << 8U) | (UINT32_C(3) << 10U) |
                   (len << 16U) | ((uint32_t)(toggle & UINT8_C(1)) << 31U);
    if (ioc) tok |= QTD_IOC;
    t->token = tok;
    for (uint32_t i = 0U; i < UINT32_C(5); ++i) t->buf[i] = 0U;
    if (len != 0U) {
        t->buf[0] = buf;
        t->buf[1] = (buf + UINT32_C(0x1000)) & ~UINT32_C(0x0FFF);
    }
}

/* Run a qTD chain (starting at ctrl_qtd[0]) through the control QH and wait for
 * the STATUS qTD to retire. We must poll the qTD itself, not the QH overlay:
 * the overlay starts inactive (so the controller fetches the first qTD), so
 * polling it would report "done" immediately. Returns 0 on success. */
static int ehci_run_ctrl(struct ehci *e, struct usb_device *dev,
                         struct ehci_qtd *last) {
    struct ehci_qh *qh = e->ctrl_qh;
    uint32_t eps = UINT32_C(0);
    if (dev->speed == USB_SPEED_HIGH)
        eps = UINT32_C(2);
    else if (dev->speed == USB_SPEED_LOW)
        eps = UINT32_C(1);
    uint32_t mp =
        dev->max_packet0 ? (uint32_t)dev->max_packet0 : UINT32_C(8);
    qh->ep_char = (uint32_t)dev->addr | (eps << 12U) |
                  (UINT32_C(1) << 14U) /* DTC: toggle from qTD */ |
                  (mp << 16U) |
                  ((eps != UINT32_C(2)) ? (UINT32_C(1) << 27U) : 0U);
    qh->ep_cap = UINT32_C(1) << 30U; /* mult = 1 */
    qh->cur_qtd = 0U;
    qh->ov_next = e->ctrl_qtd_phys; /* first qTD */
    qh->ov_alt = LP_T;
    qh->ov_token = 0U; /* inactive -> controller loads the first qTD */

    for (long s = 0; s < 60000000L; s++) {
        if (!(last->token & QTD_ACTIVE)) break;
    }
    if (last->token & (QTD_ACTIVE | QTD_HALTED)) return -1;
    return 0;
}

static int ehci_control(struct usb_hcd *hcd, struct usb_device *dev,
                        const struct usb_setup *setup, void *data, int len) {
    struct ehci *e = hcd->priv;
    if (!e || !setup || len < 0 ||
        (size_t)len > PAGE_SIZE - UINT32_C(64))
        return -1;
    struct ehci_qtd *td = e->ctrl_qtd;
    if (!td) return -1;
    if (len > 0 && data == NULL) return -1;

    uint32_t transfer_len = (uint32_t)len;
    int dir_in = (setup->bmRequestType & USB_DIR_IN) != 0;
    uint32_t next_setup = 0U;
    uint32_t next_data = 0U;
    if (qtd_phys_at(e->ctrl_qtd_phys, 1U, &next_setup) != 0 ||
        qtd_phys_at(e->ctrl_qtd_phys, 2U, &next_data) != 0)
        return -1;

    /* SETUP (DATA0). */
    memcpy(e->ctrl_buf, setup, 8U);
    size_t last = transfer_len > 0U ? 2U : 1U; /* STATUS stage */
    qtd_fill(&td[0], next_setup, UINT32_C(QTD_PID_SETUP), 0U,
             UINT32_C(8), e->ctrl_buf_phys, 0);

    if (transfer_len > 0U) {
        size_t copy_len = (size_t)transfer_len;
        if (!dir_in)
            memcpy((uint8_t *)e->ctrl_buf + 64U, data, copy_len);
        qtd_fill(&td[1], next_data,
                 dir_in ? UINT32_C(QTD_PID_IN) : UINT32_C(QTD_PID_OUT),
                 UINT8_C(1), transfer_len,
                 e->ctrl_buf_phys + UINT32_C(64), 0);
    }

    /* STATUS (DATA1, opposite dir, IOC). */
    qtd_fill(&td[last], LP_T,
             dir_in ? UINT32_C(QTD_PID_OUT) : UINT32_C(QTD_PID_IN),
             UINT8_C(1), 0U, 0U, 1);

    if (ehci_run_ctrl(e, dev, &td[last]) < 0) return -1;

    uint32_t transferred = transfer_len;
    if (dir_in && transfer_len > 0U) {
        uint32_t rem =
            (td[1].token >> 16U) & UINT32_C(0x7FFF); /* not transferred */
        if (rem > transfer_len) return -1;
        transferred = transfer_len - rem;
        if (transferred > 0U)
            memcpy(data, (uint8_t *)e->ctrl_buf + 64U,
                   (size_t)transferred);
    }
    return (int)transferred;
}

/* --- ports --- */
static int ehci_num_ports(struct usb_hcd *hcd) {
    struct ehci *e = hcd->priv;
    return e->nports;
}
static int ehci_port_connected(struct usb_hcd *hcd, int port) {
    struct ehci *e = hcd->priv;
    if (!e || port < 0 || (uint32_t)port >= (uint32_t)e->nports) return 0;
    uint32_t port_index = (uint32_t)port;
    return (rd32(e->op, EHCI_PORTSC(port_index)) & PORT_CCS) ? 1 : 0;
}

static int ehci_enumerate_port(struct usb_hcd *hcd, int port,
                               struct usb_device *dev) {
    struct ehci *e = hcd->priv;
    if (!e || !dev || port < 0 ||
        (uint32_t)port >= (uint32_t)e->nports)
        return -1;
    uint32_t port_index = (uint32_t)port;
    uint32_t sc = rd32(e->op, EHCI_PORTSC(port_index));
    if (!(sc & PORT_CCS)) return -1;

    /* Reset to determine speed. A genuine high-speed device ends up enabled
     * (PED) and is driven here; anything else (full/low-speed boot keyboard or
     * mouse) is released to the companion UHCI/OHCI via the port-owner bit. */
    wr32(e->op, EHCI_PORTSC(port_index),
         (sc & ~UINT32_C(0x2A)) | PORT_RESET);
    for (long s = 0; s < 8000000L; s++) { }
    wr32(e->op, EHCI_PORTSC(port_index),
         rd32(e->op, EHCI_PORTSC(port_index)) & ~PORT_RESET);
    for (long s = 0; s < 4000000L; s++) {
        if (!(rd32(e->op, EHCI_PORTSC(port_index)) & PORT_RESET)) break;
    }
    sc = rd32(e->op, EHCI_PORTSC(port_index));

    if (!(sc & PORT_PED)) {
        /* Not enabled after reset = full/low-speed: hand to the companion. */
        wr32(e->op, EHCI_PORTSC(port_index),
             (sc & ~UINT32_C(0x2A)) | PORT_OWNER);
        return -1;
    }

    dev->speed = USB_SPEED_HIGH;
    dev->max_packet0 = UINT16_C(64);
    return ehci_address(hcd, dev);
}

/* Address a device on the default pipe (addr 0). */
static int ehci_address(struct usb_hcd *hcd, struct usb_device *dev) {
    dev->addr = 0;
    struct usb_device_descriptor dd;
    memset(&dd, 0, sizeof(dd));
    struct usb_setup s8 = { USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, (USB_DT_DEVICE << 8), 0, 8 };
    if (ehci_control(hcd, dev, &s8, &dd, 8) < 8) return -1;
    if (dd.bMaxPacketSize0) dev->max_packet0 = dd.bMaxPacketSize0;

    struct ehci *e = hcd->priv;
    uint8_t new_addr = ++e->next_addr;
    struct usb_setup sa = { USB_DIR_OUT, USB_REQ_SET_ADDRESS, new_addr, 0, 0 };
    if (ehci_control(hcd, dev, &sa, NULL, 0) < 0) return -1;
    dev->addr = new_addr;
    for (long s = 0; s < 2000000L; s++) { }
    return 0;
}

static int ehci_address_dev(struct usb_hcd *hcd, struct usb_device *dev) {
    return ehci_address(hcd, dev);
}

/* --- interrupt IN endpoint (periodic schedule) --- */
static int ehci_intr_open(struct usb_hcd *hcd, struct usb_device *dev,
                          uint8_t ep_addr, int max_packet, int interval) {
    (void)interval;
    struct ehci *e = hcd->priv;
    if (!e || !dev || max_packet <= 0 ||
        (size_t)max_packet > PAGE_SIZE ||
        e->ep_count >= sizeof(e->eps) / sizeof(e->eps[0]))
        return -1;
    uint32_t max_packet32 = (uint32_t)max_packet;
    struct ehci_ep *ep = &e->eps[e->ep_count];
    memset(ep, 0, sizeof(*ep));
    ep->qh = (struct ehci_qh *)page_va(&ep->qh_phys);
    ep->qtd = (struct ehci_qtd *)page_va(&ep->qtd_phys);
    ep->buf = page_va(&ep->buf_phys);
    if (!ep->qh || !ep->qtd || !ep->buf) return -1;
    ep->maxlen = max_packet32;
    ep->toggle = 0U;
    ep->active = 1;
    dev->hcd_priv = ep;

    uint32_t epn = (uint32_t)(ep_addr & UINT8_C(0x0F));
    ep->qh->ep_char = (uint32_t)dev->addr | (epn << 8U) |
                      (UINT32_C(2) << 12U) /* HS */ |
                      (UINT32_C(1) << 14U) | (max_packet32 << 16U);
    ep->qh->ep_cap =
        (UINT32_C(1) << 30U) | UINT32_C(1); /* microframe 0 */
    ep->qh->horiz = LP_T;
    qtd_fill(ep->qtd, LP_T, UINT32_C(QTD_PID_IN), ep->toggle,
             max_packet32, ep->buf_phys, 1);
    ep->qh->ov_next = ep->qtd_phys;
    ep->qh->ov_alt = LP_T;
    ep->qh->ov_token = 0U;

    /* Link the interrupt QH into every periodic frame-list entry. */
    uint32_t link = ep->qh_phys | LP_QH;
    for (uint32_t i = 0U; i < UINT32_C(1024); ++i)
        e->framelist[i] = link;
    e->ep_count++;
    return 0;
}

static int ehci_intr_poll(struct usb_hcd *hcd, struct usb_device *dev,
                          uint8_t ep_addr, void *buf, int len) {
    (void)hcd;
    (void)ep_addr;
    if (!dev || !buf || len <= 0) return 0;
    struct ehci_ep *ep = dev->hcd_priv;
    if (!ep || !ep->active) return 0;
    if (ep->qh->ov_token & QTD_ACTIVE) return 0;

    uint32_t rem = (ep->qh->ov_token >> 16U) & UINT32_C(0x7FFF);
    int halted = (ep->qh->ov_token & QTD_HALTED) != 0;
    uint32_t got = 0U;
    if (!halted) {
        if (rem > ep->maxlen) return -1;
        got = ep->maxlen - rem;
        uint32_t capacity = (uint32_t)len;
        if (got > capacity) got = capacity;
        if (got > 0U) memcpy(buf, ep->buf, (size_t)got);
        ep->toggle ^= UINT8_C(1);
    }
    /* Re-arm. */
    qtd_fill(ep->qtd, LP_T, UINT32_C(QTD_PID_IN), ep->toggle,
             ep->maxlen, ep->buf_phys, 1);
    ep->qh->ov_next = ep->qtd_phys;
    ep->qh->ov_alt = LP_T;
    ep->qh->ov_token = 0U;
    return (int)got;
}

static struct usb_hcd_ops ehci_ops = {
    .num_ports = ehci_num_ports,
    .port_connected = ehci_port_connected,
    .enumerate_port = ehci_enumerate_port,
    .address_dev = ehci_address_dev,
    .control = ehci_control,
    .intr_open = ehci_intr_open,
    .intr_poll = ehci_intr_poll,
};

static int ehci_probe(struct hal_device *dev) {
    if (ehci_count >= sizeof(ehci_ctrls) / sizeof(ehci_ctrls[0])) return -1;
    struct ehci *e = &ehci_ctrls[ehci_count];
    memset(e, 0, sizeof(*e));

    uint64_t base = dev->base;
    if (!base) return -1;
    hal_vmm_map_device(base, 0x1000);
    e->base = (volatile uint8_t *)phys_to_virt(base);
    uint8_t caplen = *(volatile uint8_t *)(e->base + EHCI_CAPLENGTH);
    e->op = e->base + caplen;
    e->nports =
        (uint8_t)(rd32(e->base, EHCI_HCSPARAMS) & UINT32_C(0x0F));

    pr_info("EHCI: %s base=0x%lx ports=%u\n",
            dev->name, base, (unsigned)e->nports);

    /* Reset. */
    wr32(e->op, EHCI_USBCMD, rd32(e->op, EHCI_USBCMD) & ~CMD_RUN);
    for (long s = 0; s < 4000000L; s++)
        if (rd32(e->op, EHCI_USBSTS) & STS_HALTED) break;
    wr32(e->op, EHCI_USBCMD, CMD_HCRESET);
    for (long s = 0; s < 8000000L; s++)
        if (!(rd32(e->op, EHCI_USBCMD) & CMD_HCRESET)) break;

    /* Async head QH (H=1, points to itself). */
    e->async = (struct ehci_qh *)page_va(&e->async_phys);
    e->ctrl_qh = (struct ehci_qh *)page_va(&e->ctrl_qh_phys);
    e->ctrl_qtd = (struct ehci_qtd *)page_va(&e->ctrl_qtd_phys);
    e->ctrl_buf = page_va(&e->ctrl_buf_phys);
    e->framelist = (uint32_t *)page_va(&e->framelist_phys);
    if (!e->async || !e->ctrl_qh || !e->ctrl_qtd || !e->ctrl_buf || !e->framelist) return -1;

    /* async ring: head -> ctrl_qh -> head */
    e->async->ep_char = (1u << 15); /* H = 1 (head of reclamation) */
    e->async->ep_cap = (1u << 30);
    e->async->horiz = e->ctrl_qh_phys | LP_QH;
    e->async->ov_next = LP_T;
    e->async->ov_token = QTD_HALTED;
    e->ctrl_qh->horiz = e->async_phys | LP_QH;
    e->ctrl_qh->ov_next = LP_T;
    e->ctrl_qh->ov_token = QTD_HALTED;

    for (uint32_t i = 0U; i < UINT32_C(1024); ++i)
        e->framelist[i] = LP_T;

    wr32(e->op, EHCI_CTRLDSSEG, 0U);
    wr32(e->op, EHCI_PERIODICLB, e->framelist_phys);
    wr32(e->op, EHCI_ASYNCLB, e->async_phys);
    wr32(e->op, EHCI_USBINTR, 0);
    wr32(e->op, EHCI_USBCMD, CMD_RUN | CMD_ASE | CMD_PSE | (8u << 16));
    wr32(e->op, EHCI_CONFIGFLAG, 1); /* route all ports to EHCI */
    for (long s = 0; s < 2000000L; s++) { }

    /* Power ports. */
    for (uint32_t p = 0U; p < (uint32_t)e->nports; ++p) {
        uint32_t sc = rd32(e->op, EHCI_PORTSC(p));
        if (!(sc & PORT_POWER))
            wr32(e->op, EHCI_PORTSC(p), sc | PORT_POWER);
    }
    for (long s = 0; s < 3000000L; s++) { }

    e->hcd.name = "EHCI";
    e->hcd.ops = &ehci_ops;
    e->hcd.priv = e;
    ehci_count++;

    usb_register_hcd(&e->hcd);
    return 0;
}

static struct device_driver ehci_driver = {
    .name = "ehci",
    .vendor = DRV_ANY_ID,
    .device = DRV_ANY_ID,
    .class_code = 0x0C,
    .subclass = 0x03,
    .prog_if = 0x20,
    .flags = DEVICE_DRIVER_DMA, /* EHCI */
    .probe = ehci_probe,
};

void ehci_register(void) {
    driver_register(&ehci_driver);
}

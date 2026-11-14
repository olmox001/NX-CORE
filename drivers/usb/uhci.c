/*
 * kernel/drivers/usb/uhci.c
 * UHCI (USB 1.1) host controller driver — polled.
 *
 * UHCI is the companion controller that carries full/low-speed devices on
 * i440fx and on UTM "USB 2.0" (the EHCI routes 1.1 devices to it), so a USB
 * boot keyboard/mouse/tablet ends up here. Port-I/O based; schedule is a 1024-
 * entry frame list pointing at a skeleton of QHs (interrupt then control).
 * Control transfers run synchronously; interrupt IN endpoints are re-armed and
 * polled from usb_hid_poll() via the same timer tick as xHCI.
 */

#include <drivers/usb/usb.h>
#include <hal/pci.h>
#include <kernel/driver.h>
#include <kernel/hal.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/string.h>

/* --- I/O registers (offset from BAR0 I/O base) --- */
#define UHCI_USBCMD    UINT16_C(0x00)
#define UHCI_USBSTS    UINT16_C(0x02)
#define UHCI_USBINTR   UINT16_C(0x04)
#define UHCI_FRNUM     UINT16_C(0x06)
#define UHCI_FRBASEADD UINT16_C(0x08)
#define UHCI_SOFMOD    UINT16_C(0x0C)
#define UHCI_PORTSC1   UINT16_C(0x10)
#define UHCI_PORTSC2   UINT16_C(0x12)
#define UHCI_CTRL_TD_MAX UINT32_C(30)

#define CMD_RUN     UINT16_C(0x0001)
#define CMD_HCRESET UINT16_C(0x0002)
#define CMD_GRESET  UINT16_C(0x0004)
#define CMD_MAXP    UINT16_C(0x0080)

#define PORT_CCS  UINT16_C(0x0001)
#define PORT_CSC  UINT16_C(0x0002)
#define PORT_PED  UINT16_C(0x0004)
#define PORT_PEDC UINT16_C(0x0008)
#define PORT_LS   UINT16_C(0x0100)  /* low-speed device attached */
#define PORT_PR   UINT16_C(0x0200)  /* port reset */

/* Link pointer flags */
#define LP_TERMINATE 0x1
#define LP_QH        0x2
#define LP_VF        0x4  /* depth-first (TD only) */

/* TD ctrl_status */
#define TD_ACTIVE  (1u << 23)
#define TD_IOC     (1u << 24)
#define TD_LS      (1u << 26)
#define TD_CERR3   (3u << 27)
#define TD_SPD     (1u << 29)
#define TD_ACTLEN_MASK 0x7FF

/* PIDs */
#define PID_SETUP 0x2D
#define PID_IN    0x69
#define PID_OUT   0xE1

struct uhci_td {
    volatile uint32_t link;
    volatile uint32_t ctrl_status;
    volatile uint32_t token;
    volatile uint32_t buffer;
    uint32_t pad[4]; /* pad to 32B; alignment + software use */
} __attribute__((packed));

struct uhci_qh {
    volatile uint32_t head; /* next QH */
    volatile uint32_t elem; /* first TD */
    uint32_t pad[2];
} __attribute__((packed));

/* Per-interrupt-endpoint state. */
struct uhci_ep {
    struct uhci_qh *qh;      /* VA */
    uint32_t qh_phys;
    struct uhci_td *td;      /* VA */
    uint32_t td_phys;
    void *buf;               /* VA */
    uint32_t buf_phys;
    uint8_t toggle;
    uint32_t maxlen;
    int low_speed;
    uint8_t addr;
    uint8_t ep;
    int active;
};

struct uhci {
    uint16_t io;
    uint32_t *frame;         /* VA of frame list */
    uint32_t frame_phys;
    struct uhci_qh *qh_ctrl; /* VA */
    uint32_t qh_ctrl_phys;
    struct uhci_td *ctrl_td; /* VA: pool of control TDs */
    uint32_t ctrl_td_phys;
    void *ctrl_buf;          /* VA: control data bounce */
    uint32_t ctrl_buf_phys;

    struct uhci_ep eps[4];
    size_t ep_count;
    uint8_t next_addr;
    struct usb_hcd hcd;
};

static struct uhci uhci_ctrls[2];
static size_t uhci_count;

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

static uint16_t uhci_reg(const struct uhci *u, uint16_t offset) {
    /* Probe validates io + UHCI_PORTSC2 <= UINT16_MAX. */
    return (uint16_t)((uint32_t)u->io + (uint32_t)offset);
}

static int uhci_port_reg(const struct uhci *u, int port, uint16_t *out) {
    if (!u || !out || port < 0 || port >= 2) return -1;
    uint16_t offset =
        port == 0 ? UHCI_PORTSC1 : UHCI_PORTSC2;
    *out = uhci_reg(u, offset);
    return 0;
}

/* --- TD/control helpers --- */
static uint32_t mk_token(uint8_t pid, uint8_t addr, uint8_t ep,
                         uint8_t toggle, uint32_t len) {
    uint32_t maxlen =
        len == 0U ? UINT32_C(0x7FF) : len - UINT32_C(1);
    return (uint32_t)pid | ((uint32_t)addr << 8U) |
           ((uint32_t)ep << 15U) |
           ((uint32_t)(toggle & UINT8_C(1)) << 19U) |
           (maxlen << 21U);
}

/* Run a TD chain through the control QH and wait for completion.
 * Returns 0 on success, -1 on error/timeout. */
static int uhci_td_phys_at(uint32_t base, uint32_t index, uint32_t *out) {
    if (!out) return -1;
    size_t offset = (size_t)index * sizeof(struct uhci_td);
    uint64_t phys = (uint64_t)base + (uint64_t)offset;
    if (phys > UINT32_MAX) return -1;
    *out = (uint32_t)phys;
    return 0;
}

static int uhci_run_ctrl(struct uhci *u, uint32_t first_td_phys) {
    u->qh_ctrl->elem = first_td_phys; /* not terminated, not QH */
    /* Wait until the QH element terminates (chain consumed) or a TD errors. */
    for (long s = 0; s < 50000000L; s++) {
        if (u->qh_ctrl->elem & LP_TERMINATE) break;
        /* error: scan control TD pool for a non-active stalled TD */
    }
    int rc = 0;
    if (!(u->qh_ctrl->elem & LP_TERMINATE)) rc = -1;
    u->qh_ctrl->elem = LP_TERMINATE;
    return rc;
}

static int uhci_control(struct usb_hcd *hcd, struct usb_device *dev,
                        const struct usb_setup *setup, void *data, int len) {
    struct uhci *u = hcd->priv;
    if (!u || !dev || !setup || len < 0 ||
        (size_t)len > PAGE_SIZE - UINT32_C(64))
        return -1;
    if (len > 0 && data == NULL) return -1;

    uint32_t transfer_len = (uint32_t)len;
    uint32_t ls = dev->speed == USB_SPEED_LOW ? TD_LS : 0U;
    uint8_t addr = dev->addr;
    uint32_t mp =
        dev->max_packet0 ? (uint32_t)dev->max_packet0 : UINT32_C(8);
    if (mp == 0U || mp > UINT32_C(0x800)) return -1;
    uint32_t data_tds =
        transfer_len == 0U ? 0U : (transfer_len + mp - 1U) / mp;
    if (data_tds + UINT32_C(2) > UHCI_CTRL_TD_MAX) return -1;
    int dir_in = (setup->bmRequestType & USB_DIR_IN) != 0;

    struct uhci_td *td = u->ctrl_td;
    if (!td || !u->ctrl_buf) return -1;
    uint32_t n = 0U;

    /* SETUP (DATA0). */
    memcpy(u->ctrl_buf, setup, 8U);
    td[n].ctrl_status = TD_ACTIVE | TD_CERR3 | ls;
    td[n].token =
        mk_token(UINT8_C(PID_SETUP), addr, 0U, 0U, UINT32_C(8));
    td[n].buffer = u->ctrl_buf_phys;
    n++;

    /* DATA stage (toggle starts at 1), split into max-packet chunks. */
    uint8_t toggle = UINT8_C(1);
    uint32_t remaining = transfer_len;
    uint32_t off = UINT32_C(64);
    if (!dir_in && transfer_len > 0U)
        memcpy((uint8_t *)u->ctrl_buf + off, data, (size_t)transfer_len);
    while (remaining > 0U) {
        uint32_t chunk = remaining > mp ? mp : remaining;
        td[n].ctrl_status = TD_ACTIVE | TD_CERR3 | ls | TD_SPD;
        td[n].token =
            mk_token(dir_in ? UINT8_C(PID_IN) : UINT8_C(PID_OUT),
                     addr, 0U, toggle, chunk);
        td[n].buffer = u->ctrl_buf_phys + off;
        off += chunk;
        remaining -= chunk;
        toggle ^= UINT8_C(1);
        n++;
    }

    /* STATUS (opposite direction, DATA1, IOC). */
    td[n].ctrl_status = TD_ACTIVE | TD_CERR3 | ls | TD_IOC;
    td[n].token =
        mk_token(dir_in ? UINT8_C(PID_OUT) : UINT8_C(PID_IN),
                 addr, 0U, UINT8_C(1), 0U);
    td[n].buffer = 0U;
    n++;

    /* Link the prevalidated chain depth-first. */
    for (uint32_t i = 0U; i < n; ++i) {
        if (i + 1U == n) {
            td[i].link = LP_TERMINATE;
        } else {
            uint32_t next = 0U;
            if (uhci_td_phys_at(u->ctrl_td_phys, i + 1U, &next) != 0)
                return -1;
            td[i].link = next | LP_VF;
        }
    }

    if (uhci_run_ctrl(u, u->ctrl_td_phys) < 0) return -1;

    /* Compute bytes transferred for IN: sum ActLen of data TDs. */
    uint32_t transferred = 0U;
    if (dir_in) {
        uint32_t idx = 1U;
        uint32_t rem = transfer_len;
        while (rem > 0U && idx + 1U < n) {
            uint32_t act = td[idx].ctrl_status & UINT32_C(TD_ACTLEN_MASK);
            act = act == UINT32_C(0x7FF) ? 0U : act + 1U;
            uint32_t chunk = rem > mp ? mp : rem;
            if (act > chunk) act = chunk;
            transferred += act;
            rem -= chunk;
            idx++;
        }
        if (transferred > 0U)
            memcpy(data, (uint8_t *)u->ctrl_buf + 64U,
                   (size_t)transferred);
    }
    return dir_in ? (int)transferred : len;
}

/* --- ports / enumeration --- */
static int uhci_address(struct usb_hcd *hcd, struct usb_device *dev);

static int uhci_num_ports(struct usb_hcd *hcd) { (void)hcd; return 2; }

static int uhci_port_connected(struct usb_hcd *hcd, int port) {
    struct uhci *u = hcd->priv;
    uint16_t port_reg = 0U;
    if (uhci_port_reg(u, port, &port_reg) != 0) return 0;
    uint16_t sc = hal_inw(port_reg);
    return (sc & PORT_CCS) ? 1 : 0;
}

static int uhci_enumerate_port(struct usb_hcd *hcd, int port,
                               struct usb_device *dev) {
    struct uhci *u = hcd->priv;
    uint16_t port_reg = 0U;
    if (!dev || uhci_port_reg(u, port, &port_reg) != 0) return -1;
    uint16_t sc = hal_inw(port_reg);
    if (!(sc & PORT_CCS)) return -1;

    int low_speed = (sc & PORT_LS) ? 1 : 0;

    /* Reset sequence: assert PR, hold, deassert, then enable. */
    hal_outw(port_reg, (uint16_t)(sc | PORT_PR));
    for (long s = 0; s < 5000000L; s++) { /* ~hold */ }
    sc = hal_inw(port_reg);
    hal_outw(port_reg, (uint16_t)(sc & (uint16_t)~PORT_PR));
    for (long s = 0; s < 2000000L; s++) { }
    /* enable port, clear change bits */
    hal_outw(port_reg, (uint16_t)(hal_inw(port_reg) | PORT_PED));
    for (long s = 0; s < 2000000L; s++) { }
    /* clear CSC/PEDC (write 1 to clear) */
    hal_outw(port_reg,
             (uint16_t)(hal_inw(port_reg) | PORT_CSC | PORT_PEDC));
    if (!(hal_inw(port_reg) & PORT_PED)) return -1;

    dev->speed = low_speed ? USB_SPEED_LOW : USB_SPEED_FULL;
    dev->max_packet0 = UINT16_C(8);
    return uhci_address(hcd, dev);
}

/*
 * Assign an address on the default pipe (addr 0). Used both for root-port
 * devices and hub children — for the latter the hub already reset/enabled the
 * port, so this is the only step the controller owns. Only one device may sit
 * at address 0 at a time, which the sequential enumeration guarantees.
 */
static int uhci_address(struct usb_hcd *hcd, struct usb_device *dev) {
    struct uhci *u = hcd->priv;
    dev->addr = 0;

    struct usb_device_descriptor dd;
    memset(&dd, 0, sizeof(dd));
    struct usb_setup s8 = { USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, (USB_DT_DEVICE << 8), 0, 8 };
    if (uhci_control(hcd, dev, &s8, &dd, 8) < 8) return -1;
    if (dd.bMaxPacketSize0) dev->max_packet0 = dd.bMaxPacketSize0;

    uint8_t new_addr = ++u->next_addr;
    struct usb_setup sa = { USB_DIR_OUT, USB_REQ_SET_ADDRESS, new_addr, 0, 0 };
    if (uhci_control(hcd, dev, &sa, NULL, 0) < 0) return -1;
    dev->addr = new_addr;
    for (long s = 0; s < 2000000L; s++) { } /* SET_ADDRESS recovery */
    return 0;
}

static int uhci_address_dev(struct usb_hcd *hcd, struct usb_device *dev) {
    return uhci_address(hcd, dev);
}

/* --- interrupt IN endpoint --- */
static int uhci_intr_open(struct usb_hcd *hcd, struct usb_device *dev,
                          uint8_t ep_addr, int max_packet, int interval) {
    (void)interval;
    struct uhci *u = hcd->priv;
    if (!u || !dev || max_packet <= 0 ||
        (size_t)max_packet > PAGE_SIZE ||
        (uint32_t)max_packet > UINT32_C(0x800) ||
        u->ep_count >= sizeof(u->eps) / sizeof(u->eps[0]))
        return -1;
    uint32_t max_packet32 = (uint32_t)max_packet;
    struct uhci_ep *e = &u->eps[u->ep_count];
    memset(e, 0, sizeof(*e));

    e->qh = (struct uhci_qh *)page_va(&e->qh_phys);
    e->td = (struct uhci_td *)page_va(&e->td_phys);
    e->buf = page_va(&e->buf_phys);
    if (!e->qh || !e->td || !e->buf) return -1;
    e->maxlen = max_packet32;
    e->low_speed = (dev->speed == USB_SPEED_LOW);
    e->addr = dev->addr;
    e->ep = (uint8_t)(ep_addr & UINT8_C(0x0F));
    e->toggle = 0U;
    e->active = 1;
    dev->hcd_priv = e;

    /* Arm the first IN TD. */
    e->td->ctrl_status =
        TD_ACTIVE | TD_CERR3 | TD_IOC | (e->low_speed ? TD_LS : 0U);
    e->td->token =
        mk_token(UINT8_C(PID_IN), e->addr, e->ep, e->toggle, e->maxlen);
    e->td->buffer = e->buf_phys;
    e->td->link = LP_TERMINATE;
    e->qh->elem = e->td_phys;

    /* Chain this QH at the head of the schedule, before the control QH. */
    e->qh->head = u->qh_ctrl_phys | LP_QH;
    if (u->ep_count > 0U)
        u->eps[u->ep_count - 1U].qh->head = e->qh_phys | LP_QH;
    uint32_t first = e->qh_phys | LP_QH;
    /* If this is the first interrupt EP, point the frame list at it. */
    if (u->ep_count == 0U) {
        for (uint32_t i = 0U; i < UINT32_C(1024); ++i)
            u->frame[i] = first;
    }
    u->ep_count++;
    return 0;
}

static int uhci_intr_poll(struct usb_hcd *hcd, struct usb_device *dev,
                          uint8_t ep_addr, void *buf, int len) {
    (void)hcd;
    (void)ep_addr;
    if (!dev || !buf || len <= 0) return 0;
    struct uhci_ep *e = dev->hcd_priv;
    if (!e || !e->active) return 0;
    if (e->td->ctrl_status & TD_ACTIVE) return 0; /* still pending */

    uint32_t act = e->td->ctrl_status & UINT32_C(TD_ACTLEN_MASK);
    act = act == UINT32_C(0x7FF) ? 0U : act + 1U;
    int stalled = (e->td->ctrl_status & (UINT32_C(1) << 22U)) != 0;
    uint32_t got = 0U;
    if (!stalled && act > 0U) {
        got = act;
        uint32_t capacity = (uint32_t)len;
        if (got > capacity) got = capacity;
        memcpy(buf, e->buf, (size_t)got);
        e->toggle ^= UINT8_C(1);
    }
    /* Re-arm. */
    e->td->ctrl_status =
        TD_ACTIVE | TD_CERR3 | TD_IOC | (e->low_speed ? TD_LS : 0U);
    e->td->token =
        mk_token(UINT8_C(PID_IN), e->addr, e->ep, e->toggle, e->maxlen);
    e->td->buffer = e->buf_phys;
    e->td->link = LP_TERMINATE;
    e->qh->elem = e->td_phys;
    return (int)got;
}

static struct usb_hcd_ops uhci_ops = {
    .num_ports = uhci_num_ports,
    .port_connected = uhci_port_connected,
    .enumerate_port = uhci_enumerate_port,
    .address_dev = uhci_address_dev,
    .control = uhci_control,
    .intr_open = uhci_intr_open,
    .intr_poll = uhci_intr_poll,
};

static int uhci_probe(struct hal_device *dev) {
    if (uhci_count >= sizeof(uhci_ctrls) / sizeof(uhci_ctrls[0])) return -1;
    struct uhci *u = &uhci_ctrls[uhci_count];
    memset(u, 0, sizeof(*u));

    /* BAR4 is the UHCI I/O BAR on PIIX/ICH. */
    struct nxhal_pci_bar bar4;
    if (nxhal_pci_get_bar(dev->pci_bdf, 4U, &bar4) != 0 ||
        bar4.is_io == 0U || bar4.base == 0U ||
        bar4.base > (uint64_t)UINT16_MAX - (uint64_t)UHCI_PORTSC2)
        return -1;
    u->io = (uint16_t)bar4.base;

    pr_info("UHCI: %s io=0x%04x\n", dev->name, u->io);

    /* Global + host reset. */
    hal_outw(uhci_reg(u, UHCI_USBCMD), CMD_GRESET);
    for (long s = 0; s < 2000000L; s++) { }
    hal_outw(uhci_reg(u, UHCI_USBCMD), 0U);
    hal_outw(uhci_reg(u, UHCI_USBCMD), CMD_HCRESET);
    for (long s = 0; s < 5000000L; s++)
        if (!(hal_inw(uhci_reg(u, UHCI_USBCMD)) & CMD_HCRESET)) break;

    /* Frame list + skeleton control QH + control TD pool + bounce. */
    u->frame = (uint32_t *)page_va(&u->frame_phys);
    u->qh_ctrl = (struct uhci_qh *)page_va(&u->qh_ctrl_phys);
    u->ctrl_td = (struct uhci_td *)page_va(&u->ctrl_td_phys);
    u->ctrl_buf = page_va(&u->ctrl_buf_phys);
    if (!u->frame || !u->qh_ctrl || !u->ctrl_td || !u->ctrl_buf) return -1;

    u->qh_ctrl->head = LP_TERMINATE;
    u->qh_ctrl->elem = LP_TERMINATE;
    for (uint32_t i = 0U; i < UINT32_C(1024); ++i)
        u->frame[i] = u->qh_ctrl_phys | LP_QH;

    hal_outl(uhci_reg(u, UHCI_FRBASEADD), u->frame_phys);
    hal_outw(uhci_reg(u, UHCI_FRNUM), 0U);
    hal_outb(uhci_reg(u, UHCI_SOFMOD), UINT8_C(64));
    hal_outw(uhci_reg(u, UHCI_USBINTR), 0U); /* polled, no IRQ */
    hal_outw(uhci_reg(u, UHCI_USBCMD), CMD_RUN | CMD_MAXP);

    u->hcd.name = "UHCI";
    u->hcd.ops = &uhci_ops;
    u->hcd.priv = u;
    uhci_count++;

    usb_register_hcd(&u->hcd);
    return 0;
}

static struct device_driver uhci_driver = {
    .name = "uhci",
    .vendor = DRV_ANY_ID,
    .device = DRV_ANY_ID,
    .class_code = 0x0C,
    .subclass = 0x03,
    .prog_if = 0x00,
    .flags = DEVICE_DRIVER_DMA, /* UHCI */
    .probe = uhci_probe,
};

void uhci_register(void) {
    driver_register(&uhci_driver);
}

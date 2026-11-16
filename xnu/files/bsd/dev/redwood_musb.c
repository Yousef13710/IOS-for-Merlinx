/*
 * MT6768 MUSB (0x11200000) + USB2 PHY (0x11cc0000) peripheral driver for Apple USBMux.
 * Exposes /dev/redwood-usb (EP1 Bulk OUT -> read, EP1 Bulk IN <- write) and enumerates
 * as Apple iPhone (05ac:12a8, interface class FF/FE/02) on GIC SPI 129 (MT_SYSIRQ 97).
 *
 * Boot-arg: mtk_usb=1 (enabled by default when mtk_usb is absent or != 0).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/redwood_input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge,
    void (*handler)(void *context, uint32_t intid), void *context);
extern void gic_v3_spi_mask(uint32_t intid);
extern void gic_v3_spi_unmask(uint32_t intid);
extern void redwood_keep(const char *format, ...) __printflike(1, 2);

#define MUSB_PHYS           0x11200000
#define U2PHY_PHYS          0x11cc0000
#define MUSB_GIC_INTID      (97u + 32u)

/* Mentor MUSB common registers */
#define MUSB_FADDR          0x00
#define MUSB_POWER          0x01
#define   MUSB_POWER_HSMODE   (1u << 4)
#define   MUSB_POWER_RESET    (1u << 3)
#define   MUSB_POWER_HSENAB   (1u << 5)
#define   MUSB_POWER_SOFTCONN (1u << 6)
#define MUSB_INTRTX         0x02
#define MUSB_INTRRX         0x04
#define MUSB_INTRTXE        0x06
#define MUSB_INTRRXE        0x08
#define MUSB_INTRUSB        0x0a
#define   MUSB_INTR_SUSPEND   (1u << 0)
#define   MUSB_INTR_RESUME    (1u << 1)
#define   MUSB_INTR_RESET     (1u << 2)
#define   MUSB_INTR_DISCON    (1u << 5)
#define MUSB_INTRUSBE       0x0b
#define MUSB_INDEX          0x0e
#define MUSB_FIFO(ep)       (0x20u + (ep) * 4u)
#define MUSB_DEVCTL         0x60
#define MUSB_TXFIFOSZ       0x62
#define MUSB_RXFIFOSZ       0x63
#define MUSB_TXFIFOADD      0x64
#define MUSB_RXFIFOADD      0x66

/* Flat endpoint registers (ep 0..15 at 0x100 + ep * 0x10) */
#define MUSB_EP_BASE(ep)    (0x100u + (ep) * 0x10u)
#define MUSB_TXMAXP(ep)     (MUSB_EP_BASE(ep) + 0x00u)
#define MUSB_CSR0(ep)       (MUSB_EP_BASE(ep) + 0x02u)
#define MUSB_TXCSR(ep)      (MUSB_EP_BASE(ep) + 0x02u)
#define MUSB_RXMAXP(ep)     (MUSB_EP_BASE(ep) + 0x04u)
#define MUSB_RXCSR(ep)      (MUSB_EP_BASE(ep) + 0x06u)
#define MUSB_COUNT0(ep)     (MUSB_EP_BASE(ep) + 0x08u)
#define MUSB_RXCOUNT(ep)    (MUSB_EP_BASE(ep) + 0x08u)

/* CSR0 bits */
#define MUSB_CSR0_RXPKTRDY      (1u << 0)
#define MUSB_CSR0_TXPKTRDY      (1u << 1)
#define MUSB_CSR0_SENTSTALL     (1u << 2)
#define MUSB_CSR0_DATAEND       (1u << 3)
#define MUSB_CSR0_SETUPEND      (1u << 4)
#define MUSB_CSR0_SENDSTALL     (1u << 5)
#define MUSB_CSR0_SVCRXPKTRDY   (1u << 6)
#define MUSB_CSR0_SVCSETUPEND   (1u << 7)

/* TXCSR / RXCSR bits */
#define MUSB_TXCSR_TXPKTRDY     (1u << 0)
#define MUSB_TXCSR_FIFONOTEMPTY (1u << 1)
#define MUSB_TXCSR_FLUSHFIFO    (1u << 3)
#define MUSB_TXCSR_CLRDATATOG   (1u << 6)
#define MUSB_TXCSR_MODE         (1u << 13)

#define MUSB_RXCSR_RXPKTRDY     (1u << 0)
#define MUSB_RXCSR_FLUSHFIFO    (1u << 4)
#define MUSB_RXCSR_CLRDATATOG   (1u << 7)

/* MediaTek L1 interrupt wrapper */
#define USB_L1INTS          0x00a0
#define USB_L1INTM          0x00a4
#define   L1_TX_INT           (1u << 0)
#define   L1_RX_INT           (1u << 1)
#define   L1_USBCOM_INT       (1u << 2)

/* MediaTek USB2 PHY (tphy-v2 at 0x11cc0000, u2phyd at +0x800) */
#define U2PHYDTM0           0x868
#define U2PHYDTM1           0x86c
#define   DTM0_FORCE_UART_EN      (1u << 26)
#define   DTM0_FORCE_SUSPENDM     (1u << 18)
#define   DTM0_FORCE_DP_PULLDOWN  (1u << 20)
#define   DTM0_FORCE_DM_PULLDOWN  (1u << 21)
#define   DTM0_RG_SUSPENDM        (1u << 3)
#define   DTM0_RG_DPPULLDOWN      (1u << 6)
#define   DTM0_RG_DMPULLDOWN      (1u << 7)
#define   DTM1_RG_VBUSVALID       (1u << 5)
#define   DTM1_RG_SESSEND         (1u << 4)
#define   DTM1_RG_AVALID          (1u << 2)
#define   DTM1_RG_IDDIG           (1u << 1)
#define   DTM1_FORCE_VBUSVALID    (1u << 13)
#define   DTM1_FORCE_SESSEND      (1u << 12)
#define   DTM1_FORCE_AVALID       (1u << 10)
#define   DTM1_FORCE_IDDIG        (1u << 9)

static vm_offset_t musb_base, phy_base;
static lck_grp_t *musb_lck_grp;
static lck_mtx_t *musb_mtx;

static uint8_t usb_addr, usb_pending_addr, usb_cfg;
static bool usb_hs;
static uint32_t stat_irqs, stat_resets, stat_ep0, stat_rx_pkts, stat_tx_pkts;

/* EP0 TX state */
static const uint8_t *ep0_tx_ptr;
static uint16_t ep0_tx_len;
static uint8_t ep0_tx_buf[64];

/* Bulk OUT (RX) ring buffer */
#define RX_RING_SIZE 16384
static uint8_t rx_ring[RX_RING_SIZE];
static uint32_t rx_head, rx_tail;
static int rx_wait_chan, tx_wait_chan, irq_wait_chan;
static bool tx_busy;

static inline uint8_t rd8(uint32_t off) { return *(volatile uint8_t *)(musb_base + off); }
static inline uint16_t rd16(uint32_t off) { return *(volatile uint16_t *)(musb_base + off); }
static inline uint32_t rd32(uint32_t off) { return *(volatile uint32_t *)(musb_base + off); }
static inline void wr8(uint32_t off, uint8_t v) { *(volatile uint8_t *)(musb_base + off) = v; }
static inline void wr16(uint32_t off, uint16_t v) { *(volatile uint16_t *)(musb_base + off) = v; }
static inline void wr32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(musb_base + off) = v; }

static inline uint32_t phy_rd32(uint32_t off) { return *(volatile uint32_t *)(phy_base + off); }
static inline void phy_wr32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(phy_base + off) = v; }

/* USB Descriptors: Apple iPhone (05ac:12a8) with USBMux Bulk Interface (FF/FE/02) */
static const uint8_t dev_desc[18] = {
	18, 0x01, 0x00, 0x02,       /* bLength, DEVICE, USB 2.00 */
	0x00, 0x00, 0x00, 64,       /* Class/Sub/Proto per-intf, bMaxPacketSize0=64 */
	0xac, 0x05, 0xa8, 0x12,     /* idVendor=0x05AC, idProduct=0x12A8 */
	0x05, 0x16,                 /* bcdDevice=16.05 */
	1, 2, 3,                    /* iManufacturer, iProduct, iSerialNumber */
	1                           /* bNumConfigurations */
};

static const uint8_t dev_qual_desc[10] = {
	10, 0x06, 0x00, 0x02, 0x00, 0x00, 0x00, 64, 1, 0
};

static const uint8_t cfg_desc_hs[32] = {
	/* Configuration Descriptor */
	9, 0x02, 32, 0, 1, 1, 0, 0xc0, 250,
	/* Interface 0: Apple USBMux (0xFF / 0xFE / 0x02) */
	9, 0x04, 0, 0, 2, 0xff, 0xfe, 0x02, 4,
	/* EP1 Bulk IN (512B) */
	7, 0x05, 0x81, 0x02, 0x00, 0x02, 0,
	/* EP1 Bulk OUT (512B) */
	7, 0x05, 0x01, 0x02, 0x00, 0x02, 0
};

static const uint8_t cfg_desc_fs[32] = {
	9, 0x02, 32, 0, 1, 1, 0, 0xc0, 250,
	9, 0x04, 0, 0, 2, 0xff, 0xfe, 0x02, 4,
	7, 0x05, 0x81, 0x02, 64, 0, 0,
	7, 0x05, 0x01, 0x02, 64, 0, 0
};

static const char *const usb_strings[5] = {
	"",
	"Apple Inc.",
	"iPhone",
	"000080200011223344556677",
	"AppleUSBMux"
};

static void
fifo_write(uint32_t ep, const uint8_t *buf, uint16_t len)
{
	uint32_t fifo = MUSB_FIFO(ep);
	uint16_t i = 0;

	while (i + 4 <= len) {
		uint32_t w = (uint32_t)buf[i] | ((uint32_t)buf[i + 1] << 8) |
		    ((uint32_t)buf[i + 2] << 16) | ((uint32_t)buf[i + 3] << 24);
		wr32(fifo, w);
		i += 4;
	}
	while (i < len) {
		wr8(fifo, buf[i++]);
	}
}

static void
fifo_read(uint32_t ep, uint8_t *buf, uint16_t len)
{
	uint32_t fifo = MUSB_FIFO(ep);
	uint16_t i = 0;

	while (i + 4 <= len) {
		uint32_t w = rd32(fifo);
		buf[i] = (uint8_t)(w & 0xff);
		buf[i + 1] = (uint8_t)((w >> 8) & 0xff);
		buf[i + 2] = (uint8_t)((w >> 16) & 0xff);
		buf[i + 3] = (uint8_t)((w >> 24) & 0xff);
		i += 4;
	}
	while (i < len) {
		buf[i++] = rd8(fifo);
	}
}

static void
ep0_send_chunk(void)
{
	uint16_t chunk = ep0_tx_len > 64 ? 64 : ep0_tx_len;
	uint16_t csr = MUSB_CSR0_TXPKTRDY;

	if (chunk > 0) {
		fifo_write(0, ep0_tx_ptr, chunk);
		ep0_tx_ptr += chunk;
		ep0_tx_len -= chunk;
	}
	if (ep0_tx_len == 0) {
		csr |= MUSB_CSR0_DATAEND;
		ep0_tx_ptr = NULL;
	}
	wr16(MUSB_CSR0(0), csr);
}

static void
ep1_configure(void)
{
	uint16_t mps = usb_hs ? 512 : 64;
	uint8_t sz = usb_hs ? 6 : 3; /* 2^(sz+3): 6->512B, 3->64B */

	wr8(MUSB_INDEX, 1);
	wr8(MUSB_TXFIFOSZ, sz);
	wr16(MUSB_TXFIFOADD, 8); /* after EP0 64B (8 * 8B) */
	wr8(MUSB_RXFIFOSZ, sz);
	wr16(MUSB_RXFIFOADD, 8 + (mps >> 3));

	wr16(MUSB_TXMAXP(1), mps);
	wr16(MUSB_TXCSR(1), MUSB_TXCSR_MODE | MUSB_TXCSR_CLRDATATOG | MUSB_TXCSR_FLUSHFIFO);
	wr16(MUSB_RXMAXP(1), mps);
	wr16(MUSB_RXCSR(1), MUSB_RXCSR_CLRDATATOG | MUSB_RXCSR_FLUSHFIFO);
}

static uint16_t
build_string_desc(uint8_t idx, uint8_t *out)
{
	if (idx == 0) {
		out[0] = 4; out[1] = 0x03; out[2] = 0x09; out[3] = 0x04;
		return 4;
	}
	if (idx >= 5) {
		return 0;
	}
	const char *s = usb_strings[idx];
	size_t n = strlen(s);
	if (n > 30) n = 30;
	out[0] = (uint8_t)(2 + n * 2);
	out[1] = 0x03;
	for (size_t i = 0; i < n; i++) {
		out[2 + i * 2] = (uint8_t)s[i];
		out[2 + i * 2 + 1] = 0;
	}
	return (uint16_t)out[0];
}

static void
handle_ep0(void)
{
	uint16_t csr = rd16(MUSB_CSR0(0));

	if (csr & MUSB_CSR0_SENTSTALL) {
		wr16(MUSB_CSR0(0), csr & ~MUSB_CSR0_SENTSTALL);
	}
	if (csr & MUSB_CSR0_SETUPEND) {
		wr16(MUSB_CSR0(0), MUSB_CSR0_SVCSETUPEND);
		ep0_tx_ptr = NULL;
		ep0_tx_len = 0;
	}
	if (usb_pending_addr != 0 && !(csr & MUSB_CSR0_RXPKTRDY) && ep0_tx_ptr == NULL) {
		usb_addr = usb_pending_addr;
		usb_pending_addr = 0;
		wr8(MUSB_FADDR, usb_addr);
	}
	if (ep0_tx_ptr != NULL && !(csr & MUSB_CSR0_TXPKTRDY)) {
		ep0_send_chunk();
		return;
	}
	if (!(csr & MUSB_CSR0_RXPKTRDY)) {
		return;
	}
	uint16_t cnt = rd16(MUSB_COUNT0(0)) & 0x7f;
	if (cnt < 8) {
		wr16(MUSB_CSR0(0), MUSB_CSR0_SVCRXPKTRDY | MUSB_CSR0_DATAEND);
		return;
	}
	uint8_t setup[8];
	fifo_read(0, setup, 8);
	stat_ep0++;

	uint8_t bmReq = setup[0];
	uint8_t bReq = setup[1];
	uint16_t wVal = (uint16_t)(setup[2] | (setup[3] << 8));
	uint16_t wLen = (uint16_t)(setup[6] | (setup[7] << 8));

	const uint8_t *resp = NULL;
	uint16_t resp_len = 0;
	bool stall = false;

	if ((bmReq & 0x60) == 0) { /* Standard request */
		switch (bReq) {
		case 0x00: /* GET_STATUS */
			ep0_tx_buf[0] = (bmReq & 0x1f) == 0 ? 1 : 0; /* self-powered */
			ep0_tx_buf[1] = 0;
			resp = ep0_tx_buf;
			resp_len = 2;
			break;
		case 0x01: /* CLEAR_FEATURE */
		case 0x03: /* SET_FEATURE */
			break;
		case 0x05: /* SET_ADDRESS */
			usb_pending_addr = (uint8_t)(wVal & 0x7f);
			break;
		case 0x06: { /* GET_DESCRIPTOR */
			uint8_t dtype = (uint8_t)(wVal >> 8);
			uint8_t didx = (uint8_t)(wVal & 0xff);
			if (dtype == 1) {
				resp = dev_desc;
				resp_len = sizeof(dev_desc);
			} else if (dtype == 2) {
				resp = usb_hs ? cfg_desc_hs : cfg_desc_fs;
				resp_len = sizeof(cfg_desc_hs);
			} else if (dtype == 3) {
				resp_len = build_string_desc(didx, ep0_tx_buf);
				if (resp_len > 0) resp = ep0_tx_buf;
				else stall = true;
			} else if (dtype == 6) {
				resp = dev_qual_desc;
				resp_len = sizeof(dev_qual_desc);
			} else if (dtype == 7) {
				resp = usb_hs ? cfg_desc_fs : cfg_desc_hs;
				resp_len = sizeof(cfg_desc_hs);
			} else {
				stall = true;
			}
			break;
		}
		case 0x08: /* GET_CONFIGURATION */
			ep0_tx_buf[0] = usb_cfg;
			resp = ep0_tx_buf;
			resp_len = 1;
			break;
		case 0x09: /* SET_CONFIGURATION */
			usb_cfg = (uint8_t)(wVal & 0xff);
			if (usb_cfg != 0) {
				ep1_configure();
				wakeup(&tx_wait_chan);
			}
			break;
		default:
			stall = true;
			break;
		}
	} else {
		stall = true;
	}

	if (stall) {
		wr16(MUSB_CSR0(0), MUSB_CSR0_SVCRXPKTRDY | MUSB_CSR0_SENDSTALL);
		return;
	}
	if ((bmReq & 0x80) && wLen > 0 && resp != NULL) {
		wr16(MUSB_CSR0(0), MUSB_CSR0_SVCRXPKTRDY);
		ep0_tx_ptr = resp;
		ep0_tx_len = resp_len < wLen ? resp_len : wLen;
		ep0_send_chunk();
	} else {
		wr16(MUSB_CSR0(0), MUSB_CSR0_SVCRXPKTRDY | MUSB_CSR0_DATAEND);
	}
}

static void
handle_ep1_rx(void)
{
	uint16_t csr = rd16(MUSB_RXCSR(1));
	if (!(csr & MUSB_RXCSR_RXPKTRDY)) {
		return;
	}
	uint16_t cnt = rd16(MUSB_RXCOUNT(1)) & 0x1fff;
	uint8_t tmp[512];
	uint16_t read_len = cnt > sizeof(tmp) ? sizeof(tmp) : cnt;
	fifo_read(1, tmp, read_len);
	wr16(MUSB_RXCSR(1), csr & ~MUSB_RXCSR_RXPKTRDY);
	stat_rx_pkts++;

	for (uint16_t i = 0; i < read_len; i++) {
		uint32_t next = (rx_head + 1) % RX_RING_SIZE;
		if (next != rx_tail) {
			rx_ring[rx_head] = tmp[i];
			rx_head = next;
		}
	}
	wakeup(&rx_wait_chan);
}

static void
musb_service(void)
{
	lck_mtx_lock(musb_mtx);
	uint32_t l1 = rd32(USB_L1INTS);
	uint8_t intrusb = rd8(MUSB_INTRUSB);
	uint16_t intrtx = rd16(MUSB_INTRTX);
	uint16_t intrrx = rd16(MUSB_INTRRX);

	if (intrusb) wr8(MUSB_INTRUSB, intrusb);
	if (intrtx) wr16(MUSB_INTRTX, intrtx);
	if (intrrx) wr16(MUSB_INTRRX, intrrx);

	if (intrusb & MUSB_INTR_RESET) {
		stat_resets++;
		usb_addr = 0;
		usb_pending_addr = 0;
		usb_cfg = 0;
		ep0_tx_ptr = NULL;
		ep0_tx_len = 0;
		tx_busy = false;
		rx_head = rx_tail = 0;
		wr8(MUSB_FADDR, 0);
		usb_hs = (rd8(MUSB_POWER) & MUSB_POWER_HSMODE) != 0;
		ep1_configure();
		wakeup(&tx_wait_chan);
	}
	if ((intrtx & (1u << 0)) || (rd16(MUSB_CSR0(0)) & MUSB_CSR0_RXPKTRDY)) {
		handle_ep0();
	}
	if ((intrrx & (1u << 1)) || (rd16(MUSB_RXCSR(1)) & MUSB_RXCSR_RXPKTRDY)) {
		handle_ep1_rx();
	}
	if (usb_cfg != 0 && !(rd16(MUSB_TXCSR(1)) & MUSB_TXCSR_TXPKTRDY)) {
		tx_busy = false;
		wakeup(&tx_wait_chan);
	}
	(void)l1;
	lck_mtx_unlock(musb_mtx);
}

static void
musb_irq(__unused void *context, __unused uint32_t intid)
{
	stat_irqs++;
	gic_v3_spi_mask(MUSB_GIC_INTID);
	thread_wakeup((event_t)&irq_wait_chan);
}

static void
musb_poll_thread(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		musb_service();
		gic_v3_spi_unmask(MUSB_GIC_INTID);
		assert_wait_timeout((event_t)&irq_wait_chan, THREAD_UNINT, usb_cfg ? 5 : 20, NSEC_PER_MSEC);
		thread_block(THREAD_CONTINUE_NULL);
	}
}

static int
usb_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	lck_mtx_lock(musb_mtx);
	if (!(rd8(MUSB_POWER) & MUSB_POWER_SOFTCONN)) {
		wr8(MUSB_POWER, MUSB_POWER_HSENAB | MUSB_POWER_SOFTCONN);
	}
	lck_mtx_unlock(musb_mtx);
	return 0;
}

static int
usb_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
usb_read(__unused dev_t dev, struct uio *uio, int ioflag)
{
	int err = 0;
	uint8_t buf[512];
	uint32_t got = 0;

	lck_mtx_lock(musb_mtx);
	while (rx_head == rx_tail) {
		if (ioflag & IO_NDELAY) {
			lck_mtx_unlock(musb_mtx);
			return EWOULDBLOCK;
		}
		err = msleep(&rx_wait_chan, musb_mtx, PCATCH | PSOCK, "usbread", NULL);
		if (err != 0) {
			lck_mtx_unlock(musb_mtx);
			return err;
		}
	}
	user_ssize_t want = uio_resid(uio);
	if (want > (user_ssize_t)sizeof(buf)) want = sizeof(buf);
	while (got < (uint32_t)want && rx_head != rx_tail) {
		buf[got++] = rx_ring[rx_tail];
		rx_tail = (rx_tail + 1) % RX_RING_SIZE;
	}
	lck_mtx_unlock(musb_mtx);
	return uiomove((caddr_t)buf, (int)got, uio);
}

static int
usb_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint8_t buf[512];
	int err = 0;

	while (uio_resid(uio) > 0) {
		uint16_t mps = usb_hs ? 512 : 64;
		user_ssize_t chunk = uio_resid(uio);
		if (chunk > mps) chunk = mps;
		err = uiomove((caddr_t)buf, (int)chunk, uio);
		if (err != 0) break;

		lck_mtx_lock(musb_mtx);
		int waits = 0;
		while (usb_cfg == 0 || (rd16(MUSB_TXCSR(1)) & MUSB_TXCSR_TXPKTRDY)) {
			struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * NSEC_PER_MSEC };
			err = msleep(&tx_wait_chan, musb_mtx, PCATCH | PSOCK, "usbwrite", &ts);
			if (err != 0 && err != EWOULDBLOCK) {
				lck_mtx_unlock(musb_mtx);
				return err;
			}
			if (err == EWOULDBLOCK && ++waits >= 20) {
				lck_mtx_unlock(musb_mtx);
				return ETIMEDOUT;
			}
			err = 0;
		}
		fifo_write(1, buf, (uint16_t)chunk);
		wr16(MUSB_TXCSR(1), MUSB_TXCSR_MODE | MUSB_TXCSR_TXPKTRDY);
		stat_tx_pkts++;
		if (uio_resid(uio) == 0 && chunk == mps) {
			int zwaits = 0;
			while (usb_cfg != 0 && (rd16(MUSB_TXCSR(1)) & MUSB_TXCSR_TXPKTRDY)) {
				struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * NSEC_PER_MSEC };
				int zerr = msleep(&tx_wait_chan, musb_mtx, PCATCH | PSOCK, "usbzlp", &ts);
				if (zerr != 0 && zerr != EWOULDBLOCK) break;
				if (zerr == EWOULDBLOCK && ++zwaits >= 50) break;
			}
			if (usb_cfg != 0 && !(rd16(MUSB_TXCSR(1)) & MUSB_TXCSR_TXPKTRDY)) {
				wr16(MUSB_TXCSR(1), MUSB_TXCSR_MODE | MUSB_TXCSR_TXPKTRDY);
				stat_tx_pkts++;
			}
		}
		lck_mtx_unlock(musb_mtx);
	}
	return err;
}

static const struct cdevsw usb_cdevsw = {
	.d_open = usb_open, .d_close = usb_close, .d_read = usb_read, .d_write = usb_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_ttys = NULL,
	.d_select = eno_select, .d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

SYSCTL_DECL(_kern_redwood);

static int
usb_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char buf[160];
	int n = snprintf(buf, sizeof(buf),
	    "addr=%u cfg=%u hs=%d pwr=%#x devctl=%#x irqs=%u rst=%u ep0=%u rx=%u tx=%u",
	    usb_addr, usb_cfg, usb_hs, musb_base ? rd8(MUSB_POWER) : 0,
	    musb_base ? rd8(MUSB_DEVCTL) : 0, stat_irqs, stat_resets, stat_ep0, stat_rx_pkts, stat_tx_pkts);
	if (n < 0) n = 0;
	if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
	return SYSCTL_OUT(req, buf, (size_t)n + 1);
}

SYSCTL_PROC(_kern_redwood, OID_AUTO, usb, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, NULL, 0,
    usb_sysctl, "A", "MT6768 MUSB peripheral status");

void
redwood_musb_init(void)
{
	uint32_t en = 1;

	if (PE_parse_boot_argn("mtk_usb", &en, sizeof(en)) && en == 0) {
		return;
	}
	musb_base = ml_io_map(MUSB_PHYS, 0x1000);
	phy_base = ml_io_map(U2PHY_PHYS, 0x1000);
	if (musb_base == 0 || phy_base == 0) {
		printf("musb: ml_io_map failed\n");
		return;
	}
	musb_lck_grp = lck_grp_alloc_init("redwood-usb", LCK_GRP_ATTR_NULL);
	musb_mtx = lck_mtx_alloc_init(musb_lck_grp, LCK_ATTR_NULL);

	int major = cdevsw_add(-1, &usb_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "redwood-usb") == NULL) {
		printf("musb: cannot create /dev/redwood-usb\n");
	}

	/* Detach first, force B-device (peripheral) mode on USB2 PHY, clear pulldowns */
	wr8(MUSB_POWER, MUSB_POWER_HSENAB);
	uint32_t dtm0 = phy_rd32(U2PHYDTM0);
	dtm0 &= ~(DTM0_FORCE_UART_EN | DTM0_FORCE_SUSPENDM | DTM0_RG_DPPULLDOWN | DTM0_RG_DMPULLDOWN);
	dtm0 |= DTM0_FORCE_DP_PULLDOWN | DTM0_FORCE_DM_PULLDOWN | DTM0_RG_SUSPENDM;
	phy_wr32(U2PHYDTM0, dtm0);

	uint32_t dtm1 = phy_rd32(U2PHYDTM1);
	dtm1 &= ~DTM1_RG_SESSEND;
	dtm1 |= DTM1_FORCE_VBUSVALID | DTM1_FORCE_AVALID | DTM1_FORCE_SESSEND | DTM1_FORCE_IDDIG |
	    DTM1_RG_VBUSVALID | DTM1_RG_AVALID | DTM1_RG_IDDIG;
	phy_wr32(U2PHYDTM1, dtm1);
	delay(20000);

	/* Clear pending interrupts and enable EP0, EP1, and USB common interrupts */
	wr8(MUSB_INTRUSB, 0xff);
	wr16(MUSB_INTRTX, 0xffff);
	wr16(MUSB_INTRRX, 0xffff);
	wr8(MUSB_INTRUSBE, MUSB_INTR_RESET | MUSB_INTR_SUSPEND | MUSB_INTR_RESUME | MUSB_INTR_DISCON);
	wr16(MUSB_INTRTXE, (1u << 0) | (1u << 1));
	wr16(MUSB_INTRRXE, (1u << 1));
	wr32(USB_L1INTM, L1_TX_INT | L1_RX_INT | L1_USBCOM_INT);

	if (gic_v3_spi_attach(MUSB_GIC_INTID, false, musb_irq, NULL) == KERN_SUCCESS) {
		gic_v3_spi_unmask(MUSB_GIC_INTID);
	} else {
		printf("musb: gic_v3_spi_attach(%u) failed, using poll thread\n", MUSB_GIC_INTID);
	}

	/* Keep D+ disconnected until /dev/redwood-usb is opened by usbmux_relay */
	redwood_keep("musb: ready (pwr=%#x devctl=%#x dtm0=%#x dtm1=%#x)",
	    rd8(MUSB_POWER), rd8(MUSB_DEVCTL), phy_rd32(U2PHYDTM0), phy_rd32(U2PHYDTM1));

	thread_t th;
	if (kernel_thread_start(musb_poll_thread, NULL, &th) == KERN_SUCCESS) {
		thread_deallocate(th);
	}
}

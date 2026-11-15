/*
 * A camera on a MediaTek phone: the skeleton. It is the part of redwood_mtkcam.c (the Infinix Hot 20's working driver, MT6768 + Samsung S5K JN1,
 * which has the same node and the same ABI) that does not depend on the phone: /dev/redwood-camera for iOS (sys/redwood_camera.h, RWFRAME1), a
 * session that starts when something reads and ends when nothing has for a while, the newest-frame handover, the EXPOSURE command. What is
 * different on every phone -- the rails and clocks, the sensor and its tables, the CSI receiver and the DMA that writes the frames -- is a table of
 * operations, struct mtkcam_platform, that a port fills in. Nothing here knows a register.
 *
 * It does nothing until it is told to (boot-arg mtkcam_skel=1) AND the platform is complete: every field the driver needs that is still missing
 * is said (redwood_host_keep_add) and the node is not made. There are no default values for what only the phone can tell.
 *
 * Not in bsd/conf/files, and not to be built together with redwood_mtkcam.c (both make /dev/redwood-camera). To try it: add
 *     bsd/dev/redwood_mtkcam_skel.c   standard
 * to bsd/conf/files, call redwood_mtkcam_skel_init() where redwood_mtkcam_init() is called (redwood_input.c), and put your platform in
 * mtkcam_platform below. docs/camera-mtk-porting.md has the order to find things out in and the tools for it; the proven sequence for one phone is
 * redwood_mtkcam.c and mtkloader/mtkcam.h of this tree (docs/camera-x6826b.md).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/proc.h>
#include <sys/redwood_camera.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>

extern void redwood_host_keep_add(const char *format, ...) __printflike(1, 2);
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority, thread_t *new_thread);
extern kern_return_t thread_terminate(thread_t thread);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void delay(int usec);
extern void redwood_pwrap_lock(void);
extern void redwood_pwrap_unlock(void);
extern uint64_t kvtophys(vm_offset_t va);

#include "redwood_mtkcam_gm1sp.h"

#define say(...) redwood_host_keep_add("mtkcam-skel: " __VA_ARGS__)

#define SKEL_MAX_BUFFERS        4u
#define SKEL_MAX_STRIDE         8192u           /* of the RAW10 lines handed out: 4 pixels in 5 bytes, 6528 pixels at most */
#define SKEL_CHUNK_LINES        8u
#define SKEL_IDLE_STOP_MS       4000u           /* the session ends when nothing has read for this long */

/* What one frame the hardware has finished says about itself. */
struct mtkcam_frame_meta {
	uint32_t        exposure_lines;         /* the sensor's exposure it was taken with */
	uint32_t        gain;                   /* the analog gain register's value it was taken with */
	uint32_t        frame_lines;            /* the frame length in lines it was taken with */
};

/* What the frames are. Zero is "not known": a port that does not know a field leaves it zero and the driver refuses to start. */
struct mtkcam_geometry {
	uint32_t        width, height;          /* pixels (height even: whole Bayer cells) */
	uint32_t        bayer;                  /* the order of a 2x2 cell: 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR (sys/redwood_camera.h) */
	uint32_t        black;                  /* the black level in 10-bit steps */
	uint32_t        line_ns;                /* one sensor line's time */
	uint32_t        gain_unity, gain_max;   /* the gain register's value for 1x and the most it may be given */
	uint32_t        frame_lines_min, frame_lines_max, margin;       /* the mode's own frame length, the longest, and exposure_lines <= frame_lines - margin */
	uint32_t        nbuf;                   /* frame buffers the DMA cycles through (2..SKEL_MAX_BUFFERS) */
};

/*
 * The operations of one phone. All of them are required (the skeleton checks); each returns 0 or an errno, and says (say()) what it is about to
 * do BEFORE a step that could hang a bus: the last line of a log that ends is the step that hung.
 */
struct mtkcam_platform {
	const char              *name;          /* "mt6768 / x6826b / s5k-jn1": what the log calls it */
	struct mtkcam_geometry  geometry;
	/* rails, the camera power domain and its bus protection, clocks, reset and MCLK pins. Undone by power_off. On a phone whose PMIC is
	 * reached through a wrapper shared with other drivers (MT6358 here), take that driver lock around every transaction (redwood_pwrap_lock). */
	int     (*power_on)(void);
	void    (*power_off)(void);
	/* the sensor: chip ID read and checked, its table written, streaming on; and off again (sensor_stop must leave the bus idle) */
	int     (*sensor_start)(void);
	void    (*sensor_stop)(void);
	/* the CSI-2 receiver and the DMA: set up, buffers queued, running; and stopped (the DMA must be quiet when this returns) */
	int     (*rx_start)(void);
	void    (*rx_stop)(void);
	/* the buffer (0..nbuf-1) of a finished frame, or -1 if none is done yet; called in a loop with a short sleep between, from the session's thread */
	int     (*frame_done)(struct mtkcam_frame_meta *meta);
	/* `lines` lines from `first` of buffer `buf`, as MIPI RAW10 (4 pixels in 5 bytes, stride = width * 5 / 4), into dst. A DMA that writes another
	 * layout is converted here (the MT6768's writes a byte a pixel and is widened here); the cache is invalidated here if the buffer is cached. */
	void    (*read_lines)(unsigned buf, uint32_t first, uint32_t lines, uint8_t *dst);
	/* the buffer is the hardware's again */
	void    (*frame_release)(unsigned buf);
	/* the sensor's exposure, gain and frame length for the frames to come (frame_lines 0: the shortest that holds the exposure) */
	int     (*set_exposure)(uint32_t lines, uint32_t gain, uint32_t frame_lines);
};

/*
 * ---- THE PORT: fill this in ----
 * Nothing is assumed. For the Infinix Hot 20 the values are in redwood_mtkcam.c (CAM_PITCH, CAM_LINES, CAM_BAYER, CAM_BLACK10, CAM_GAIN_UNITY, ...)
 * and each one says there how it was found.
 */
static int m_power_on(void);
static void m_power_off(void);
static int m_sensor_start(void);
static void m_sensor_stop(void);
static int m_rx_start(void);
static void m_rx_stop(void);
static int m_frame_done(struct mtkcam_frame_meta *meta);
static void m_read_lines(unsigned buf, uint32_t first, uint32_t lines, uint8_t *dst);
static void m_frame_release(unsigned buf);
static int m_set_exposure(uint32_t lines, uint32_t gain, uint32_t frame_lines);

static const struct mtkcam_platform mtkcam_platform = {
	.name = "mt6768 / merlinx / s5k-gm1sp",
	.geometry = {
		.width = 4000,
		.height = 3000,
		.bayer = 1,             /* GRBG (output_format=2 Gr in /proc/driver/camera_info) */
		.black = 64,
		.line_ns = 10467,       /* 5024 / 480 MHz */
		.gain_unity = 0x100,
		.gain_max = 0x1000,
		.frame_lines_min = 0x0c7a,
		.frame_lines_max = 0xfff0,
		.margin = 8,
		.nbuf = 3,
	},
	.power_on = m_power_on,
	.power_off = m_power_off,
	.sensor_start = m_sensor_start,
	.sensor_stop = m_sensor_stop,
	.rx_start = m_rx_start,
	.rx_stop = m_rx_stop,
	.frame_done = m_frame_done,
	.read_lines = m_read_lines,
	.frame_release = m_frame_release,
	.set_exposure = m_set_exposure,
};

/* ---- merlinx (Redmi Note 9, MT6768, S5KGM1SP on I2C2) hardware ---- */
#define M_NBUF          3u
#define M_W             4000u
#define M_H             3000u
#define M_BUF_BYTES     (M_W * M_H)     /* 12,000,000 bytes (8-bit top-8 DMA) */
#define M_HDR_BYTES     4096u
#define M_SENSOR_ADDR7  0x10u

#define M_PWRAP_OFF     0xd000ULL
#define M_WACS2_CMD     0xc20
#define M_WACS2_RDATA   0xc24
#define M_WACS2_VLDCLR  0xc28
#define M_GPIO_OFF      0x5000ULL
#define M_VCAMIO_EN     0x1cb0
#define M_PIN_AVDD      178
#define M_PIN_DVDD      179
#define M_PIN_RST       102
#define M_PIN_MCLK      100
#define M_PIN_SCL       103
#define M_PIN_SDA       104

static struct {
	uint64_t pa;
	vm_size_t sz;
	vm_offset_t va;
} m_win[] = {
	{ 0x10000000ULL, 0x10000, 0 },  /* topckgen, infracfg, GPIO, SPM, APMIXED, pwrap */
	{ 0x11c10000ULL, 0x6000, 0 },   /* MIPI RX */
	{ 0x14000000ULL, 0x3000, 0 },   /* mmsys, smi_common */
	{ 0x1a000000ULL, 0x53000, 0 },  /* camsys, smi_larb3, SENINF, CAMSV */
};

static vm_offset_t m_img_va[M_NBUF], m_hdr_va[M_NBUF];
static uint32_t m_img_pa[M_NBUF], m_hdr_pa[M_NBUF];
static bool m_bufs_ok, m_domain_by_us, m_have_io;
static uint16_t m_vcamio_orig;
static uint32_t m_cfg1_orig, m_cfg6_orig, m_cfg7_orig, m_mm_cg_orig, m_apmix_orig;
static uint32_t m_scl_mode, m_sda_mode;
static uint32_t m_cur_exp = 0x0400, m_cur_gain = 0x0100, m_cur_fll = 0x0c7a;
static unsigned m_poll_idx;

static volatile uint32_t *m_reg(uint64_t pa) {
	for (unsigned i = 0; i < 4; i++) {
		if (pa >= m_win[i].pa && pa + 4 <= m_win[i].pa + m_win[i].sz && m_win[i].va) {
			return (volatile uint32_t *)(m_win[i].va + (pa - m_win[i].pa));
		}
	}
	return NULL;
}
static uint32_t pa_rd32(uint64_t pa) {
	volatile uint32_t *p = m_reg(pa);
	return p ? *p : 0;
}
static void pa_wr32(uint64_t pa, uint32_t v) {
	volatile uint32_t *p = m_reg(pa);
	if (p) *p = v;
}
static inline uint32_t m_rd32(vm_offset_t base, uint32_t off) { return *(volatile uint32_t *)(base + off); }
static inline void m_wr32(vm_offset_t base, uint32_t off, uint32_t v) { *(volatile uint32_t *)(base + off) = v; }
static inline void m_dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static void m_inval(volatile void *base, uint64_t n) {
	for (uint64_t a = (uint64_t)base & ~63ULL; a < (uint64_t)base + n; a += 64) {
		__asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
	}
	m_dsb();
}

static bool m_poll(uint64_t pa, uint32_t mask, uint32_t want, uint32_t ms) {
	for (uint32_t i = 0; i < ms * 10; i++) {
		if ((pa_rd32(pa) & mask) == want) return true;
		delay(100);
	}
	return false;
}

static bool m_pwrap_wait(vm_offset_t pw, uint32_t st) {
	for (uint32_t i = 0; i < 200000; i++) {
		if (((m_rd32(pw, M_WACS2_RDATA) >> 16) & 7u) == st) return true;
	}
	return false;
}
static bool m_pwrap_rd(vm_offset_t pw, uint32_t reg, uint16_t *val) {
	bool ok = false;
	redwood_pwrap_lock();
	if (m_pwrap_wait(pw, 0)) {
		m_wr32(pw, M_WACS2_CMD, (reg >> 1) << 16);
		if (m_pwrap_wait(pw, 6)) { *val = (uint16_t)m_rd32(pw, M_WACS2_RDATA); m_wr32(pw, M_WACS2_VLDCLR, 1); ok = true; }
	}
	redwood_pwrap_unlock();
	return ok;
}
static bool m_pwrap_wr(vm_offset_t pw, uint32_t reg, uint16_t val) {
	bool ok = false;
	redwood_pwrap_lock();
	if (m_pwrap_wait(pw, 0)) {
		m_wr32(pw, M_WACS2_CMD, (1u << 31) | ((reg >> 1) << 16) | val);
		ok = m_pwrap_wait(pw, 0);
	}
	redwood_pwrap_unlock();
	return ok;
}

static void m_pin_mode(vm_offset_t gp, uint32_t pin, uint32_t mode) {
	uint32_t off = 0x300 + 0x10 * (pin / 8), sh = 4 * (pin % 8);
	m_wr32(gp, off, (m_rd32(gp, off) & ~(0xfu << sh)) | (mode << sh));
}
static void m_pin_out(vm_offset_t gp, uint32_t pin, int level) {
	uint32_t bank = pin / 32, bit = 1u << (pin % 32);
	m_wr32(gp, (level ? 0x104 : 0x108) + 0x10 * bank, bit);
	m_wr32(gp, 0x004 + 0x10 * bank, bit);
}
static int m_pin_in(vm_offset_t gp, uint32_t pin) {
	return (m_rd32(gp, 0x200 + 0x10 * (pin / 32)) >> (pin % 32)) & 1u;
}
static void m_bb_line(vm_offset_t gp, uint32_t pin, int high) {
	uint32_t bank = pin / 32, bit = 1u << (pin % 32);
	if (high) m_wr32(gp, 0x008 + 0x10 * bank, bit);
	else { m_wr32(gp, 0x108 + 0x10 * bank, bit); m_wr32(gp, 0x004 + 0x10 * bank, bit); }
}
static void m_bb_scl(vm_offset_t gp, int high) {
	m_bb_line(gp, M_PIN_SCL, high);
	if (high) for (int i = 0; i < 200 && !m_pin_in(gp, M_PIN_SCL); i++) delay(5);
	delay(4);
}
static void m_bb_sda(vm_offset_t gp, int high) { m_bb_line(gp, M_PIN_SDA, high); delay(4); }
static void m_bb_start(vm_offset_t gp) { m_bb_sda(gp, 1); m_bb_scl(gp, 1); m_bb_sda(gp, 0); m_bb_scl(gp, 0); }
static void m_bb_stop(vm_offset_t gp)  { m_bb_sda(gp, 0); m_bb_scl(gp, 1); m_bb_sda(gp, 1); }
static int m_bb_wr(vm_offset_t gp, uint8_t b) {
	for (int i = 7; i >= 0; i--) { m_bb_sda(gp, (b >> i) & 1); m_bb_scl(gp, 1); m_bb_scl(gp, 0); }
	m_bb_sda(gp, 1); m_bb_scl(gp, 1);
	int ack = !m_pin_in(gp, M_PIN_SDA);
	m_bb_scl(gp, 0);
	return ack;
}
static uint8_t m_bb_rd(vm_offset_t gp, int ack) {
	uint8_t b = 0;
	m_bb_sda(gp, 1);
	for (int i = 0; i < 8; i++) { m_bb_scl(gp, 1); b = (uint8_t)((b << 1) | (m_pin_in(gp, M_PIN_SDA) ? 1 : 0)); m_bb_scl(gp, 0); }
	m_bb_sda(gp, !ack); m_bb_scl(gp, 1); m_bb_scl(gp, 0); m_bb_sda(gp, 1);
	return b;
}
static int m_bb_read8(vm_offset_t gp, uint32_t addr7, uint16_t reg, uint8_t *val) {
	int missed = 0;
	m_bb_start(gp);
	missed |= !m_bb_wr(gp, (uint8_t)(addr7 << 1)) << 0;
	missed |= !m_bb_wr(gp, (uint8_t)(reg >> 8)) << 1;
	missed |= !m_bb_wr(gp, (uint8_t)(reg & 0xffu)) << 2;
	m_bb_start(gp);
	missed |= !m_bb_wr(gp, (uint8_t)((addr7 << 1) | 1)) << 3;
	*val = m_bb_rd(gp, 0);
	m_bb_stop(gp);
	return missed;
}
static bool m_bb_write16(vm_offset_t gp, uint32_t addr7, uint16_t reg, uint16_t val) {
	int ok;
	m_bb_start(gp);
	ok = m_bb_wr(gp, (uint8_t)(addr7 << 1));
	ok &= m_bb_wr(gp, (uint8_t)(reg >> 8));
	ok &= m_bb_wr(gp, (uint8_t)(reg & 0xffu));
	ok &= m_bb_wr(gp, (uint8_t)(val >> 8));
	ok &= m_bb_wr(gp, (uint8_t)(val & 0xffu));
	m_bb_stop(gp);
	return ok != 0;
}

static bool m_alloc_bufs(void) {
	if (m_bufs_ok) return true;
	for (unsigned k = 0; k < M_NBUF; k++) {
		if (kmem_alloc_contig(kernel_map, &m_img_va[k], M_BUF_BYTES, PAGE_MASK,
		    (ppnum_t)(0x100000000ULL >> PAGE_SHIFT), 0,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			say("kmem_alloc_contig img[%u] failed", k);
			return false;
		}
		if (kmem_alloc_contig(kernel_map, &m_hdr_va[k], M_HDR_BYTES, PAGE_MASK,
		    (ppnum_t)(0x100000000ULL >> PAGE_SHIFT), 0,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			say("kmem_alloc_contig hdr[%u] failed", k);
			return false;
		}
		m_img_pa[k] = (uint32_t)kvtophys(m_img_va[k]);
		m_hdr_pa[k] = (uint32_t)kvtophys(m_hdr_va[k]);
		say("buf[%u] img_pa=%#x hdr_pa=%#x", k, m_img_pa[k], m_hdr_pa[k]);
	}
	m_bufs_ok = true;
	return true;
}

static int m_power_on(void) {
	for (unsigned i = 0; i < 4; i++) {
		if (!m_win[i].va) {
			m_win[i].va = ml_io_map(m_win[i].pa, m_win[i].sz);
			if (!m_win[i].va) { say("ml_io_map(%#llx) failed", m_win[i].pa); return ENOMEM; }
		}
	}
	if (!m_alloc_bufs()) return ENOMEM;

	vm_offset_t top = m_win[0].va, pw = top + M_PWRAP_OFF, gp = top + M_GPIO_OFF;

	/* CAM power domain */
	m_domain_by_us = false;
	if (!((pa_rd32(0x10006180ULL) | pa_rd32(0x10006184ULL)) & (1u << 7))) {
		pa_wr32(0x10006344ULL, pa_rd32(0x10006344ULL) | (1u << 2));
		pa_wr32(0x10006344ULL, pa_rd32(0x10006344ULL) | (1u << 3));
		if (!m_poll(0x10006180ULL, 1u << 7, 1u << 7, 20) || !m_poll(0x10006184ULL, 1u << 7, 1u << 7, 20)) {
			say("CAM domain did not power on");
			return EIO;
		}
		pa_wr32(0x10006344ULL, pa_rd32(0x10006344ULL) & ~(1u << 4));
		pa_wr32(0x10006344ULL, pa_rd32(0x10006344ULL) & ~(1u << 1));
		pa_wr32(0x10006344ULL, pa_rd32(0x10006344ULL) | (1u << 0));
		pa_wr32(0x10006344ULL, pa_rd32(0x10006344ULL) & ~(1u << 8));
		(void)m_poll(0x10006344ULL, 1u << 12, 0, 20);
		pa_wr32(0x10006344ULL, pa_rd32(0x10006344ULL) & ~(1u << 9));
		(void)m_poll(0x10006344ULL, 1u << 13, 0, 20);
		pa_wr32(0x100012a4ULL, 0x00100000u);
		pa_wr32(0x140023c8ULL, 8);
		pa_wr32(0x100012acULL, 0x00280000u);
		m_dsb();
		m_domain_by_us = true;
	}

	m_cfg1_orig = m_rd32(top, 0x50);
	m_cfg6_orig = m_rd32(top, 0xa0);
	m_cfg7_orig = m_rd32(top, 0xb0);
	m_mm_cg_orig = pa_rd32(0x14000100ULL);
	m_wr32(top, 0xb8, 0x80008000u);
	m_wr32(top, 0xa8, 0x00800000u);
	pa_wr32(0x14000108ULL, 0x02000000u);
	pa_wr32(0x1a000008ULL, 0x00001fc3u);
	m_apmix_orig = pa_rd32(0x1000c014ULL);
	pa_wr32(0x1000c014ULL, m_apmix_orig | 0x00000840u);
	for (int n = 0; n < 6; n++) {
		uint64_t a = 0x11c10080ULL + 0x1000ULL * (uint64_t)n;
		pa_wr32(a, pa_rd32(a) | 2u);
	}
	m_dsb();

	/* Sensor rails & pins */
	m_pin_mode(gp, M_PIN_RST, 0);
	m_pin_out(gp, M_PIN_RST, 0);
	m_pin_mode(gp, M_PIN_AVDD, 0);
	m_pin_out(gp, M_PIN_AVDD, 1);
	delay(1000);
	m_have_io = false;
	if (m_pwrap_rd(pw, M_VCAMIO_EN, &m_vcamio_orig)) {
		m_pwrap_wr(pw, M_VCAMIO_EN, m_vcamio_orig | 1u);
		m_have_io = true;
	}
	delay(1000);
	m_pin_mode(gp, M_PIN_DVDD, 0);
	m_pin_out(gp, M_PIN_DVDD, 1);
	delay(2000);
	m_wr32(top, 0x58, 0x00800000u);
	m_dsb();
	m_pin_mode(gp, M_PIN_MCLK, 1);
	delay(2000);
	m_pin_out(gp, M_PIN_RST, 1);
	delay(5000);
	return 0;
}

static void m_power_off(void) {
	vm_offset_t top = m_win[0].va, pw = top + M_PWRAP_OFF, gp = top + M_GPIO_OFF;
	if (!top) return;
	m_pin_out(gp, M_PIN_RST, 0);
	m_pin_mode(gp, M_PIN_MCLK, 0);
	if (m_cfg1_orig & 0x00800000u) m_wr32(top, 0x54, 0x00800000u);
	m_pin_out(gp, M_PIN_DVDD, 0);
	if (m_have_io && !(m_vcamio_orig & 1u)) m_pwrap_wr(pw, M_VCAMIO_EN, m_vcamio_orig);
	m_pin_out(gp, M_PIN_AVDD, 0);
	pa_wr32(0x1000c014ULL, m_apmix_orig);
	if (m_domain_by_us) {
		pa_wr32(0x100012a8ULL, 0x00280000u); (void)m_poll(0x10001258ULL, 0x00280000u, 0x00280000u, 20);
		pa_wr32(0x100012a0ULL, 0x00100000u); (void)m_poll(0x10001228ULL, 0x00100000u, 0x00100000u, 20);
		pa_wr32(0x140023c4ULL, 8); (void)m_poll(0x140023c0ULL, 8, 8, 20);
		pa_wr32(0x10006344ULL, pa_rd32(0x10006344ULL) | (3u << 8));
		(void)m_poll(0x10006344ULL, 3u << 12, 3u << 12, 20);
		pa_wr32(0x10006344ULL, pa_rd32(0x10006344ULL) | (1u << 1) | (1u << 4));
		pa_wr32(0x10006344ULL, pa_rd32(0x10006344ULL) & ~((1u << 0) | (1u << 2) | (1u << 3)));
	}
	pa_wr32(0x14000104ULL, m_mm_cg_orig & 0x02000000u);
	if (m_cfg7_orig & 0x80008000u) m_wr32(top, 0xb4, m_cfg7_orig & 0x80008000u);
	if (m_cfg6_orig & 0x00800000u) m_wr32(top, 0xa4, 0x00800000u);
}

static int m_sensor_start(void) {
	vm_offset_t gp = m_win[0].va + M_GPIO_OFF;
	m_scl_mode = (m_rd32(gp, 0x300 + 0x10 * (M_PIN_SCL / 8)) >> (4 * (M_PIN_SCL % 8))) & 0xfu;
	m_sda_mode = (m_rd32(gp, 0x300 + 0x10 * (M_PIN_SDA / 8)) >> (4 * (M_PIN_SDA % 8))) & 0xfu;
	m_pin_mode(gp, M_PIN_SCL, 0);
	m_pin_mode(gp, M_PIN_SDA, 0);
	m_bb_scl(gp, 1);
	m_bb_sda(gp, 1);

	uint8_t hi = 0, lo = 0;
	if (m_bb_read8(gp, M_SENSOR_ADDR7, 0x0000, &hi) || m_bb_read8(gp, M_SENSOR_ADDR7, 0x0001, &lo) || ((hi << 8) | lo) != 0x08d1) {
		say("sensor_start: bad ID %02x%02x", hi, lo);
		return EIO;
	}
	say("sensor_start: S5KGM1SP ID 0x08d1, writing init (%lu) + preview (%lu)",
	    (unsigned long)(sizeof(gm1sp_init) / sizeof(gm1sp_init[0])),
	    (unsigned long)(sizeof(gm1sp_preview) / sizeof(gm1sp_preview[0])));
	for (unsigned i = 0; i < sizeof(gm1sp_init) / sizeof(gm1sp_init[0]); i++) {
		if (!m_bb_write16(gp, M_SENSOR_ADDR7, gm1sp_init[i].reg, gm1sp_init[i].val)) {
			say("sensor_start: init write %u (%#x) failed", i, gm1sp_init[i].reg);
			return EIO;
		}
		if (gm1sp_init[i].reg == 0x6010) delay(5000);
	}
	for (unsigned i = 0; i < sizeof(gm1sp_preview) / sizeof(gm1sp_preview[0]); i++) {
		if (!m_bb_write16(gp, M_SENSOR_ADDR7, gm1sp_preview[i].reg, gm1sp_preview[i].val)) {
			say("sensor_start: preview write %u (%#x) failed", i, gm1sp_preview[i].reg);
			return EIO;
		}
	}
	if (!m_bb_write16(gp, M_SENSOR_ADDR7, 0x0100, 0x0100)) {
		say("sensor_start: stream on failed");
		return EIO;
	}
	m_cur_exp = 0x0400; m_cur_gain = 0x0100; m_cur_fll = 0x0c7a;
	return 0;
}

static void m_sensor_stop(void) {
	vm_offset_t gp = m_win[0].va + M_GPIO_OFF;
	(void)m_bb_write16(gp, M_SENSOR_ADDR7, 0x0100, 0x0000);
	delay(5000);
	m_pin_mode(gp, M_PIN_SCL, m_scl_mode);
	m_pin_mode(gp, M_PIN_SDA, m_sda_mode);
}

static int m_rx_start(void) {
	for (unsigned k = 0; k < M_NBUF; k++) {
		volatile uint64_t *p = (volatile uint64_t *)m_img_va[k];
		for (uint64_t i = 0; i < M_BUF_BYTES / 8; i++) p[i] = 0;
		m_inval((volatile void *)m_img_va[k], M_BUF_BYTES);
	}
	for (int port = 0; port < 21; port++) {
		pa_wr32(0x1a002380ULL + 4 * (uint64_t)port, 0);
	}
	m_dsb();
	for (unsigned i = 0; i < sizeof(gm1sp_rx) / sizeof(gm1sp_rx[0]); i++) {
		pa_wr32(gm1sp_rx[i].addr, gm1sp_rx[i].val);
		m_dsb();
	}
	for (unsigned i = 0; i < sizeof(gm1sp_sv) / sizeof(gm1sp_sv[0]); i++) {
		if (gm1sp_sv[i].addr == 0x1a051500u || gm1sp_sv[i].addr == 0x1a051504u) continue;
		pa_wr32(gm1sp_sv[i].addr, gm1sp_sv[i].val);
		m_dsb();
	}
	pa_wr32(0x1a051230ULL, M_W - 1);
	pa_wr32(0x1a051234ULL, M_H - 1);
	pa_wr32(0x1a051238ULL, (pa_rd32(0x1a051238ULL) & 0xffff0000u) | M_W);
	m_dsb();
	for (unsigned k = 0; k < M_NBUF; k++) {
		pa_wr32(0x1a051118ULL, m_img_pa[k]);
		pa_wr32(0x1a051e04ULL, m_hdr_pa[k]);
		m_dsb();
		pa_wr32(0x1a05102cULL, pa_rd32(0x1a05102cULL) | 1u);
		m_dsb();
		delay(500);
	}
	for (unsigned i = 0; i < sizeof(gm1sp_sv) / sizeof(gm1sp_sv[0]); i++) {
		if (gm1sp_sv[i].addr == 0x1a051500u || gm1sp_sv[i].addr == 0x1a051504u) {
			pa_wr32(gm1sp_sv[i].addr, gm1sp_sv[i].val);
			m_dsb();
		}
	}
	m_poll_idx = 0;
	say("rx_start: SENINF + CAMSV1 streaming 4000x3000");
	return 0;
}

static void m_rx_stop(void) {
	pa_wr32(0x1a051504ULL, 0);
	pa_wr32(0x1a051500ULL, pa_rd32(0x1a051500ULL) & ~1u);
	m_dsb();
	delay(5000);
}

static int m_frame_done(struct mtkcam_frame_meta *meta) {
	for (unsigned n = 0; n < M_NBUF; n++) {
		unsigned k = (m_poll_idx + n) % M_NBUF;
		volatile uint8_t *tail = (volatile uint8_t *)(m_img_va[k] + M_BUF_BYTES - 64);
		m_inval(tail, 64);
		for (unsigned i = 0; i < 64; i++) {
			if (tail[i] != 0) {
				m_poll_idx = (k + 1) % M_NBUF;
				meta->exposure_lines = m_cur_exp;
				meta->gain = m_cur_gain;
				meta->frame_lines = m_cur_fll;
				return (int)k;
			}
		}
	}
	return -1;
}

static void m_read_lines(unsigned buf, uint32_t first, uint32_t lines, uint8_t *dst) {
	const volatile uint8_t *src = (const volatile uint8_t *)(m_img_va[buf] + (uint64_t)first * M_W);
	m_inval((volatile void *)src, (uint64_t)lines * M_W);
	for (uint32_t y = 0; y < lines; y++) {
		const volatile uint8_t *s = src + (uint64_t)y * M_W;
		uint8_t *d = dst + (uint64_t)y * (M_W * 5 / 4);
		for (uint32_t x = 0; x < M_W; x += 4, d += 5) {
			d[0] = s[x];
			d[1] = s[x + 1];
			d[2] = s[x + 2];
			d[3] = s[x + 3];
			d[4] = 0;
		}
	}
}

static void m_frame_release(unsigned buf) {
	volatile uint8_t *tail = (volatile uint8_t *)(m_img_va[buf] + M_BUF_BYTES - 64);
	for (unsigned i = 0; i < 64; i++) tail[i] = 0;
	m_inval(tail, 64);
	pa_wr32(0x1a051118ULL, m_img_pa[buf]);
	pa_wr32(0x1a051e04ULL, m_hdr_pa[buf]);
	m_dsb();
	pa_wr32(0x1a05102cULL, pa_rd32(0x1a05102cULL) | 1u);
	m_dsb();
}

static int m_set_exposure(uint32_t lines, uint32_t gain, uint32_t frame_lines) {
	vm_offset_t gp = m_win[0].va + M_GPIO_OFF;
	if (frame_lines == 0) {
		frame_lines = lines + 8;
		if (frame_lines < 0x0c7a) frame_lines = 0x0c7a;
	}
	if (!m_bb_write16(gp, M_SENSOR_ADDR7, 0x0340, (uint16_t)frame_lines)) return EIO;
	if (!m_bb_write16(gp, M_SENSOR_ADDR7, 0x0202, (uint16_t)lines)) return EIO;
	if (!m_bb_write16(gp, M_SENSOR_ADDR7, 0x0204, (uint16_t)gain)) return EIO;
	m_cur_exp = lines;
	m_cur_gain = gain;
	m_cur_fll = frame_lines;
	return 0;
}

/* ---- the platform check ---- */
static bool
skel_platform_complete(const struct mtkcam_platform *p)
{
	const struct mtkcam_geometry *g = &p->geometry;
	bool ok = true;

#define NEED(cond, what) do { if (!(cond)) { say("the platform lacks: %s", what); ok = false; } } while (0)
	NEED(p->name != NULL, "name");
	NEED(g->width >= 2 && !(g->width & 3), "geometry.width (a multiple of 4: RAW10)");
	NEED(g->width * 5 / 4 <= SKEL_MAX_STRIDE, "geometry.width at most 6528");
	NEED(g->height >= 2 && !(g->height & 1), "geometry.height (even)");
	NEED(g->nbuf >= 2 && g->nbuf <= SKEL_MAX_BUFFERS, "geometry.nbuf (2..4)");
	NEED(g->line_ns != 0, "geometry.line_ns");
	NEED(g->gain_unity != 0 && g->gain_max >= g->gain_unity, "geometry.gain_unity, gain_max");
	NEED(g->frame_lines_min != 0 && g->frame_lines_max >= g->frame_lines_min, "geometry.frame_lines_min, frame_lines_max");
	NEED(g->black != 0, "geometry.black (the black level; 0 would be a guess)");
	NEED(p->power_on && p->power_off, "power_on, power_off");
	NEED(p->sensor_start && p->sensor_stop, "sensor_start, sensor_stop");
	NEED(p->rx_start && p->rx_stop, "rx_start, rx_stop");
	NEED(p->frame_done && p->read_lines && p->frame_release, "frame_done, read_lines, frame_release");
	NEED(p->set_exposure, "set_exposure");
#undef NEED
	return ok;
}

/* ---- the session ---- */
static LCK_GRP_DECLARE(skel_lck_grp, "redwood-mtkcam-skel");
static LCK_MTX_DECLARE(skel_lock, &skel_lck_grp);

enum skel_slot_state { SLOT_FREE, SLOT_READY, SLOT_READING };

static struct {
	enum skel_slot_state    state;
	uint64_t                sequence, time;
	struct mtkcam_frame_meta meta;
} skel_slot[SKEL_MAX_BUFFERS];

static uint8_t skel_chunk[SKEL_CHUNK_LINES * SKEL_MAX_STRIDE];

static uint32_t skel_opens;
static bool skel_wanted, skel_streaming, skel_reading, skel_broken, skel_node_made;
static uint64_t skel_seq, skel_given, skel_last_read_ms;
static struct {
	bool            pending;
	uint32_t        lines, gain, frame_lines;
} skel_ask;

static uint64_t
skel_ms(void)
{
	uint64_t t, ns;

	t = mach_absolute_time();
	absolutetime_to_nanoseconds(t, &ns);
	return ns / NSEC_PER_MSEC;
}

/* One session: power, sensor, receiver; publish what the DMA finishes until nobody has read for SKEL_IDLE_STOP_MS; everything back off. */
static bool
skel_session(void)
{
	const struct mtkcam_platform *p = &mtkcam_platform;
	uint64_t last_frame_ms;
	int error;

	say("session: power on");
	if ((error = p->power_on()) != 0) {
		say("power_on failed (%d)", error);
		return false;
	}
	say("session: sensor start");
	if ((error = p->sensor_start()) != 0) {
		say("sensor_start failed (%d)", error);
		p->power_off();
		return false;
	}
	say("session: receiver start");
	if ((error = p->rx_start()) != 0) {
		say("rx_start failed (%d)", error);
		p->sensor_stop();
		p->power_off();
		return false;
	}
	lck_mtx_lock(&skel_lock);
	for (unsigned i = 0; i < SKEL_MAX_BUFFERS; i++) {
		skel_slot[i].state = SLOT_FREE;
	}
	skel_given = 0;
	skel_streaming = true;
	skel_last_read_ms = last_frame_ms = skel_ms();
	lck_mtx_unlock(&skel_lock);

	for (;;) {
		struct mtkcam_frame_meta meta;
		int b = p->frame_done(&meta);
		bool stop = false;

		if (b >= 0 && (unsigned)b < p->geometry.nbuf) {
			lck_mtx_lock(&skel_lock);
			if (skel_slot[b].state == SLOT_FREE) {
				/* the older frame nobody read is given back: the newest one is the one wanted */
				for (unsigned i = 0; i < p->geometry.nbuf; i++) {
					if (skel_slot[i].state == SLOT_READY) {
						skel_slot[i].state = SLOT_FREE;
						lck_mtx_unlock(&skel_lock);
						p->frame_release(i);
						lck_mtx_lock(&skel_lock);
					}
				}
				skel_slot[b].state = SLOT_READY;
				skel_slot[b].sequence = ++skel_seq;
				skel_slot[b].time = mach_absolute_time();
				skel_slot[b].meta = meta;
				wakeup(&skel_seq);
				lck_mtx_unlock(&skel_lock);
			} else {
				lck_mtx_unlock(&skel_lock);
				p->frame_release((unsigned)b);          /* (a buffer the driver still holds is not the DMA's to finish: give it back) */
			}
			last_frame_ms = skel_ms();
		}
		lck_mtx_lock(&skel_lock);
		if (skel_ask.pending) {
			struct { uint32_t lines, gain, frame_lines; } ask = { skel_ask.lines, skel_ask.gain, skel_ask.frame_lines };

			skel_ask.pending = false;
			lck_mtx_unlock(&skel_lock);
			if (p->set_exposure(ask.lines, ask.gain, ask.frame_lines) != 0) {
				say("set_exposure(%u, %u, %u) failed", ask.lines, ask.gain, ask.frame_lines);
			}
			lck_mtx_lock(&skel_lock);
		}
		if (skel_opens == 0 || skel_ms() - skel_last_read_ms > SKEL_IDLE_STOP_MS) {
			stop = true;
		} else if (skel_ms() - last_frame_ms > 3000u) {
			say("no frame for 3 s: the camera is given up (reads fail with EIO)");
			skel_broken = true;
			stop = true;
		}
		lck_mtx_unlock(&skel_lock);
		if (stop) {
			break;
		}
		if (b < 0) {
			delay_for_interval(2, NSEC_PER_MSEC);
		}
	}
	lck_mtx_lock(&skel_lock);
	skel_streaming = false;
	lck_mtx_unlock(&skel_lock);
	p->rx_stop();
	p->sensor_stop();
	p->power_off();
	say("session: stopped after %llu frames", skel_seq);
	return !skel_broken;
}

/* ---- /dev/redwood-camera ---- */
static int
skel_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	if (skel_broken) {
		return EIO;
	}
	lck_mtx_lock(&skel_lock);
	skel_opens++;
	lck_mtx_unlock(&skel_lock);
	return 0;
}

static int
skel_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	lck_mtx_lock(&skel_lock);
	if (skel_opens > 0) {
		skel_opens--;
	}
	lck_mtx_unlock(&skel_lock);
	return 0;
}

/* A struct redwood_camera_frame, then as many of the frame's bytes as there is room for (a buffer of the header's size learns the frame and gets nothing more). */
static int
skel_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	const struct mtkcam_platform *p = &mtkcam_platform;
	const struct mtkcam_geometry *g = &p->geometry;
	struct redwood_camera_frame f;
	uint64_t since = skel_ms();
	uint32_t stride = g->width / 4 * 5;
	int b, error = 0;

	lck_mtx_lock(&skel_lock);
	if (skel_reading) {
		lck_mtx_unlock(&skel_lock);
		return EBUSY;
	}
	skel_reading = true;
	skel_last_read_ms = skel_ms();
	for (;;) {
		if (skel_broken) {
			error = EIO;
			break;
		}
		b = -1;
		for (int i = 0; i < (int)g->nbuf; i++) {
			if (skel_slot[i].state == SLOT_READY && skel_slot[i].sequence > skel_given && (b < 0 || skel_slot[i].sequence > skel_slot[b].sequence)) {
				b = i;
			}
		}
		if (b >= 0 && skel_streaming) {
			break;
		}
		if (!skel_streaming && !skel_wanted) {
			skel_wanted = true;
			wakeup(&skel_wanted);
		}
		if (skel_ms() - since > (skel_streaming ? 3000u : 20000u)) {
			error = EAGAIN;
			break;
		}
		struct timespec ts = { 0, 50 * NSEC_PER_MSEC };

		error = msleep(&skel_seq, &skel_lock, PZERO | PCATCH, "redwood-mtkcam-skel", &ts);
		if (error == EWOULDBLOCK) {
			error = 0;
		}
		if (error) {
			break;
		}
		skel_last_read_ms = skel_ms();
	}
	if (error) {
		skel_reading = false;
		lck_mtx_unlock(&skel_lock);
		return error;
	}
	skel_slot[b].state = SLOT_READING;
	skel_given = skel_slot[b].sequence;
	bzero(&f, sizeof(f));
	memcpy(f.magic, REDWOOD_CAMERA_MAGIC, 8);
	f.header_bytes = sizeof(f);
	f.width = g->width;
	f.height = g->height;
	f.stride = stride;
	f.format = 1;                           /* MIPI RAW10 */
	f.bayer = g->bayer;
	f.black = g->black;
	f.sequence = skel_slot[b].sequence;
	f.time = skel_slot[b].time;
	f.exposure_lines = skel_slot[b].meta.exposure_lines;
	f.line_ns = g->line_ns;
	f.gain = skel_slot[b].meta.gain;
	f.gain_unity = g->gain_unity;
	f.gain_max = g->gain_max;
	f.frame_lines = skel_slot[b].meta.frame_lines;
	f.frame_lines_min = g->frame_lines_min;
	f.frame_lines_max = g->frame_lines_max;
	f.margin = g->margin;
	f.sensor = 1;                           /* the wide (main) camera: the only one the skeleton knows */
	lck_mtx_unlock(&skel_lock);

	error = uiomove((caddr_t)&f, (int)MIN(sizeof(f), (size_t)uio_resid(uio)), uio);
	for (uint32_t y = 0; y < g->height && error == 0 && uio_resid(uio) > 0; y += SKEL_CHUNK_LINES) {
		uint32_t n = MIN(SKEL_CHUNK_LINES, g->height - y);

		p->read_lines((unsigned)b, y, n, skel_chunk);
		error = uiomove((caddr_t)skel_chunk, (int)MIN((user_ssize_t)(n * stride), uio_resid(uio)), uio);
	}
	lck_mtx_lock(&skel_lock);
	skel_slot[b].state = SLOT_FREE;
	skel_reading = false;
	skel_last_read_ms = skel_ms();
	lck_mtx_unlock(&skel_lock);
	p->frame_release((unsigned)b);
	return error;
}

static int
skel_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct redwood_camera_command c;
	int error;

	if (uio_resid(uio) < (user_ssize_t)sizeof(c) || (error = uiomove((caddr_t)&c, sizeof(c), uio)) != 0) {
		return EINVAL;
	}
	switch (c.command) {
	case REDWOOD_CAMERA_EXPOSURE:
		lck_mtx_lock(&skel_lock);
		skel_ask.lines = c.a;
		skel_ask.gain = c.b;
		skel_ask.frame_lines = c.c;
		skel_ask.pending = true;
		lck_mtx_unlock(&skel_lock);
		return 0;
	case REDWOOD_CAMERA_KEEP:
		return ENOSPC;                  /* no memory in one piece is kept for the host */
	case REDWOOD_CAMERA_SENSOR:
		return c.a == 1 ? 0 : EINVAL;   /* only the wide (main) camera */
	default:
		return EINVAL;
	}
}

static const struct cdevsw skel_cdevsw = {
	.d_open = skel_open, .d_close = skel_close, .d_read = skel_read, .d_write = skel_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* A session whenever a read wants frames; after three sessions that never got to a frame the camera is given up. */
static void
skel_thread(__unused void *arg, __unused wait_result_t w)
{
	uint32_t failed = 0;
	int major;

	if (!skel_platform_complete(&mtkcam_platform)) {
		say("the platform is not complete: no camera");
		thread_terminate(current_thread());
		return;
	}
	major = cdevsw_add(-1, &skel_cdevsw);
	skel_node_made = major >= 0 && devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "redwood-camera") != NULL;
	say("%s: /dev/redwood-camera %s", mtkcam_platform.name, skel_node_made ? "made" : "CANNOT BE MADE");
	while (skel_node_made && !skel_broken) {
		struct timespec ts = { 1, 0 };

		lck_mtx_lock(&skel_lock);
		while (!skel_wanted) {
			(void)msleep(&skel_wanted, &skel_lock, PZERO, "redwood-mtkcam-skel-idle", &ts);
		}
		skel_wanted = false;
		if (skel_opens == 0) {
			lck_mtx_unlock(&skel_lock);
			continue;               /* (a read that came while the last session was ending) */
		}
		lck_mtx_unlock(&skel_lock);
		if (!skel_session()) {
			if (++failed >= 3) {
				say("three sessions failed: the camera is given up");
				skel_broken = true;
			}
		} else {
			failed = 0;
		}
	}
	thread_terminate(current_thread());
}

void redwood_mtkcam_skel_init(void);

void
redwood_mtkcam_skel_init(void)
{
	uint32_t level = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("mtkcam_skel", &level, sizeof(level)) || level < 1) {
		return;
	}
	(void)m_alloc_bufs();
	if (kernel_thread_start_priority(skel_thread, NULL, 81 /* BASEPRI_KERNEL */, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

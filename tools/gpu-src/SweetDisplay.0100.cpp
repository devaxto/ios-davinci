/*
 * DAVINCI: probe + zero-copy flip of the display controller (MDSS/SDE) that
 * the bootloader left running.
 *
 * The whole display path today is "CPU writes the ABL splash buffer at
 * 0x9c000000 and the DPU keeps scanning it out". The registers that do that
 * are configured by the bootloader; the probe reads them, remembers the live
 * SSPP SRC_ADDR / YSTRIDE and the CTL flush bit, and (only when asked via
 * sweet_display_flip) rewrites SRC_ADDR so a present is two MMIO writes
 * instead of a 10 MB memcpy. See docs/GPU.md.
 *
 * Register offsets below are the davinci DTB values relative to mdp_phys
 * 0xae00000. SSPP_SRC0_ADDR = +0x14, SSPP_SRC_YSTRIDE0 = +0x24, CTL_FLUSH =
 * +0x018, CTL_START = +0x01C (Linux dpu_hw_sspp / dpu_hw_ctl). Flush bits
 * match SDE: VIG0/1 = 0/1, DMA0/1/2 = 11/12/24, LM0 = 6.
 */
#include "SweetDisplay.h"

/* MACHINE_FLAGS passes -DARM64_BOARD_CONFIG_SWEET; the MASTER `SWEET`
 * config option is NOT visible to every optional iokitcpp TU. Using the
 * board macro (same as sweet_gpu.c / SweetAccelerator.cpp) is what actually
 * compiles the DPU probe+flip — otherwise only the #else stubs link and
 * sweet_display_can_flip() is permanently false → 10 MB CPU present forever. */
#if defined(ARM64_BOARD_CONFIG_SWEET)

#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>

extern "C" vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size);

#define MDSS_PA             0x0ae00000u
#define MDSS_SIZE           0x00084208u
#define MDSS_SDE_OFF        0x01000u
#define DSI0_PA             0x0ae94000u
#define DSI0_SIZE           0x00001000u

#define SSPP_SRC0_ADDR      0x014u
#define SSPP_SRC_YSTRIDE0   0x024u
#define SSPP_SRC_XY         0x028u
#define SSPP_DST_XY         0x02Cu
#define SSPP_SRC_SIZE       0x030u
#define SSPP_DST_SIZE       0x034u
#define SSPP_SRC_FORMAT     0x038u
#define CTL_FLUSH           0x018u
#define CTL_START           0x01Cu
#define CTL_LAYER           0x000u

/*
 * SRC_SIZE / DST_SIZE pack width and height in the two 16-bit halves; older
 * and newer SDE revisions disagree on whether the fields hold the size or
 * size-1, so the probe derives the encoding from the live DST_SIZE (which
 * must be the panel rect, since the CPU copy into the panel shows 1:1) and
 * refuses to touch geometry it cannot decode.
 */
#define SWEET_SIZE_UNKNOWN  0
#define SWEET_SIZE_DIRECT   1
#define SWEET_SIZE_MINUS1   2

struct sweet_dpu_block {
	const char *name;
	uint32_t    off;
	uint32_t    len;
	bool        live;
	uint32_t    flush;      /* CTL_FLUSH bit for this SSPP, 0 if not an SSPP */
};

static const struct sweet_dpu_block kDpuBlocks[] = {
	{ "top",   0x01000u, 0x45cu, false, 0 },
	{ "ctl0",  0x02000u, 0x1e0u, false, 0 },
	{ "ctl1",  0x02200u, 0x1e0u, false, 0 },
	{ "ctl2",  0x02400u, 0x1e0u, false, 0 },
	{ "ctl3",  0x02600u, 0x1e0u, false, 0 },
	{ "lm0",   0x45000u, 0x320u, true,  0 },
	{ "lm1",   0x46000u, 0x320u, true,  0 },
	{ "lm2",   0x47000u, 0x320u, true,  0 },
	{ "lm3",   0x48000u, 0x320u, true,  0 },
	{ "vig0",  0x05000u, 0x1f0u, false, (1u << 0) },
	{ "vig1",  0x07000u, 0x1f0u, false, (1u << 1) },
	{ "dma0",  0x25000u, 0x1f0u, false, (1u << 11) },
	{ "dma1",  0x27000u, 0x1f0u, false, (1u << 12) },
	{ "dma2",  0x29000u, 0x1f0u, false, (1u << 24) },
	{ "intf0", 0x6b000u, 0x100u, true,  0 },
	{ "intf1", 0x6b800u, 0x100u, true,  0 },
	{ "intf2", 0x6c000u, 0x100u, true,  0 },
	{ "intf3", 0x6c800u, 0x100u, true,  0 },
	{ "pp0",   0x71000u, 0x0d4u, true,  0 },
	{ "pp1",   0x71800u, 0x0d4u, true,  0 },
	{ "pp2",   0x72000u, 0x0d4u, true,  0 },
	{ "pp3",   0x72800u, 0x0d4u, true,  0 },
};

#define SWEET_DPU_LIVE_WORDS    64

struct sweet_dpu_state {
	vm_offset_t mdss;
	uint32_t    sspp_off;
	uint32_t    src_addr_off;   /* absolute from mdss base */
	uint32_t    src_stride_off;
	uint32_t    src_xy_off;     /* absolute from mdss base */
	uint32_t    src_size_off;
	uint32_t    dst_xy_off;
	uint32_t    dst_size_off;
	uint32_t    ctl_off;
	uint32_t    ctl_bits;       /* bit i = ctl i carries a live LAYER */
	uint32_t    flush_mask;
	uint32_t    orig_pa;
	uint32_t    orig_stride;
	uint32_t    orig_src_size;
	uint32_t    orig_dst_size;
	uint32_t    orig_src_xy;
	uint32_t    cur_pa;
	uint32_t    cur_stride;
	uint32_t    cur_src_size;
	uint32_t    cur_x, cur_y;    /* source window origin */
	uint32_t    cur_w, cur_h;    /* source window size (= panel) */
	int         size_enc;       /* SWEET_SIZE_* */
	int         video_mode;     /* INTF/PP counters moved */
	int         ready;
	int         disabled;       /* a flip failed to latch, stay on CPU copy */
	const char *sspp_name;
};

static struct sweet_dpu_state g_dpu;

static uint32_t
dpu_rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static void
dpu_wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static uint32_t
dpu_size_lo(uint32_t v, int enc)
{
	return (v & 0xffffu) + (enc == SWEET_SIZE_MINUS1 ? 1u : 0u);
}

static uint32_t
dpu_size_hi(uint32_t v, int enc)
{
	return (v >> 16) + (enc == SWEET_SIZE_MINUS1 ? 1u : 0u);
}

static uint32_t
dpu_size_pack(uint32_t w, uint32_t h, int enc)
{
	uint32_t lo = w - (enc == SWEET_SIZE_MINUS1 ? 1u : 0u);
	uint32_t hi = h - (enc == SWEET_SIZE_MINUS1 ? 1u : 0u);

	return ((hi & 0xffffu) << 16) | (lo & 0xffffu);
}

static uint32_t
dpu_xy_pack(uint32_t x, uint32_t y)
{
	return ((y & 0xffffu) << 16) | (x & 0xffffu);
}

/*
 * CTL offsets of the 4 SDE control paths (dtb qcom,sde-ctl-off). Every CTL
 * that has a layer programmed gets the flush and the command-mode kick, so
 * a wrong guess about which CTL drives DSI0 cannot eat the update.
 */
static const uint32_t kCtlOffsets[] = { 0x02000u, 0x02200u, 0x02400u, 0x02600u };

/* sweetdpu=1: discover quietly. sweetdpu=2: dump every nonzero register. */
static int g_dpu_verbose;

static void
dpu_dump_block(vm_offset_t base, const struct sweet_dpu_block *b,
    uint32_t fb_pa, uint32_t fb_stride, uint32_t *words, uint32_t *nonzero)
{
	for (uint32_t off = 0; off + 4 <= b->len; off += 4) {
		uint32_t v = dpu_rd(base, b->off + off);

		(*words)++;
		if (v == 0) {
			continue;
		}
		(*nonzero)++;
		if (g_dpu_verbose >= 2) {
			IOLog("SWEET DPU: %s+0x%03x=0x%08x\n", b->name, off, v);
		}
		if (v == fb_pa) {
			IOLog("SWEET DPU: %s+0x%03x is the panel framebuffer\n",
			    b->name, off);
			/*
			 * Prefer the canonical SRC0_ADDR offset; accept any hit
			 * inside an SSPP so a quirky ABL programming still works.
			 */
			if (b->flush != 0 && (!g_dpu.ready || off == SSPP_SRC0_ADDR)) {
				g_dpu.sspp_off = b->off;
				g_dpu.src_addr_off = b->off + off;
				g_dpu.src_stride_off = b->off + SSPP_SRC_YSTRIDE0;
				g_dpu.src_xy_off = b->off + SSPP_SRC_XY;
				g_dpu.src_size_off = b->off + SSPP_SRC_SIZE;
				g_dpu.dst_xy_off = b->off + SSPP_DST_XY;
				g_dpu.dst_size_off = b->off + SSPP_DST_SIZE;
				g_dpu.flush_mask = b->flush;
				g_dpu.orig_pa = fb_pa;
				g_dpu.cur_pa = fb_pa;
				g_dpu.sspp_name = b->name;
				g_dpu.ready = 1;
			}
		} else if (v == fb_stride || (v & 0xffffu) == fb_stride) {
			IOLog("SWEET DPU: %s+0x%03x is the panel stride\n",
			    b->name, off);
			if (b->flush != 0) {
				g_dpu.orig_stride = fb_stride;
				g_dpu.cur_stride = fb_stride;
				if (g_dpu.sspp_off == b->off) {
					g_dpu.src_stride_off = b->off + off;
				}
			}
		}
	}
}

static void
dpu_check_live(vm_offset_t base, const struct sweet_dpu_block *b)
{
	uint32_t before[SWEET_DPU_LIVE_WORDS], after[SWEET_DPU_LIVE_WORDS];
	uint32_t n = b->len / 4;
	uint32_t moved = 0, shown = 0;

	if (n > SWEET_DPU_LIVE_WORDS) {
		n = SWEET_DPU_LIVE_WORDS;
	}
	for (uint32_t i = 0; i < n; i++) {
		before[i] = dpu_rd(base, b->off + 4 * i);
	}
	IODelay(30000);
	for (uint32_t i = 0; i < n; i++) {
		after[i] = dpu_rd(base, b->off + 4 * i);
		if (after[i] == before[i]) {
			continue;
		}
		moved++;
		if (g_dpu_verbose >= 2 && shown < 4) {
			shown++;
			IOLog("SWEET DPU: %s+0x%03x moved 0x%08x -> 0x%08x\n",
			    b->name, 4 * i, before[i], after[i]);
		}
	}
	if (g_dpu_verbose >= 2 || moved != 0) {
		IOLog("SWEET DPU: %s %u/%u words moved in 30 ms%s\n", b->name, moved, n,
		    moved ? " (live)" : "");
	}
	if (moved != 0 && (b->name[0] == 'i' || b->name[0] == 'p')) {
		/* intf* / pp* counters advancing => video mode. */
		g_dpu.video_mode = 1;
	}
}

static void
dpu_pick_ctls(vm_offset_t base)
{
	for (unsigned int i = 0; i < sizeof(kCtlOffsets) / sizeof(kCtlOffsets[0]); i++) {
		uint32_t layer = dpu_rd(base, kCtlOffsets[i] + CTL_LAYER);

		if (layer == 0) {
			continue;
		}
		g_dpu.ctl_bits |= 1u << i;
		IOLog("SWEET DPU: ctl%u live (layer=0x%08x)\n", i, layer);
	}
	if (g_dpu.ctl_bits == 0) {
		g_dpu.ctl_bits = 1u << 0;
		IOLog("SWEET DPU: no live CTL_LAYER, defaulting to ctl0\n");
	}
	g_dpu.ctl_off = kCtlOffsets[0];
}

/* Flush + kick every live CTL; the panels that need it run in command mode. */
static void
dpu_commit(void)
{
	for (unsigned int i = 0; i < sizeof(kCtlOffsets) / sizeof(kCtlOffsets[0]); i++) {
		if (!(g_dpu.ctl_bits & (1u << i))) {
			continue;
		}
		dpu_wr(g_dpu.mdss, kCtlOffsets[i] + CTL_FLUSH, g_dpu.flush_mask);
		if (!g_dpu.video_mode) {
			dpu_wr(g_dpu.mdss, kCtlOffsets[i] + CTL_START, 1u);
		}
	}
}

void
sweet_display_probe(uint64_t fb_phys, uint32_t fb_stride,
    uint32_t fb_width, uint32_t fb_height)
{
	int mode = 1;
	uint32_t words = 0, nonzero = 0, found = 0;
	vm_offset_t mdss, dsi;

	PE_parse_boot_argn("sweetdpu", &mode, sizeof(mode));
	if (mode <= 0) {
		return;
	}
	g_dpu_verbose = mode;

	bzero(&g_dpu, sizeof(g_dpu));
	g_dpu.orig_stride = fb_stride;
	g_dpu.cur_stride = fb_stride;

	mdss = ml_io_map(MDSS_PA, MDSS_SIZE);
	if (mdss == 0) {
		IOLog("SWEET DPU: MDSS map failed\n");
		return;
	}
	g_dpu.mdss = mdss;

	IOLog("SWEET DPU: panel 0x%llx %ux%u stride %u\n",
	    (unsigned long long)fb_phys, fb_width, fb_height, fb_stride);
	IOLog("SWEET DPU: mdp 0x%08x sde rev 0x%08x\n", MDSS_PA,
	    dpu_rd(mdss, MDSS_SDE_OFF));
	if (dpu_rd(mdss, MDSS_SDE_OFF) == 0 ||
	    dpu_rd(mdss, MDSS_SDE_OFF) == 0xffffffffu) {
		IOLog("SWEET DPU: SDE not clocked, no register dump\n");
		return;
	}

	for (unsigned int i = 0; i < sizeof(kDpuBlocks) / sizeof(kDpuBlocks[0]); i++) {
		dpu_dump_block(mdss, &kDpuBlocks[i], (uint32_t)fb_phys, fb_stride,
		    &words, &nonzero);
	}
	IOLog("SWEET DPU: mdp dump %u words, %u nonzero\n", words, nonzero);

	for (unsigned int i = 0; i < sizeof(kDpuBlocks) / sizeof(kDpuBlocks[0]); i++) {
		if (kDpuBlocks[i].live) {
			dpu_check_live(mdss, &kDpuBlocks[i]);
		}
	}

	dpu_pick_ctls(mdss);

	/*
	 * Geometry the bootloader left live. The CPU copy paints the whole panel
	 * 1:1 out of this very pipeline, so DST_SIZE must be the panel rect and
	 * SRC_SIZE the same: that is what makes a repoint safe. Decode the
	 * size-1 vs size encoding from DST_SIZE; if it matches neither, leave
	 * SRC_SIZE/DST_SIZE alone and only allow same-geometry flips.
	 */
	if (g_dpu.ready) {
		uint32_t src, dst, src_xy, dst_xy, fmt;

		src = dpu_rd(mdss, g_dpu.src_size_off);
		dst = dpu_rd(mdss, g_dpu.dst_size_off);
		src_xy = dpu_rd(mdss, g_dpu.src_xy_off);
		dst_xy = dpu_rd(mdss, g_dpu.dst_xy_off);
		fmt = dpu_rd(mdss, g_dpu.sspp_off + SSPP_SRC_FORMAT);
		g_dpu.orig_src_size = src;
		g_dpu.orig_dst_size = dst;
		g_dpu.orig_src_xy = src_xy;
		g_dpu.cur_src_size = src;
		g_dpu.cur_w = fb_width;
		g_dpu.cur_h = fb_height;
		if (dpu_size_lo(dst, SWEET_SIZE_MINUS1) == fb_width &&
		    dpu_size_hi(dst, SWEET_SIZE_MINUS1) == fb_height) {
			g_dpu.size_enc = SWEET_SIZE_MINUS1;
		} else if (dpu_size_lo(dst, SWEET_SIZE_DIRECT) == fb_width &&
		    dpu_size_hi(dst, SWEET_SIZE_DIRECT) == fb_height) {
			g_dpu.size_enc = SWEET_SIZE_DIRECT;
		}
		IOLog("SWEET DPU: plane src_size 0x%08x dst_size 0x%08x src_xy "
		    "0x%08x dst_xy 0x%08x fmt 0x%08x sizes are %s\n",
		    src, dst, src_xy, dst_xy, fmt,
		    g_dpu.size_enc == SWEET_SIZE_MINUS1 ? "size-1" :
		    g_dpu.size_enc == SWEET_SIZE_DIRECT ? "size" : "unknown");
	}

	dsi = ml_io_map(DSI0_PA, DSI0_SIZE);
	if (dsi == 0) {
		IOLog("SWEET DPU: DSI0 map failed\n");
	} else {
	for (uint32_t off = 0; off < 0x100; off += 4) {
		uint32_t v = dpu_rd(dsi, off);

		if (v == 0) {
			continue;
		}
		found++;
		if (g_dpu_verbose >= 2) {
			IOLog("SWEET DPU: dsi0+0x%03x=0x%08x\n", off, v);
		}
	}
	IOLog("SWEET DPU: dsi0 0x%08x, %u nonzero words\n", DSI0_PA, found);
	}

	if (g_dpu.ready) {
		IOLog("SWEET DPU: flip ready on %s SRC_ADDR=+0x%x YSTRIDE=+0x%x "
		    "SZ=+0x%x CTL0x%x flush=0x%x %s%s%s\n",
		    g_dpu.sspp_name ? g_dpu.sspp_name : "?",
		    g_dpu.src_addr_off, g_dpu.src_stride_off, g_dpu.src_size_off,
		    g_dpu.ctl_bits, g_dpu.flush_mask,
		    g_dpu.video_mode ? "video" : "cmd-or-idle",
		    g_dpu.size_enc == SWEET_SIZE_UNKNOWN ? " geom-locked" : "",
		    g_dpu.orig_src_xy != 0 ? " src-xy-nonzero" : "");
	} else {
		IOLog("SWEET DPU: no SSPP holds the panel PA - flip disabled\n");
	}
}

bool
sweet_display_can_flip(void)
{
	return g_dpu.ready && !g_dpu.disabled &&
	    g_dpu.mdss != 0 && g_dpu.ctl_bits != 0;
}

/* Put the plane back exactly as the bootloader left it (panel PA, geometry). */
static void
dpu_restore(void)
{
	if (!g_dpu.ready || g_dpu.mdss == 0) {
		return;
	}
	dpu_wr(g_dpu.mdss, g_dpu.src_size_off, g_dpu.orig_src_size);
	dpu_wr(g_dpu.mdss, g_dpu.dst_size_off, g_dpu.orig_dst_size);
	dpu_wr(g_dpu.mdss, g_dpu.src_xy_off, g_dpu.orig_src_xy);
	dpu_wr(g_dpu.mdss, g_dpu.src_stride_off,
	    (dpu_rd(g_dpu.mdss, g_dpu.src_stride_off) & 0xffff0000u) |
	    (g_dpu.orig_stride & 0xffffu));
	dpu_wr(g_dpu.mdss, g_dpu.src_addr_off, g_dpu.orig_pa);
	dpu_commit();
	g_dpu.cur_pa = g_dpu.orig_pa;
	g_dpu.cur_stride = g_dpu.orig_stride;
	g_dpu.cur_src_size = g_dpu.orig_src_size;
	g_dpu.cur_x = 0;
	g_dpu.cur_y = 0;
}

/*
 * Point the live SSPP at phys (32-bit PA) with the given bytes-per-row and,
 * optionally, a source window origin inside that buffer (the centered panel
 * crop of a larger logical surface) and window size (defaults to the panel
 * rect the probe recorded).
 *
 * Only SRC_ADDR / YSTRIDE0 / SRC_XY change; the mixer, format and scaling
 * stay as the bootloader left them, and the source window keeps the panel
 * size, so the fetcher walks exactly the pixels the CPU copy used to move.
 * A size the encoding cannot be derived for is never written - a wrong
 * SRC_SIZE is what scrambles a flip.
 */
bool
sweet_display_flip(uint32_t phys, uint32_t stride, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height)
{
	uint32_t ystride, size, latched;

	if (!sweet_display_can_flip() || phys == 0 || width == 0 || height == 0) {
		return false;
	}
	if (g_dpu.size_enc == SWEET_SIZE_UNKNOWN &&
	    (x != 0 || y != 0 || width != g_dpu.cur_w || height != g_dpu.cur_h)) {
		/* Cannot express a window without knowing the SRC_SIZE encoding. */
		return false;
	}
	if (phys == g_dpu.cur_pa && stride == g_dpu.cur_stride &&
	    x == g_dpu.cur_x && y == g_dpu.cur_y &&
	    width == g_dpu.cur_w && height == g_dpu.cur_h) {
		return true;
	}

	if (g_dpu.size_enc != SWEET_SIZE_UNKNOWN) {
		size = dpu_size_pack(width, height, g_dpu.size_enc);
		if (size != g_dpu.cur_src_size) {
			dpu_wr(g_dpu.mdss, g_dpu.src_size_off, size);
			dpu_wr(g_dpu.mdss, g_dpu.dst_size_off, size);
			g_dpu.cur_src_size = size;
		}
		dpu_wr(g_dpu.mdss, g_dpu.src_xy_off, dpu_xy_pack(x, y));
	}
	dpu_wr(g_dpu.mdss, g_dpu.src_addr_off, phys);
	if (stride != 0 && stride != g_dpu.cur_stride) {
		ystride = dpu_rd(g_dpu.mdss, g_dpu.src_stride_off);
		ystride = (ystride & 0xffff0000u) | (stride & 0xffffu);
		dpu_wr(g_dpu.mdss, g_dpu.src_stride_off, ystride);
		g_dpu.cur_stride = stride;
	}
	dpu_commit();

	latched = dpu_rd(g_dpu.mdss, g_dpu.src_addr_off);
	if (latched != phys) {
		IOLog("SWEET DPU: flip did not stick (0x%08x != 0x%08x), "
		    "restoring and disabling\n", latched, phys);
		dpu_restore();
		g_dpu.disabled = 1;
		return false;
	}
	g_dpu.cur_pa = phys;
	g_dpu.cur_x = x;
	g_dpu.cur_y = y;
	g_dpu.cur_w = width;
	g_dpu.cur_h = height;
	return true;
}

void
sweet_display_flip_reset(void)
{
	if (!g_dpu.ready || g_dpu.orig_pa == 0) {
		return;
	}
	dpu_restore();
}

#else /* !ARM64_BOARD_CONFIG_SWEET */

void
sweet_display_probe(uint64_t fb_phys, uint32_t fb_stride,
    uint32_t fb_width, uint32_t fb_height)
{
	(void)fb_phys;
	(void)fb_stride;
	(void)fb_width;
	(void)fb_height;
}

bool
sweet_display_can_flip(void)
{
	return false;
}

bool
sweet_display_flip(uint32_t phys, uint32_t stride, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height)
{
	(void)phys;
	(void)stride;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	return false;
}

void
sweet_display_flip_reset(void)
{
}

#endif /* SWEET */

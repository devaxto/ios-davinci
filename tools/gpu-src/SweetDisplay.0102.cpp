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
 * Register offsets are the davinci DTB values relative to mdp_phys
 * 0xae00000, with the SSPP rect offsets from the CAF SDE driver for this SoC
 * (drivers/gpu/drm/msm/sde/sde_hw_sspp.c, SRC sub-block base 0):
 * SRC_SIZE +0x00, SRC_XY +0x08, OUT_SIZE +0x0c, OUT_XY +0x10, SRC0_ADDR
 * +0x14, SRC_YSTRIDE0 +0x24, SRC_FORMAT +0x30. Sizes pack (h << 16) | w and
 * positions (y << 16) | x. 0101 read SRC_FORMAT as SRC_SIZE and
 * UNPACK_PATTERN as OUT_SIZE, so the live window never decoded and was never
 * programmed - the panel kept the bootloader's scaling. CTL_FLUSH = +0x018,
 * CTL_START = +0x01C (Linux dpu_hw_ctl). Flush bits match SDE:
 * VIG0/1 = 0/1, DMA0/1/2 = 11/12/24, LM0 = 6.
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

#define SSPP_SRC_SIZE       0x000u
#define SSPP_SRC_XY         0x008u
#define SSPP_OUT_SIZE       0x00Cu
#define SSPP_OUT_XY         0x010u
#define SSPP_SRC0_ADDR      0x014u
#define SSPP_SRC_YSTRIDE0   0x024u
#define SSPP_SRC_FORMAT     0x030u
#define SSPP_SRC_UNPACK     0x034u
#define SSPP_SRC_OP_MODE    0x038u
#define SSPP_MULTIRECT_MODE 0x170u
#define CTL_FLUSH           0x018u
#define CTL_START           0x01Cu
#define CTL_LAYER           0x000u

/*
 * The vendor packing is (h << 16) | w for sizes and (y << 16) | x for
 * positions, but a bootloader may hold size-1 or the halves swapped, so the
 * probe decodes the live OUT_SIZE against the panel rect over four candidates
 * and keeps the one that reproduces it. Nothing decodes -> the vendor packing
 * is used: the window has to be programmed either way, because leaving one we
 * cannot read is what kept the DPU scaling the bootloader's splash rectangle.
 */
static const struct {
	int minus1;
	int swap;       /* width in the high half */
} kSizeEnc[] = {
	{ 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 },
};
#define SWEET_SIZE_ENCS     (sizeof(kSizeEnc) / sizeof(kSizeEnc[0]))
#define SWEET_SIZE_UNKNOWN  0   /* or the 1-based index in kSizeEnc */

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
	uint32_t    out_xy_off;
	uint32_t    out_size_off;
	uint32_t    ctl_off;
	uint32_t    ctl_bits;       /* bit i = ctl i carries a live LAYER */
	uint32_t    flush_mask;
	uint32_t    orig_pa;
	uint32_t    orig_stride;
	uint32_t    orig_src_size;
	uint32_t    orig_out_size;
	uint32_t    orig_src_xy;
	uint32_t    cur_pa;
	uint32_t    cur_stride;
	uint32_t    cur_src_size;
	uint32_t    cur_x, cur_y;    /* source window origin */
	uint32_t    cur_w, cur_h;    /* source window size (= panel) */
	uint32_t    flips_logged;
	int         size_enc;       /* index into kSizeEnc, 0 = unknown */
	int         geom_guess;     /* size packing could not be decoded */
	int         geom_write;     /* sweetfb_geom, default on */
	int         dirty;          /* plane moved off the bootloader state */
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

static void
dpu_size_decode(uint32_t v, int enc, uint32_t *w, uint32_t *h)
{
	uint32_t lo = v & 0xffffu;
	uint32_t hi = v >> 16;

	*w = 0;
	*h = 0;
	if (enc <= 0 || enc > (int)SWEET_SIZE_ENCS) {
		return;
	}
	if (kSizeEnc[enc - 1].minus1) {
		lo++;
		hi++;
	}
	if (kSizeEnc[enc - 1].swap) {
		*w = hi;
		*h = lo;
	} else {
		*w = lo;
		*h = hi;
	}
}

static uint32_t
dpu_size_pack(uint32_t w, uint32_t h, int enc)
{
	uint32_t lo = w, hi = h;

	if (enc <= 0 || enc > (int)SWEET_SIZE_ENCS) {
		return 0;
	}
	if (kSizeEnc[enc - 1].minus1) {
		lo--;
		hi--;
	}
	if (kSizeEnc[enc - 1].swap) {
		return ((lo & 0xffffu) << 16) | (hi & 0xffffu);
	}
	return ((hi & 0xffffu) << 16) | (lo & 0xffffu);
}

/* SRC_XY is programmed with the same macro as SRC_SIZE. */
static uint32_t
dpu_xy_pack(uint32_t x, uint32_t y, int enc)
{
	if (enc > 0 && enc <= (int)SWEET_SIZE_ENCS && kSizeEnc[enc - 1].swap) {
		return ((x & 0xffffu) << 16) | (y & 0xffffu);
	}
	return ((y & 0xffffu) << 16) | (x & 0xffffu);
}

static const char *
dpu_enc_name(void)
{
	if (g_dpu.size_enc <= 0 || g_dpu.size_enc > (int)SWEET_SIZE_ENCS) {
		return "unknown";
	}
	if (kSizeEnc[g_dpu.size_enc - 1].swap) {
		return kSizeEnc[g_dpu.size_enc - 1].minus1 ? "w-high,s-1" : "w-high";
	}
	return kSizeEnc[g_dpu.size_enc - 1].minus1 ? "w-low,s-1" : "w-low";
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
				g_dpu.out_xy_off = b->off + SSPP_OUT_XY;
				g_dpu.out_size_off = b->off + SSPP_OUT_SIZE;
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
	 * Geometry the bootloader left live, read from where the SDE actually
	 * keeps it: SRC_SIZE/SRC_XY for the source rect, OUT_SIZE/OUT_XY for the
	 * mixer rect. The vendor packing is (h << 16) | w; the four candidates
	 * only exist so a bootloader that wrote size-1 or swapped halves is
	 * understood. Whatever it is, the flip programs the window again: the
	 * panel never showed the surface correctly while the bootloader's
	 * rectangle was left in place.
	 */
	if (g_dpu.ready) {
		uint32_t src, out, src_xy, out_xy, fmt, unpack, opmode, multi;
		uint32_t dw = 0, dh = 0, sw = 0, sh = 0;
		int enc = SWEET_SIZE_UNKNOWN;
		int geom = 1;

		src = dpu_rd(mdss, g_dpu.src_size_off);
		out = dpu_rd(mdss, g_dpu.out_size_off);
		src_xy = dpu_rd(mdss, g_dpu.src_xy_off);
		out_xy = dpu_rd(mdss, g_dpu.out_xy_off);
		fmt = dpu_rd(mdss, g_dpu.sspp_off + SSPP_SRC_FORMAT);
		unpack = dpu_rd(mdss, g_dpu.sspp_off + SSPP_SRC_UNPACK);
		opmode = dpu_rd(mdss, g_dpu.sspp_off + SSPP_SRC_OP_MODE);
		multi = dpu_rd(mdss, g_dpu.sspp_off + SSPP_MULTIRECT_MODE);
		g_dpu.orig_src_size = src;
		g_dpu.orig_out_size = out;
		g_dpu.orig_src_xy = src_xy;
		PE_parse_boot_argn("sweetfb_geom", &geom, sizeof(geom));
		g_dpu.geom_write = geom > 0;

		for (int e = 1; e <= (int)SWEET_SIZE_ENCS; e++) {
			dpu_size_decode(out, e, &dw, &dh);
			if (dw == fb_width && dh == fb_height) {
				enc = e;
				break;
			}
		}
		if (enc == SWEET_SIZE_UNKNOWN) {
			/* kSizeEnc[0] is the vendor packing: (h << 16) | w. */
			enc = 1;
			g_dpu.geom_guess = 1;
		}
		g_dpu.size_enc = enc;
		dpu_size_decode(src, enc, &sw, &sh);
		dpu_size_decode(out, enc, &dw, &dh);
		dpu_size_decode(src_xy, enc, &g_dpu.cur_x, &g_dpu.cur_y);
		/* What the plane really holds, so the flip knows what to fix. */
		g_dpu.cur_w = dw;
		g_dpu.cur_h = dh;
		IOLog("SWEET DPU: plane src_size 0x%08x out_size 0x%08x src_xy "
		    "0x%08x out_xy 0x%08x fmt 0x%08x unpack 0x%08x opmode 0x%08x "
		    "multi 0x%08x -> src %ux%u out %ux%u at %u,%u sizes %s%s\n",
		    src, out, src_xy, out_xy, fmt, unpack, opmode, multi,
		    sw, sh, dw, dh, g_dpu.cur_x, g_dpu.cur_y, dpu_enc_name(),
		    g_dpu.geom_guess ? " (guess)" : "");
		if (multi != 0) {
			/* Virtual planes: rect1 has its own address/size block. */
			IOLog("SWEET DPU: SSPP is in multirect mode, flip keeps rect0 "
			    "only\n");
		}
		if (dw != fb_width || dh != fb_height) {
			IOLog("SWEET DPU: bootloader window is %ux%u, the flip will "
			    "program %ux%u%s\n", dw, dh, fb_width, fb_height,
			    g_dpu.geom_write ? "" : " (sweetfb_geom=0)");
		}
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
		    "SRC_SIZE=+0x%x OUT_SIZE=+0x%x CTL0x%x flush=0x%x %s%s%s "
		    "sizes %s\n",
		    g_dpu.sspp_name ? g_dpu.sspp_name : "?",
		    g_dpu.src_addr_off, g_dpu.src_stride_off, g_dpu.src_size_off,
		    g_dpu.out_size_off, g_dpu.ctl_bits, g_dpu.flush_mask,
		    g_dpu.video_mode ? "video" : "cmd-or-idle",
		    g_dpu.geom_guess ? " geom-guess" : "",
		    g_dpu.geom_write ? "" : " geom-read-only",
		    dpu_enc_name());
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
	dpu_wr(g_dpu.mdss, g_dpu.out_size_off, g_dpu.orig_out_size);
	dpu_wr(g_dpu.mdss, g_dpu.src_xy_off, g_dpu.orig_src_xy);
	dpu_wr(g_dpu.mdss, g_dpu.src_stride_off,
	    (dpu_rd(g_dpu.mdss, g_dpu.src_stride_off) & 0xffff0000u) |
	    (g_dpu.orig_stride & 0xffffu));
	dpu_wr(g_dpu.mdss, g_dpu.src_addr_off, g_dpu.orig_pa);
	dpu_commit();
	g_dpu.cur_pa = g_dpu.orig_pa;
	g_dpu.cur_stride = g_dpu.orig_stride;
	g_dpu.cur_src_size = g_dpu.orig_src_size;
	/* The origin the bootloader left, not 0: a later crop flip only writes
	 * SRC_XY when it differs from this. */
	dpu_size_decode(g_dpu.orig_src_xy, g_dpu.size_enc, &g_dpu.cur_x,
	    &g_dpu.cur_y);
	dpu_size_decode(g_dpu.orig_out_size, g_dpu.size_enc, &g_dpu.cur_w,
	    &g_dpu.cur_h);
	g_dpu.dirty = 0;
}

/*
 * Point the live SSPP at phys (32-bit PA) with the given bytes-per-row and a
 * source window (origin inside that buffer, size in pixels).
 *
 * The window is programmed, not inherited: SRC_SIZE and OUT_SIZE both get
 * (h << 16) | w in the packing the probe decoded, SRC_XY gets the crop origin.
 * Leaving the bootloader's rectangle in place is what made 0101 scan the
 * surface through its scaling (white background, blocky icons). YSTRIDE0 takes
 * the surface pitch and SRC_ADDR the surface PA; OUT_XY stays where the
 * bootloader put the panel rect (0,0).
 *
 * SRC_ADDR is read back, and so is OUT_SIZE when it was written: a write that
 * did not land restores the splash plane and disables the flip for this boot,
 * so the CPU copy keeps the screen.
 */
bool
sweet_display_flip(uint32_t phys, uint32_t stride, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height)
{
	uint32_t size, xy, ystride, latched, out;
	bool wrote_size = false;

	if (!sweet_display_can_flip() || phys == 0 || width == 0 || height == 0) {
		return false;
	}
	if (g_dpu.cur_pa == phys && g_dpu.dirty &&
	    g_dpu.cur_stride == stride && g_dpu.cur_x == x && g_dpu.cur_y == y &&
	    g_dpu.cur_w == width && g_dpu.cur_h == height) {
		return true;
	}
	if (g_dpu.geom_write) {
		size = dpu_size_pack(width, height, g_dpu.size_enc);
		if (size == 0) {
			return false;
		}
		if (dpu_rd(g_dpu.mdss, g_dpu.src_size_off) != size) {
			dpu_wr(g_dpu.mdss, g_dpu.src_size_off, size);
			wrote_size = true;
		}
		if (dpu_rd(g_dpu.mdss, g_dpu.out_size_off) != size) {
			dpu_wr(g_dpu.mdss, g_dpu.out_size_off, size);
			wrote_size = true;
		}
		xy = dpu_xy_pack(x, y, g_dpu.size_enc);
		if (dpu_rd(g_dpu.mdss, g_dpu.src_xy_off) != xy) {
			dpu_wr(g_dpu.mdss, g_dpu.src_xy_off, xy);
		}
		g_dpu.cur_src_size = size;
		g_dpu.cur_x = x;
		g_dpu.cur_y = y;
		g_dpu.cur_w = width;
		g_dpu.cur_h = height;
	}
	if (stride != 0) {
		ystride = dpu_rd(g_dpu.mdss, g_dpu.src_stride_off);
		if ((ystride & 0xffffu) != (stride & 0xffffu)) {
			dpu_wr(g_dpu.mdss, g_dpu.src_stride_off,
			    (ystride & 0xffff0000u) | (stride & 0xffffu));
		}
		g_dpu.cur_stride = stride;
	}
	dpu_wr(g_dpu.mdss, g_dpu.src_addr_off, phys);
	dpu_commit();

	latched = dpu_rd(g_dpu.mdss, g_dpu.src_addr_off);
	out = dpu_rd(g_dpu.mdss, g_dpu.out_size_off);
	if (latched != phys || (wrote_size &&
	    out != dpu_size_pack(width, height, g_dpu.size_enc))) {
		IOLog("SWEET DPU: flip did not stick (addr 0x%08x/0x%08x out "
		    "0x%08x), restoring and disabling\n", latched, phys, out);
		dpu_restore();
		g_dpu.disabled = 1;
		return false;
	}
	/*
	 * The first flips log the whole plane so a wrong picture can be read back
	 * from the log instead of guessed at.
	 */
	if (g_dpu.flips_logged < 8) {
		g_dpu.flips_logged++;
		IOLog("SWEET DPU: flip %u pa 0x%08x stride %u win %ux%u at %u,%u -> "
		    "SRC_SIZE 0x%08x OUT_SIZE 0x%08x SRC_XY 0x%08x OUT_XY 0x%08x "
		    "YSTRIDE 0x%08x%s\n", g_dpu.flips_logged, phys, stride, width,
		    height, x, y, dpu_rd(g_dpu.mdss, g_dpu.src_size_off), out,
		    dpu_rd(g_dpu.mdss, g_dpu.src_xy_off),
		    dpu_rd(g_dpu.mdss, g_dpu.out_xy_off),
		    dpu_rd(g_dpu.mdss, g_dpu.src_stride_off),
		    wrote_size ? " (window programmed)" : "");
	}
	g_dpu.cur_pa = phys;
	g_dpu.dirty = 1;
	return true;
}

void
sweet_display_flip_reset(void)
{
	if (!g_dpu.ready || g_dpu.orig_pa == 0 || !g_dpu.dirty) {
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

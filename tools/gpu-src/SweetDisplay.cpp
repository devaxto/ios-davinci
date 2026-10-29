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

#if defined(SWEET)

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
#define CTL_FLUSH           0x018u
#define CTL_START           0x01Cu
#define CTL_LAYER           0x000u

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
#define SWEET_DPU_LM0_FLUSH     (1u << 6)

struct sweet_dpu_state {
	vm_offset_t mdss;
	uint32_t    sspp_off;
	uint32_t    src_addr_off;   /* absolute from mdss base */
	uint32_t    src_stride_off;
	uint32_t    ctl_off;
	uint32_t    flush_mask;
	uint32_t    orig_pa;
	uint32_t    orig_stride;
	uint32_t    cur_pa;
	uint32_t    cur_stride;
	int         video_mode;     /* INTF/PP counters moved */
	int         ready;
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
		IOLog("SWEET DPU: %s+0x%03x=0x%08x\n", b->name, off, v);
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
				g_dpu.flush_mask = b->flush | SWEET_DPU_LM0_FLUSH;
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
		if (shown < 4) {
			shown++;
			IOLog("SWEET DPU: %s+0x%03x moved 0x%08x -> 0x%08x\n",
			    b->name, 4 * i, before[i], after[i]);
		}
	}
	IOLog("SWEET DPU: %s %u/%u words moved in 30 ms%s\n", b->name, moved, n,
	    moved ? " (live)" : "");
	if (moved != 0 && (b->name[0] == 'i' || b->name[0] == 'p')) {
		/* intf* / pp* counters advancing => video mode. */
		g_dpu.video_mode = 1;
	}
}

static void
dpu_pick_ctl(vm_offset_t base)
{
	static const uint32_t ctls[] = { 0x02000u, 0x02200u, 0x02400u, 0x02600u };

	for (unsigned int i = 0; i < sizeof(ctls) / sizeof(ctls[0]); i++) {
		if (dpu_rd(base, ctls[i] + CTL_LAYER) != 0) {
			g_dpu.ctl_off = ctls[i];
			IOLog("SWEET DPU: using ctl at +0x%x (layer=0x%08x)\n",
			    ctls[i], dpu_rd(base, ctls[i] + CTL_LAYER));
			return;
		}
	}
	g_dpu.ctl_off = ctls[0];
	IOLog("SWEET DPU: no live CTL_LAYER, defaulting to ctl0\n");
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

	dpu_pick_ctl(mdss);

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
			IOLog("SWEET DPU: dsi0+0x%03x=0x%08x\n", off, v);
		}
		IOLog("SWEET DPU: dsi0 0x%08x, %u nonzero words\n", DSI0_PA, found);
	}

	if (g_dpu.ready) {
		IOLog("SWEET DPU: flip ready on %s SRC_ADDR=+0x%x YSTRIDE=+0x%x "
		    "CTL=+0x%x flush=0x%x %s\n",
		    g_dpu.sspp_name ? g_dpu.sspp_name : "?",
		    g_dpu.src_addr_off, g_dpu.src_stride_off, g_dpu.ctl_off,
		    g_dpu.flush_mask,
		    g_dpu.video_mode ? "video" : "cmd-or-idle");
	} else {
		IOLog("SWEET DPU: no SSPP holds the panel PA - flip disabled\n");
	}
}

bool
sweet_display_can_flip(void)
{
	return g_dpu.ready && g_dpu.mdss != 0 && g_dpu.ctl_off != 0;
}

bool
sweet_display_flip(uint32_t phys, uint32_t stride)
{
	uint32_t ystride;

	if (!sweet_display_can_flip() || phys == 0) {
		return false;
	}
	if (phys == g_dpu.cur_pa && stride == g_dpu.cur_stride) {
		return true;
	}

	dpu_wr(g_dpu.mdss, g_dpu.src_addr_off, phys);
	if (stride != 0 && stride != g_dpu.cur_stride) {
		ystride = dpu_rd(g_dpu.mdss, g_dpu.src_stride_off);
		ystride = (ystride & 0xffff0000u) | (stride & 0xffffu);
		dpu_wr(g_dpu.mdss, g_dpu.src_stride_off, ystride);
		g_dpu.cur_stride = stride;
	}
	dpu_wr(g_dpu.mdss, g_dpu.ctl_off + CTL_FLUSH, g_dpu.flush_mask);
	if (!g_dpu.video_mode) {
		/* Command-mode panels need an explicit kick. */
		dpu_wr(g_dpu.mdss, g_dpu.ctl_off + CTL_START, 1u);
	}
	g_dpu.cur_pa = phys;
	return true;
}

void
sweet_display_flip_reset(void)
{
	if (!sweet_display_can_flip() || g_dpu.orig_pa == 0) {
		return;
	}
	(void)sweet_display_flip(g_dpu.orig_pa, g_dpu.orig_stride);
}

#else /* !SWEET */

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
sweet_display_flip(uint32_t phys, uint32_t stride)
{
	(void)phys;
	(void)stride;
	return false;
}

void
sweet_display_flip_reset(void)
{
}

#endif /* SWEET */

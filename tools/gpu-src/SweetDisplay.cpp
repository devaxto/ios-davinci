/*
 * DAVINCI: read-only probe of the display controller (MDSS/SDE) that the
 * bootloader left running.
 *
 * The whole display path today is "CPU writes the ABL splash buffer at
 * 0x9c000000 and the DPU keeps scanning it out". The registers that do that
 * are configured by the bootloader and never touched by XNU, so the probe
 * reads them and logs them: that is how the live layer (SSPP), mixer, CTL
 * and INTF, plus the source address/stride/format, are recovered. Nothing is
 * written and no clock or GDSC is touched, so a wrong guess costs a log, not
 * a panel. A block whose clocks are gated reads as 0/0xffffffff and the dump
 * stops instead of hammering the bus.
 *
 * Register offsets below are the davinci DTB values (qcom,sde-off,
 * qcom,sde-ctl-off, qcom,sde-mixer-off, qcom,sde-sspp-off, qcom,sde-intf-off,
 * qcom,sde-pp-off) relative to mdp_phys 0xae00000. They are not guesses.
 *
 * Next step (docs/GPU.md, "zero-copy present"): with the layer located, a
 * present becomes SRC_ADDR/STRIDE + a CTL flush, i.e. no 10 MB CPU copy at
 * all.
 */
#include "SweetDisplay.h"

#if defined(SWEET)

#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>

extern "C" vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size);

#define MDSS_PA             0x0ae00000u    /* mdp_phys, DTB reg[0] */
#define MDSS_SIZE           0x00084208u    /* covers CTL/LM/SSPP/INTF/PP */
#define MDSS_SDE_OFF        0x01000u       /* qcom,sde-off */
#define DSI0_PA             0x0ae94000u    /* qcom,mdss_dsi_ctrl0 */
#define DSI0_SIZE           0x00001000u

struct sweet_dpu_block {
	const char *name;
	uint32_t    off;
	uint32_t    len;
	bool        live;       /* timing engine: check that it is counting */
};

/*
 * SSDPP 0/1 are the vig pipes, 2/3/4 the dma pipes (qcom,sde-sspp-type).
 * INTF 0 is DisplayPort, 1 and 2 are the two DSI controllers.
 */
static const struct sweet_dpu_block kDpuBlocks[] = {
	{ "top",   0x01000u, 0x45cu, false },
	{ "ctl0",  0x02000u, 0x1e0u, false },
	{ "ctl1",  0x02200u, 0x1e0u, false },
	{ "ctl2",  0x02400u, 0x1e0u, false },
	{ "ctl3",  0x02600u, 0x1e0u, false },
	{ "lm0",   0x45000u, 0x320u, true },
	{ "lm1",   0x46000u, 0x320u, true },
	{ "lm2",   0x47000u, 0x320u, true },
	{ "lm3",   0x48000u, 0x320u, true },
	{ "vig0",  0x05000u, 0x1f0u, false },
	{ "vig1",  0x07000u, 0x1f0u, false },
	{ "dma0",  0x25000u, 0x1f0u, false },
	{ "dma1",  0x27000u, 0x1f0u, false },
	{ "dma2",  0x29000u, 0x1f0u, false },
	{ "intf0", 0x6b000u, 0x100u, true },
	{ "intf1", 0x6b800u, 0x100u, true },
	{ "intf2", 0x6c000u, 0x100u, true },
	{ "intf3", 0x6c800u, 0x100u, true },
	{ "pp0",   0x71000u, 0x0d4u, true },
	{ "pp1",   0x71800u, 0x0d4u, true },
	{ "pp2",   0x72000u, 0x0d4u, true },
	{ "pp3",   0x72800u, 0x0d4u, true },
};

#define SWEET_DPU_LIVE_WORDS    64      /* one INTF/PP block, for the liveness check */

static uint32_t
dpu_rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

/* Nonzero words only: a zeroed register is an unconfigured one. */
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
		} else if (v == fb_stride) {
			IOLog("SWEET DPU: %s+0x%03x is the panel stride\n",
			    b->name, off);
		}
	}
}

/*
 * Which INTF/PP is actually scanning: a live timing engine moves its frame and
 * line counters. Two reads 30 ms apart settle it, which is also how a mixer
 * that only holds a stale config is told apart from the one in use.
 */
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

	mdss = ml_io_map(MDSS_PA, MDSS_SIZE);
	if (mdss == 0) {
		IOLog("SWEET DPU: MDSS map failed\n");
		return;
	}

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

	dsi = ml_io_map(DSI0_PA, DSI0_SIZE);
	if (dsi == 0) {
		IOLog("SWEET DPU: DSI0 map failed\n");
		return;
	}
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

#endif /* SWEET */

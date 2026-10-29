/*
 * DAVINCI / SWEET: Adreno 618 bring-up.
 *
 * Boot-arg sweetgpu:
 *   0 = off
 *   1 = read-only probe (default) - never enables rails
 *   2 = enable CX GDSC + GCC/GPUCC clocks, read RBBM_CHIP_ID
 *   3 = also allow IOKit AGXAccelerator stub (SweetAccelerator.cpp)
 *   4 = load embedded a630 GMU firmware into ITCM and start the CM3
 *       (a618 = legacy HFI path). Still NOT Metal: proves the GMU runs.
 *
 * sweetgpu=3 can make Metal prefer a stub that is not yet a real driver;
 * keep the working software compositor unless you are tracing selectors.
 * See docs/GPU.md and docs/METAL-PORT.md.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include "sweet_gpu_fw.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
void sweet_gpu_probe(void);
int sweet_gpu_mode(void);
bool sweet_gpu_is_on(void);
uint32_t sweet_gpu_chip_id(void);
bool sweet_gpu_gmu_up(void);

#define GCC_PA                  0x00100000u
#define GCC_SIZE                0x00090000u
#define CBCR_CLK_OFF            (1u << 31)
#define CBCR_ENABLE             (1u << 0)
#define CBCR_SLEEP_WAKE         ((0xfu << 4) | (0xfu << 8))

#define GCC_DDRSS_GPU_AXI       0x4452cu
#define GCC_GPU_CFG_AHB         0x71004u
#define GCC_GPU_MEMNOC_GFX      0x7100cu
#define GCC_GPU_SNOC_DVM_GFX    0x71014u

#define GPUCC_PA                0x05090000u
#define GPUCC_SIZE              0x00009000u
#define GPUCC_GX_GDSCR          0x100cu
#define GPUCC_CX_GDSCR          0x106cu
#define GPUCC_CXO_AON_CBCR      0x1004u
#define GPUCC_CRC_AHB_CBCR      0x107cu
#define GPUCC_CX_SNOC_DVM_CBCR  0x108cu
#define GPUCC_CX_GMU_CBCR       0x1098u
#define GPUCC_CXO_CBCR          0x109cu
#define GDSC_SW_COLLAPSE        (1u << 0)
#define GDSC_PWR_ON             (1u << 31)

#define GPU_PA                  0x05000000u
#define GPU_SIZE                0x00040000u
#define REG_A6XX_RBBM_CHIP_ID   0x0000u
#define REG_A6XX_RBBM_STATUS    0x0210u

/*
 * GMU aperture (linux a6xx_gmu.xml.h dword offsets, byte = off << 2).
 * Map the full 0x31000 window - ITCM and HFI sit past the first page.
 */
#define GMU_PA                  0x0506a000u
#define GMU_SIZE                0x00031000u
#define REG_A6XX_GMU_CM3_ITCM_START       0x00000c00u
#define REG_A6XX_GMU_CM3_DTCM_START       0x00001c00u
#define REG_A6XX_GMU_CM3_SYSRESET         0x00005000u
#define REG_A6XX_GMU_CM3_BOOT_CONFIG      0x00005001u
#define REG_A6XX_GMU_CM3_FW_INIT_RESULT   0x0000501cu
#define REG_A6XX_GMU_HFI_CTRL_STATUS      0x00005180u
#define REG_A6XX_GMU_HFI_QTBL_INFO        0x00005184u
#define REG_A6XX_GMU_HFI_QTBL_ADDR        0x00005185u
#define REG_A6XX_GMU_HFI_CTRL_INIT        0x00005186u
#define REG_A6XX_GMU_GENERAL_7            0x000051ccu
#define REG_A6XX_GMU_AHB_FENCE_RANGE_0    0x00009311u
#define REG_A6XX_GMU_AHB_FENCE_RANGE_1    0x00009312u

static const struct {
	const char *name;
	uint32_t    off;
} gpu_gcc_cbcr[] = {
	{ "cfg-ahb",     GCC_GPU_CFG_AHB },
	{ "memnoc-gfx",  GCC_GPU_MEMNOC_GFX },
	{ "snoc-dvm",    GCC_GPU_SNOC_DVM_GFX },
	{ "ddrss-axi",   GCC_DDRSS_GPU_AXI },
};

static const struct {
	const char *name;
	uint32_t    off;
} gpu_cc_cbcr[] = {
	{ "cxo-aon",    GPUCC_CXO_AON_CBCR },
	{ "crc-ahb",    GPUCC_CRC_AHB_CBCR },
	{ "cx-snoc",    GPUCC_CX_SNOC_DVM_CBCR },
	{ "cx-gmu",     GPUCC_CX_GMU_CBCR },
	{ "cxo",        GPUCC_CXO_CBCR },
};

static int g_mode = 1;
static bool g_on = false;
static bool g_gmu_up = false;
static uint32_t g_chip_id = 0;
static vm_offset_t g_gcc, g_gpucc, g_gpu, g_gmu;

static uint32_t
gpu_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

static void
gpu_wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile ("dmb osh" ::: "memory");
}

/* GMU dword register -> byte offset (linux gmu_write). */
static uint32_t
gmu_rd(uint32_t reg)
{
	return gpu_rd(g_gmu, reg << 2);
}

static void
gmu_wr(uint32_t reg, uint32_t v)
{
	gpu_wr(g_gmu, reg << 2, v);
}

static void
gmu_wr_bulk(uint32_t reg, const uint32_t *data, uint32_t nbytes)
{
	volatile uint32_t *dst = (volatile uint32_t *)(g_gmu + (reg << 2));
	uint32_t n = nbytes / 4;

	__asm__ volatile ("dmb oshst" ::: "memory");
	for (uint32_t i = 0; i < n; i++) {
		dst[i] = data[i];
	}
	__asm__ volatile ("dmb osh" ::: "memory");
}

int
sweet_gpu_mode(void)
{
	return g_mode;
}

bool
sweet_gpu_is_on(void)
{
	return g_on;
}

bool
sweet_gpu_gmu_up(void)
{
	return g_gmu_up;
}

uint32_t
sweet_gpu_chip_id(void)
{
	return g_chip_id;
}

static void
gpu_dump_state(const char *tag)
{
	uint32_t cx = gpu_rd(g_gpucc, GPUCC_CX_GDSCR);
	uint32_t gx = gpu_rd(g_gpucc, GPUCC_GX_GDSCR);
	printf("SWEET GPU: %s GDSC cx 0x%08x (%s) gx 0x%08x (%s)\n", tag,
	    cx, (cx & GDSC_PWR_ON) ? "on" : "off",
	    gx, (gx & GDSC_PWR_ON) ? "on" : "off");
	for (unsigned i = 0; i < sizeof(gpu_gcc_cbcr) / sizeof(gpu_gcc_cbcr[0]); i++) {
		uint32_t v = gpu_rd(g_gcc, gpu_gcc_cbcr[i].off);
		printf("SWEET GPU: %s gcc %-10s CBCR 0x%08x (%s)\n", tag,
		    gpu_gcc_cbcr[i].name, v,
		    (v & CBCR_CLK_OFF) ? "off" : "on");
	}
	for (unsigned i = 0; i < sizeof(gpu_cc_cbcr) / sizeof(gpu_cc_cbcr[0]); i++) {
		uint32_t v = gpu_rd(g_gpucc, gpu_cc_cbcr[i].off);
		printf("SWEET GPU: %s gpucc %-8s CBCR 0x%08x (%s)\n", tag,
		    gpu_cc_cbcr[i].name, v,
		    (v & CBCR_CLK_OFF) ? "off" : "on");
	}
}

static bool
gpu_cbcr_enable(vm_offset_t base, uint32_t off, uint32_t extra, const char *name)
{
	uint32_t v = gpu_rd(base, off);
	gpu_wr(base, off, v | CBCR_ENABLE | extra);
	for (int i = 0; i < 1000; i++) {
		v = gpu_rd(base, off);
		if (!(v & CBCR_CLK_OFF)) {
			return true;
		}
		delay(10);
	}
	printf("SWEET GPU: CBCR %s stuck off (0x%08x)\n", name, v);
	return false;
}

static bool
gpu_cx_enable(void)
{
	uint32_t v = gpu_rd(g_gpucc, GPUCC_CX_GDSCR);
	if (v & GDSC_PWR_ON) {
		return true;
	}
	gpu_wr(g_gpucc, GPUCC_CX_GDSCR, v & ~GDSC_SW_COLLAPSE);
	for (int i = 0; i < 1000; i++) {
		v = gpu_rd(g_gpucc, GPUCC_CX_GDSCR);
		if (v & GDSC_PWR_ON) {
			return true;
		}
		delay(10);
	}
	printf("SWEET GPU: CX_GDSC did not power on (0x%08x)\n", v);
	return false;
}

static bool
gpu_power_on(void)
{
	printf("SWEET GPU: power-on CX + clocks (sweetgpu=%d)\n", g_mode);
	if (!gpu_cbcr_enable(g_gcc, GCC_GPU_CFG_AHB, 0, "cfg-ahb") ||
	    !gpu_cbcr_enable(g_gcc, GCC_GPU_MEMNOC_GFX, 0, "memnoc-gfx") ||
	    !gpu_cbcr_enable(g_gcc, GCC_GPU_SNOC_DVM_GFX, 0, "snoc-dvm") ||
	    !gpu_cbcr_enable(g_gcc, GCC_DDRSS_GPU_AXI, 0, "ddrss-axi")) {
		return false;
	}
	if (!gpu_cbcr_enable(g_gpucc, GPUCC_CXO_AON_CBCR, 0, "cxo-aon") ||
	    !gpu_cbcr_enable(g_gpucc, GPUCC_CRC_AHB_CBCR, 0, "crc-ahb") ||
	    !gpu_cbcr_enable(g_gpucc, GPUCC_CX_SNOC_DVM_CBCR, 0, "cx-snoc")) {
		return false;
	}
	if (!gpu_cx_enable()) {
		return false;
	}
	if (!gpu_cbcr_enable(g_gpucc, GPUCC_CX_GMU_CBCR, CBCR_SLEEP_WAKE, "cx-gmu") ||
	    !gpu_cbcr_enable(g_gpucc, GPUCC_CXO_CBCR, 0, "cxo")) {
		return false;
	}
	return true;
}

static void
gpu_read_chip(void)
{
	g_gpu = ml_io_map(GPU_PA, GPU_SIZE);
	if (g_gpu == 0) {
		printf("SWEET GPU: GPU map failed\n");
		return;
	}
	g_chip_id = gpu_rd(g_gpu, REG_A6XX_RBBM_CHIP_ID);
	printf("SWEET GPU: RBBM_CHIP_ID 0x%08x RBBM_STATUS 0x%08x\n",
	    g_chip_id, gpu_rd(g_gpu, REG_A6XX_RBBM_STATUS));
	g_on = (g_chip_id != 0 && g_chip_id != 0xffffffffu);
}

/*
 * a618 = legacy GMU (linux a6xx_gmu.c): dump a630_gmu.bin into ITCM, drop
 * CM3 reset, wait for FW_INIT_RESULT. RPMH/PDC is still TODO - if the poll
 * times out we log and leave CX up so chip_id stays readable.
 */
static bool
gpu_gmu_start_legacy(void)
{
	uint32_t val, mask, want;
	int i;

	g_gmu = ml_io_map(GMU_PA, GMU_SIZE);
	if (g_gmu == 0) {
		printf("SWEET GPU: GMU map failed (need 0x%x)\n", GMU_SIZE);
		return false;
	}
	if (SWEET_GPU_GMU_FW_BYTES == 0 || SWEET_GPU_GMU_FW_BYTES > 0x8000u) {
		printf("SWEET GPU: embedded GMU fw missing/too big (%u)\n",
		    SWEET_GPU_GMU_FW_BYTES);
		return false;
	}

	printf("SWEET GPU: loading GMU fw %u bytes into ITCM (legacy a618)\n",
	    SWEET_GPU_GMU_FW_BYTES);
	/* TCM retention (pre-a650). */
	gmu_wr(REG_A6XX_GMU_GENERAL_7, 1);
	gmu_wr_bulk(REG_A6XX_GMU_CM3_ITCM_START, sweet_gpu_gmu_fw,
	    SWEET_GPU_GMU_FW_BYTES);

	gmu_wr(REG_A6XX_GMU_CM3_FW_INIT_RESULT, 0);
	gmu_wr(REG_A6XX_GMU_CM3_BOOT_CONFIG, 0x02);
	/* Fence range for a6xx (linux a6xx_gmu_fw_start). */
	gmu_wr(REG_A6XX_GMU_AHB_FENCE_RANGE_0, (0xau << 18) | 0xa0u);
	gmu_wr(REG_A6XX_GMU_AHB_FENCE_RANGE_1, 0);

	val = gmu_rd(REG_A6XX_GMU_CM3_DTCM_START + 0xff8u);
	if (val <= 0x20010004u) {
		mask = 0xffffffffu;
		want = 0xbabefaceu;
	} else {
		mask = 0x1ffu;
		want = 0x100u;
	}
	printf("SWEET GPU: CM3 boot tip 0x%08x -> expect init 0x%08x\n", val, want);

	gmu_wr(REG_A6XX_GMU_CM3_SYSRESET, 1);
	delay(100);
	gmu_wr(REG_A6XX_GMU_CM3_SYSRESET, 0);

	for (i = 0; i < 10000; i++) {
		val = gmu_rd(REG_A6XX_GMU_CM3_FW_INIT_RESULT);
		if ((val & mask) == want) {
			g_gmu_up = true;
			printf("SWEET GPU: GMU CM3 UP fw_init=0x%08x (%d us)\n",
			    val, i * 10);
			return true;
		}
		delay(10);
	}
	printf("SWEET GPU: GMU CM3 fw_init TIMEOUT (last 0x%08x) - "
	    "RPMH/PDC sequencing still TODO\n", val);
	return false;
}

void
sweet_gpu_probe(void)
{
	g_mode = 1;
	PE_parse_boot_argn("sweetgpu", &g_mode, sizeof(g_mode));
	if (g_mode <= 0) {
		return;
	}

	g_gcc = ml_io_map(GCC_PA, GCC_SIZE);
	if (g_gcc == 0) {
		printf("SWEET GPU: GCC map failed\n");
		return;
	}
	g_gpucc = ml_io_map(GPUCC_PA, GPUCC_SIZE);
	if (g_gpucc == 0) {
		printf("SWEET GPU: GPUCC map failed\n");
		return;
	}

	printf("SWEET GPU: Adreno 618 (davinci/sweet) mode %d\n", g_mode);
	gpu_dump_state("pre");

	if (g_mode >= 2) {
		if (!gpu_power_on()) {
			printf("SWEET GPU: power-on FAILED; leaving rails as-is\n");
			gpu_dump_state("fail");
			return;
		}
		gpu_dump_state("post");
		gpu_read_chip();
		if (g_on) {
			printf("SWEET GPU: CX path UP chip_id=0x%08x\n", g_chip_id);
		}
		if (g_mode >= 4 && g_on) {
			if (gpu_gmu_start_legacy()) {
				printf("SWEET GPU: Metal track B1 milestone - GMU firmware running\n");
			}
		} else if (g_on) {
			printf("SWEET GPU: GX/GMU/submit still TODO "
			    "(sweetgpu=4 loads GMU fw; Metal needs METAL-PORT.md B2+)\n");
		}
		return;
	}

	/* mode 1: read-only */
	{
		uint32_t cx = gpu_rd(g_gpucc, GPUCC_CX_GDSCR);
		uint32_t cfg = gpu_rd(g_gcc, GCC_GPU_CFG_AHB);
		uint32_t mem = gpu_rd(g_gcc, GCC_GPU_MEMNOC_GFX);
		bool cx_on = (cx & GDSC_PWR_ON) != 0;
		bool cfg_on = !(cfg & CBCR_CLK_OFF);
		bool memnoc_on = !(mem & CBCR_CLK_OFF);
		if (!cx_on || !cfg_on || !memnoc_on) {
			printf("SWEET GPU: CX/clks gated; GPU MMIO not read "
			    "(use sweetgpu=2 to power on, =4 for GMU fw)\n");
			return;
		}
		gpu_read_chip();
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */

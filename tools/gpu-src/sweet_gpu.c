/*
 * DAVINCI / SWEET: Adreno 618 bring-up.
 *
 * Boot-arg sweetgpu:
 *   0 = off
 *   1 = read-only probe (default) - never enables rails
 *   2 = enable CX GDSC + GCC/GPUCC clocks, read RBBM_CHIP_ID
 *   3 = also allow IOKit AGXAccelerator stub (SweetAccelerator.cpp)
 *   4 = load embedded a618 GMU firmware into ITCM and start the CM3
 *       (Xiaomi vendor a618_gmu.bin; legacy HFI). Still NOT Metal.
 *   5 = after GMU: HFI qtbl + OOB GX/GPU_SET + SQE + CP ring + ME_INIT/NOP
 *       (Metal track B2 start). Does NOT enable the AGX stub.
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
bool sweet_gpu_sqe_up(void);

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
/* Legacy probe used byte offsets for these two; keep for log continuity. */
#define REG_A6XX_RBBM_CHIP_ID_BYTE   0x0000u
#define REG_A6XX_RBBM_STATUS_BYTE    0x0210u

/*
 * A6xx register indices (linux a6xx_regs.h). Access as (reg << 2) bytes.
 */
#define REG_A6XX_RBBM_STATUS              0x00000210u
#define REG_A6XX_RBBM_STATUS3             0x00000213u
#define REG_A6XX_RBBM_SECVID_TSB_CNTL     0x0000f803u
#define REG_A6XX_RBBM_SECVID_TRUST_CNTL   0x0000f400u
#define REG_A6XX_CP_RB_BASE               0x00000800u
#define REG_A6XX_CP_RB_CNTL               0x00000802u
#define REG_A6XX_CP_RB_RPTR               0x00000806u
#define REG_A6XX_CP_RB_WPTR               0x00000807u
#define REG_A6XX_CP_SQE_CNTL              0x00000808u
#define REG_A6XX_CP_SQE_INSTR_BASE        0x00000830u
#define REG_A6XX_CP_AHB_CNTL              0x00000845u
#define AXXX_CP_RB_CNTL_NO_UPDATE        0x08000000u
#define A6XX_RBBM_STATUS3_SMMU_STALLED    0x01000000u

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
#define REG_A6XX_GMU_HOST2GMU_INTR_SET    0x00005194u
#define REG_A6XX_GMU_GMU2HOST_INTR_INFO   0x00005192u
#define REG_A6XX_GMU_GMU2HOST_INTR_CLR    0x00005191u
#define REG_A6XX_GMU_BOOT_SLUMBER_OPTION  0x000023f8u
#define REG_A6XX_GMU_GX_VOTE_IDX          0x000023f9u
#define REG_A6XX_GMU_MX_VOTE_IDX          0x000023fau
#define REG_A6XX_GMU_GX_SPTPRAC_POWER_CONTROL 0x00000081u
#define REG_A6XX_GMU_SPTPRAC_PWR_CLK_STATUS   0x000050d0u
#define REG_A6XX_GPU_GMU_CX_GMU_PWR_COL_CP_RESP 0x00005101u

/* Legacy a618 OOB bits (linux a6xx_gmu_oob_bits). */
#define GMU_OOB_GPU_SET_BIT       16
#define GMU_OOB_GPU_SET_ACK       24
#define GMU_OOB_BOOT_SLUMBER_BIT  22
#define GMU_OOB_BOOT_SLUMBER_ACK  30

/*
 * GPU/GMU-visible scratch in the unused low DDR bank, immediately after the
 * ABL 24 MB splash (0x9c000000). XNU highmem does not manage this bank, so a
 * fixed carve is safe for bring-up. HFI_QTBL_ADDR is 32-bit only.
 */
#define SWEET_GPU_MEM_PA          0x9d800000u
#define SWEET_GPU_MEM_SIZE        0x00040000u /* 256 KiB */
#define SWEET_GPU_HFI_OFF         0x0000u
#define SWEET_GPU_HFI_SIZE        0x4000u     /* 16 KiB */
#define SWEET_GPU_SQE_OFF         0x4000u
#define SWEET_GPU_SQE_SIZE        0x9000u     /* 36 KiB */
#define SWEET_GPU_RB_OFF          0xd000u
#define SWEET_GPU_RB_SIZE         0x8000u     /* 32 KiB */
#define SWEET_GPU_RB_DWORDS       (SWEET_GPU_RB_SIZE / 4u)

#define CP_TYPE7_PKT              0x70000000u
#define CP_ME_INIT                72u
#define CP_NOP                    16u

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
static bool g_sqe_up = false;
static uint32_t g_chip_id = 0;
static vm_offset_t g_gcc, g_gpucc, g_gpu, g_gmu, g_mem;
static uint32_t *g_rb;
static uint32_t g_rb_wptr;

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

/* A6xx GPU dword register index (linux gpu_write). */
static uint32_t
agpu_rd(uint32_t reg)
{
	return gpu_rd(g_gpu, reg << 2);
}

static void
agpu_wr(uint32_t reg, uint32_t v)
{
	gpu_wr(g_gpu, reg << 2, v);
}

static void
agpu_wr64(uint32_t lo, uint64_t v)
{
	agpu_wr(lo, (uint32_t)v);
	agpu_wr(lo + 1u, (uint32_t)(v >> 32));
}

static uint32_t
pm4_parity(uint32_t val)
{
	uint32_t x = val ^ (val >> 4) ^ (val >> 8) ^ (val >> 12) ^
	    (val >> 16) ^ (val >> 20) ^ (val >> 24) ^ (val >> 28);
	return (0x9669u >> (x & 0xfu)) & 1u;
}

static uint32_t
cp_type7(uint32_t opcode, uint32_t cnt)
{
	return CP_TYPE7_PKT | (cnt & 0xffffu) | (pm4_parity(cnt) << 15) |
	    ((opcode & 0x7fu) << 16) | (pm4_parity(opcode) << 23);
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

bool
sweet_gpu_sqe_up(void)
{
	return g_sqe_up;
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
	g_chip_id = gpu_rd(g_gpu, REG_A6XX_RBBM_CHIP_ID_BYTE);
	printf("SWEET GPU: RBBM_CHIP_ID 0x%08x RBBM_STATUS 0x%08x\n",
	    g_chip_id, gpu_rd(g_gpu, REG_A6XX_RBBM_STATUS_BYTE));
	g_on = (g_chip_id != 0 && g_chip_id != 0xffffffffu);
}

static bool
gpu_mem_map(void)
{
	if (g_mem != 0) {
		return true;
	}
	g_mem = ml_io_map(SWEET_GPU_MEM_PA, SWEET_GPU_MEM_SIZE);
	if (g_mem == 0) {
		printf("SWEET GPU: scratch map failed pa 0x%08x\n", SWEET_GPU_MEM_PA);
		return false;
	}
	printf("SWEET GPU: scratch pa 0x%08x size 0x%x mapped\n",
	    SWEET_GPU_MEM_PA, SWEET_GPU_MEM_SIZE);
	return true;
}

/*
 * Minimal legacy HFI queue table (linux a6xx_hfi_init). Two 4K queues.
 * GMU sees the same physical address (SMMU bypass / identity assumed for
 * bring-up; STATUS3 will tell us if that is wrong).
 */
static void
gpu_hfi_init_table(void)
{
	volatile uint32_t *hfi = (volatile uint32_t *)(g_mem + SWEET_GPU_HFI_OFF);
	uint32_t table_bytes = 6u * 4u + 2u * 12u * 4u; /* table + 2 headers */
	uint32_t q0_iova = SWEET_GPU_MEM_PA + SWEET_GPU_HFI_OFF + 0x1000u;
	uint32_t q1_iova = SWEET_GPU_MEM_PA + SWEET_GPU_HFI_OFF + 0x2000u;
	unsigned i;

	for (i = 0; i < SWEET_GPU_HFI_SIZE / 4u; i++) {
		hfi[i] = 0;
	}

	/* a6xx_hfi_queue_table_header */
	hfi[0] = 0;                 /* version */
	hfi[1] = table_bytes;       /* size (bytes in linux; some firmwares want dwords - both logged) */
	hfi[2] = 6;                 /* qhdr0_offset in dwords */
	hfi[3] = 12;                /* qhdr_size in dwords */
	hfi[4] = 2;                 /* num_queues */
	hfi[5] = 2;                 /* active_queues */

	/* queue 0 header @ dword 6 (command) */
	hfi[6]  = 1;                /* status */
	hfi[7]  = q0_iova;          /* iova */
	hfi[8]  = (10u << 8) | 0;   /* type */
	hfi[9]  = 0x1000u / 4u;     /* size dwords */
	hfi[10] = 0;                /* msg_size */
	hfi[11] = 0;                /* dropped */
	hfi[12] = 1;                /* rx_watermark */
	hfi[13] = 1;                /* tx_watermark */
	hfi[14] = 1;                /* rx_request */
	hfi[15] = 0;                /* tx_request */
	hfi[16] = 0;                /* read_index */
	hfi[17] = 0;                /* write_index */

	/* queue 1 header @ dword 18 (response); legacy id=4 */
	hfi[18] = 1;
	hfi[19] = q1_iova;
	hfi[20] = (10u << 8) | 4;
	hfi[21] = 0x1000u / 4u;
	hfi[22] = 0;
	hfi[23] = 0;
	hfi[24] = 1;
	hfi[25] = 1;
	hfi[26] = 1;
	hfi[27] = 0;
	hfi[28] = 0;
	hfi[29] = 0;

	__asm__ volatile ("dmb osh" ::: "memory");
}

static bool
gpu_gmu_set_oob(uint32_t set_bit, uint32_t ack_bit, const char *name)
{
	uint32_t val;
	int i;

	gmu_wr(REG_A6XX_GMU_HOST2GMU_INTR_SET, 1u << set_bit);
	for (i = 0; i < 10000; i++) {
		val = gmu_rd(REG_A6XX_GMU_GMU2HOST_INTR_INFO);
		if (val & (1u << ack_bit)) {
			gmu_wr(REG_A6XX_GMU_GMU2HOST_INTR_CLR, 1u << ack_bit);
			printf("SWEET GPU: OOB %s ack ok (%d us) info 0x%08x\n",
			    name, i * 10, val);
			return true;
		}
		delay(10);
	}
	printf("SWEET GPU: OOB %s TIMEOUT info 0x%08x\n",
	    name, gmu_rd(REG_A6XX_GMU_GMU2HOST_INTR_INFO));
	return false;
}

static void
gpu_gmu_clear_oob(uint32_t clear_bit)
{
	gmu_wr(REG_A6XX_GMU_HOST2GMU_INTR_SET, 1u << clear_bit);
}

/*
 * a618 = legacy GMU (linux a6xx_gmu.c): dump vendor a618_gmu.bin into ITCM,
 * drop CM3 reset, wait for FW_INIT_RESULT. For sweetgpu>=5 also plant an HFI
 * queue table before releasing reset so OOB/GX can talk.
 */
static bool
gpu_gmu_start_legacy(bool with_hfi)
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

	if (with_hfi) {
		if (!gpu_mem_map()) {
			return false;
		}
		gpu_hfi_init_table();
	}

	printf("SWEET GPU: loading GMU fw %u bytes into ITCM (legacy a618)%s\n",
	    SWEET_GPU_GMU_FW_BYTES, with_hfi ? " +HFI" : "");
	/* TCM retention (pre-a650). */
	gmu_wr(REG_A6XX_GMU_GENERAL_7, 1);
	gmu_wr_bulk(REG_A6XX_GMU_CM3_ITCM_START, sweet_gpu_gmu_fw,
	    SWEET_GPU_GMU_FW_BYTES);

	gmu_wr(REG_A6XX_GMU_CM3_FW_INIT_RESULT, 0);
	gmu_wr(REG_A6XX_GMU_CM3_BOOT_CONFIG, 0x02);

	if (with_hfi) {
		gmu_wr(REG_A6XX_GMU_HFI_QTBL_ADDR,
		    SWEET_GPU_MEM_PA + SWEET_GPU_HFI_OFF);
		gmu_wr(REG_A6XX_GMU_HFI_QTBL_INFO, 1);
		/* linux a6xx_gmu_fw_start fence range */
		gmu_wr(REG_A6XX_GMU_AHB_FENCE_RANGE_0,
		    (1u << 31) | (0xau << 18) | 0xa0u);
	} else {
		gmu_wr(REG_A6XX_GMU_AHB_FENCE_RANGE_0, (0xau << 18) | 0xa0u);
	}
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
	gmu_wr(REG_A6XX_GPU_GMU_CX_GMU_PWR_COL_CP_RESP, 0);
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

static bool
gpu_gx_rail_on(void)
{
	uint32_t val;
	int i;

	/* Prefer GMU OOB BOOT_SLUMBER (linux a6xx_gmu_gfx_rail_on). */
	gmu_wr(REG_A6XX_GMU_BOOT_SLUMBER_OPTION, 0);
	gmu_wr(REG_A6XX_GMU_GX_VOTE_IDX, 0);
	gmu_wr(REG_A6XX_GMU_MX_VOTE_IDX, 0);
	if (gpu_gmu_set_oob(GMU_OOB_BOOT_SLUMBER_BIT, GMU_OOB_BOOT_SLUMBER_ACK,
	    "BOOT_SLUMBER")) {
		gpu_gmu_clear_oob(GMU_OOB_BOOT_SLUMBER_ACK);
	}

	/* Also poke SPTPRAC (CPU-side GX bring-up on legacy). */
	gmu_wr(REG_A6XX_GMU_GX_SPTPRAC_POWER_CONTROL, 0x778000u);
	for (i = 0; i < 1000; i++) {
		val = gmu_rd(REG_A6XX_GMU_SPTPRAC_PWR_CLK_STATUS);
		if ((val & 0x38u) == 0x28u) {
			printf("SWEET GPU: SPTPRAC GX up status 0x%08x (%d us)\n",
			    val, i * 10);
			return true;
		}
		delay(10);
	}
	val = gmu_rd(REG_A6XX_GMU_SPTPRAC_PWR_CLK_STATUS);
	printf("SWEET GPU: SPTPRAC poll last 0x%08x (GX may still be off)\n", val);

	val = gpu_rd(g_gpucc, GPUCC_GX_GDSCR);
	printf("SWEET GPU: GX_GDSC now 0x%08x (%s)\n",
	    val, (val & GDSC_PWR_ON) ? "on" : "off");
	return (val & GDSC_PWR_ON) != 0;
}

static bool
gpu_hfi_ctrl_start(void)
{
	uint32_t val;
	int i;

	gmu_wr(REG_A6XX_GMU_HFI_CTRL_INIT, 1);
	for (i = 0; i < 10000; i++) {
		val = gmu_rd(REG_A6XX_GMU_HFI_CTRL_STATUS);
		if (val & 1u) {
			printf("SWEET GPU: HFI ctrl UP status 0x%08x (%d us)\n",
			    val, i * 10);
			return true;
		}
		delay(10);
	}
	printf("SWEET GPU: HFI ctrl TIMEOUT status 0x%08x\n",
	    gmu_rd(REG_A6XX_GMU_HFI_CTRL_STATUS));
	return false;
}

static void
rb_emit(uint32_t v)
{
	g_rb[g_rb_wptr % SWEET_GPU_RB_DWORDS] = v;
	g_rb_wptr = (g_rb_wptr + 1u) % SWEET_GPU_RB_DWORDS;
}

/*
 * Metal track B2: OOB GPU_SET, load SQE into low-bank scratch, program the
 * CP ring, submit ME_INIT + NOP. Success = RPTR catches WPTR without an
 * SMMU stall. A clear/blit into splash is the next milestone.
 */
static bool
gpu_sqe_ring_bringup(void)
{
	uint32_t *sqe_dst;
	uint32_t sqe_bytes, rb_cntl, status, status3, rptr, wptr;
	uint64_t sqe_pa, rb_pa;
	int i;

	if (!g_gmu_up) {
		printf("SWEET GPU: SQE skipped (GMU not up)\n");
		return false;
	}
	if (SWEET_GPU_SQE_FW_BYTES <= SWEET_GPU_SQE_PAYLOAD_OFF ||
	    SWEET_GPU_SQE_PAYLOAD_BYTES > SWEET_GPU_SQE_SIZE) {
		printf("SWEET GPU: embedded SQE bad (%u)\n", SWEET_GPU_SQE_FW_BYTES);
		return false;
	}
	if (!gpu_mem_map()) {
		return false;
	}

	if (!gpu_gx_rail_on()) {
		printf("SWEET GPU: GX rail not confirmed; continuing SQE attempt\n");
	}
	(void)gpu_hfi_ctrl_start();

	if (!gpu_gmu_set_oob(GMU_OOB_GPU_SET_BIT, GMU_OOB_GPU_SET_ACK, "GPU_SET")) {
		printf("SWEET GPU: GPU_SET failed - SQE may hang if GX gated\n");
	}

	/* Minimal secure-mode exit (no ZAP shader). */
	agpu_wr(REG_A6XX_RBBM_SECVID_TSB_CNTL, 0);
	agpu_wr(REG_A6XX_RBBM_SECVID_TRUST_CNTL, 0);
	agpu_wr(REG_A6XX_CP_AHB_CNTL, 1);

	sqe_bytes = SWEET_GPU_SQE_PAYLOAD_BYTES;
	sqe_dst = (uint32_t *)(g_mem + SWEET_GPU_SQE_OFF);
	/* Payload is already LE words in sweet_gpu_sqe_fw[]; skip dword 0. */
	for (i = 0; i < (int)(sqe_bytes / 4u); i++) {
		sqe_dst[i] = sweet_gpu_sqe_fw[1 + i];
	}
	__asm__ volatile ("dmb osh" ::: "memory");
	sqe_pa = SWEET_GPU_MEM_PA + SWEET_GPU_SQE_OFF;
	printf("SWEET GPU: SQE %u bytes @ pa 0x%llx\n",
	    sqe_bytes, (unsigned long long)sqe_pa);

	g_rb = (uint32_t *)(g_mem + SWEET_GPU_RB_OFF);
	for (i = 0; i < (int)SWEET_GPU_RB_DWORDS; i++) {
		g_rb[i] = 0;
	}
	g_rb_wptr = 0;
	rb_pa = SWEET_GPU_MEM_PA + SWEET_GPU_RB_OFF;

	agpu_wr64(REG_A6XX_CP_SQE_INSTR_BASE, sqe_pa);
	agpu_wr64(REG_A6XX_CP_RB_BASE, rb_pa);
	/* BUFSZ=log2(32K/8)=12, BLKSZ=log2(32/8)=2, NO_UPDATE (no apriv). */
	rb_cntl = 12u | (2u << 8) | AXXX_CP_RB_CNTL_NO_UPDATE;
	agpu_wr(REG_A6XX_CP_RB_CNTL, rb_cntl);
	agpu_wr(REG_A6XX_CP_SQE_CNTL, 1);
	printf("SWEET GPU: SQE_CNTL=1 RB pa 0x%llx cntl 0x%08x\n",
	    (unsigned long long)rb_pa, rb_cntl);

	/* linux a6xx_cp_init */
	rb_emit(cp_type7(CP_ME_INIT, 8));
	rb_emit(0x0000002fu);
	rb_emit(0x00000003u);
	rb_emit(0x20000000u);
	rb_emit(0);
	rb_emit(0);
	rb_emit(0);
	rb_emit(0);
	rb_emit(0);
	/* One NOP so a live SQE has a trivial packet after ME_INIT. */
	rb_emit(cp_type7(CP_NOP, 1));
	rb_emit(0);
	__asm__ volatile ("dmb osh" ::: "memory");

	wptr = g_rb_wptr;
	agpu_wr(REG_A6XX_CP_RB_WPTR, wptr);
	printf("SWEET GPU: submitted ME_INIT+NOP wptr=%u\n", wptr);

	for (i = 0; i < 10000; i++) {
		rptr = agpu_rd(REG_A6XX_CP_RB_RPTR);
		status = agpu_rd(REG_A6XX_RBBM_STATUS);
		status3 = agpu_rd(REG_A6XX_RBBM_STATUS3);
		if (rptr == wptr) {
			g_sqe_up = true;
			printf("SWEET GPU: SQE RING IDLE rptr=wptr=%u "
			    "status 0x%08x status3 0x%08x (%d us)\n",
			    rptr, status, status3, i * 10);
			if (status3 & A6XX_RBBM_STATUS3_SMMU_STALLED) {
				printf("SWEET GPU: SMMU stalled on fault - "
				    "need Adreno SMMU bypass/map next\n");
				g_sqe_up = false;
				return false;
			}
			printf("SWEET GPU: Metal track B2 milestone - "
			    "CP executed ME_INIT/NOP\n");
			return true;
		}
		delay(10);
	}

	rptr = agpu_rd(REG_A6XX_CP_RB_RPTR);
	status = agpu_rd(REG_A6XX_RBBM_STATUS);
	status3 = agpu_rd(REG_A6XX_RBBM_STATUS3);
	printf("SWEET GPU: SQE RING TIMEOUT rptr=%u wptr=%u "
	    "status 0x%08x status3 0x%08x%s\n",
	    rptr, wptr, status, status3,
	    (status3 & A6XX_RBBM_STATUS3_SMMU_STALLED) ? " SMMU_STALL" : "");
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
			if (gpu_gmu_start_legacy(g_mode >= 5)) {
				printf("SWEET GPU: Metal track B1 milestone - GMU firmware running\n");
			}
			if (g_mode >= 5 && g_gmu_up) {
				(void)gpu_sqe_ring_bringup();
			}
		} else if (g_on) {
			printf("SWEET GPU: GX/GMU/submit still TODO "
			    "(sweetgpu=4 GMU; =5 SQE/ring; Metal = METAL-PORT.md)\n");
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
			    "(use sweetgpu=2 to power on, =4 GMU, =5 SQE)\n");
			return;
		}
		gpu_read_chip();
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */

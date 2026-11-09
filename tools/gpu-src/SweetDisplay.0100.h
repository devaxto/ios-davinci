/*
 * SweetDisplay.h: DPU (MDSS/SDE) probe + zero-copy flip for davinci/SWEET.
 *
 * The bootloader leaves the display controller scanning the panel
 * framebuffer. sweet_display_probe() reads the live pipeline (never writes
 * unless sweetfb_flip is later used) and remembers the SSPP SRC_ADDR /
 * YSTRIDE / SRC_XY / SRC_SIZE and the live CTLs. sweet_display_flip() then
 * points that layer at a new physically contiguous buffer - a present with
 * no 10 MB CPU copy. See docs/GPU.md.
 */
#ifndef _SWEET_DISPLAY_H_
#define _SWEET_DISPLAY_H_

#include <sys/types.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * fb_phys/fb_stride describe the panel memory XNU draws into; the probe
 * looks for those values in the register dump to name SRC_ADDR / YSTRIDE.
 * Boot-arg sweetdpu: 0 = off, 1 = probe (default).
 */
void sweet_display_probe(uint64_t fb_phys, uint32_t fb_stride,
    uint32_t fb_width, uint32_t fb_height);

/* True after a successful probe that found the live layer's SRC_ADDR. */
bool sweet_display_can_flip(void);

/*
 * Point the live SSPP at phys (32-bit PA) with the given bytes-per-row and
 * source window. (x, y) is the origin of the panel-sized window inside the
 * buffer (0,0 for an exact panel surface; the centered crop for a larger
 * logical surface), width/height default to the panel rect when 0.
 *
 * Only SRC_ADDR, SRC_YSTRIDE0 and SRC_XY are written (SRC_SIZE/DST_SIZE only
 * when the window differs from the bootloader's), then every live CTL is
 * flushed and kicked. Returns false if the probe did not find a layer, the
 * DPU mapping is gone, the size encoding could not be derived for a window
 * change, or the write did not latch (the plane is restored first).
 */
bool sweet_display_flip(uint32_t phys, uint32_t stride, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height);

/* Restore the original ABL splash PA (for power-off fade / panic path). */
void sweet_display_flip_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* _SWEET_DISPLAY_H_ */

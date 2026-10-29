/*
 * SweetDisplay.h: read-only DPU (MDSS/SDE) probe for davinci/SWEET.
 *
 * The bootloader leaves the display controller scanning out the panel
 * framebuffer, so the register blocks that matter are already configured
 * when XNU runs. sweet_display_probe() reads them (never writes) and puts
 * them in the kernel log, which is how the layer/stride/format of the live
 * pipeline are recovered without Linux or a schematic. See docs/GPU.md.
 */
#ifndef _SWEET_DISPLAY_H_
#define _SWEET_DISPLAY_H_

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * fb_phys/fb_stride describe the panel memory XNU is told to draw into; the
 * probe looks for those values in the register dump to name the registers
 * that drive the panel. Boot-arg sweetdpu: 0 = off, 1 = probe (default).
 */
void sweet_display_probe(uint64_t fb_phys, uint32_t fb_stride,
    uint32_t fb_width, uint32_t fb_height);

#ifdef __cplusplus
}
#endif

#endif /* _SWEET_DISPLAY_H_ */

/*
 * Built-in IOMobileFramebuffer for SWEET: the ABL's continuous-splash
 * framebuffer (1080x2400 a8r8g8b8 in RAM, the panel scans it out by
 * itself) as the main display, driven by CoreAnimation's CPU renderer.
 *
 * IOMobileFramebuffer.framework (iOS 16.7) counts /arm-io children with
 * device_type "display-subsystem", waits for that many services matching
 * IOServiceMatching("IOMobileFramebuffer"), opens each with type 0 and
 * reads canvas-width/canvas-height from the service's provider. The class
 * here (AppleCLCD, an IOMobileFramebuffer) matches the /arm-io/disp0 nub.
 *
 * User client selectors used by CoreAnimation:
 *   3  GetLayerDefaultSurface [layer, ?] -> [IOSurface port name]
 *   4  SwapBegin -> [swap id]
 *   5  SwapEnd, struct 0x46c: +0x50 swap id, +0x54 IOSurface id per layer
 *      (4 layers), +0x64 rect a / +0xc4 rect b per layer (int x,y,w,h),
 *      +0x104 enabled layer mask
 *   6  SwapWait, 7 GetID, 8 GetDisplaySize -> [w, h],
 *   12 RequestPowerChange [state]: 0 = display off (backboardd, power
 *      button / auto-lock), else on. Off fades the panel out over a few
 *      vsyncs (halving each pixel) and keeps it black until on again;
 *      the panel itself stays powered (AMOLED black pixels are off).
 *   18 IsMainDisplay,
 *   28 GetDotPitch, 72 Enable/DisableNotifications [func, refcon, type, 0]
 * Everything else is accepted and logged once.
 *
 * SwapEnd only records the frame. A second 60 Hz timer, on its own work loop,
 * copies the enabled layers' surfaces into the panel memory (no scaling, CPU
 * copy) and stretches its period to whatever the measured copy cost allows
 * (boot-arg sweetfb_hz caps the rate). Vsync (notification type 5) is a
 * separate 60 Hz timer that only sends (count, mach_absolute_time, 0) as
 * 32-bit halves to clients that enabled it, through the port they registered
 * (IOConnectSetNotificationPort), so a slow copy cannot delay CoreAnimation's
 * frame pacing.
 *
 * The panel memory itself is published as an IOSurface and returned as the
 * default framebuffer surface. The kernel text console keeps drawing over
 * the GUI by default: boot-arg sweetfb_noconsole=1 stops drawing it from
 * the first swap on (it still reaches the persistent log; panics draw).
 *
 * The copy goes through a cached mapping by default (sweetfb_cache=0 asks
 * for write-combined instead): a 10 MB frame of write-combined stores is a
 * 10-20 ms stall, the same frame memcpy'd into cache + dc cvac is about 1 ms.
 * start() also runs the DPU probe (SweetDisplay.cpp). With sweetfb_flip=1 and
 * a contiguous layer surface below 4 GB, present becomes SRC_ADDR + CTL flush
 * instead of the memcpy (CPU fallback stays for anything the probe missed).
 */
#include "SweetDisplay.h"
#include "SweetSurface.h"

extern "C" int sweet_console_muted;     /* osfmk/console/serial_console.c */

#if defined(SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/system.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <sys/proc.h>

enum {
	kSweetFBGetLayerDefaultSurface  = 3,
	kSweetFBSwapBegin               = 4,
	kSweetFBSwapEnd                 = 5,
	kSweetFBSwapWait                = 6,
	kSweetFBGetID                   = 7,
	kSweetFBGetDisplaySize          = 8,
	kSweetFBRequestPowerChange      = 12,
	kSweetFBIsMainDisplay           = 18,
	kSweetFBGetDotPitch             = 28,
	kSweetFBNotifications           = 72,
};

#define SWEET_FB_SWAP_REC_SIZE          0x46c
#define SWEET_FB_LAYERS                 4
#define SWEET_FB_NOTIFY_TYPES           8
#define SWEET_FB_NOTIFY_VSYNC           5
#define SWEET_FB_VSYNC_US               16667
#define SWEET_FB_DOT_PITCH              458     /* ppi, as the iPhone X panel */
/*
 * The iOS userland only has status bar / layout data for real iPhone
 * screens (UIKit: "No status bar visual provider for screen", the red
 * rdar://45025538 bar for 360x800 points). The display is therefore
 * presented as the iPhone X panel, 1125x2436 (375x812 points @3x), and the
 * centered 1080x2340 part of it is shown on the 1080x2340 panel.
 * Boot-arg sweetfb_native=1 presents the real panel size instead.
 */
#define SWEET_FB_LOGICAL_W              1125
#define SWEET_FB_LOGICAL_H              2436
#define SWEET_FB_FORMAT_BGRA            0x42475241

class IOMobileFramebuffer : public IOService {
	OSDeclareDefaultStructors(IOMobileFramebuffer);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

class IOMobileFramebufferUserClient;

class AppleCLCD : public IOMobileFramebuffer {
	OSDeclareDefaultStructors(AppleCLCD);

	IOWorkLoop         *fWorkLoop;         /* vsync notifications */
	IOTimerEventSource *fVsync;
	IOWorkLoop         *fPresentLoop;      /* panel copies */
	IOTimerEventSource *fPresentTimer;
	uint64_t            fVsyncCount;

	static void vsyncFired(OSObject *owner, IOTimerEventSource *sender);
	static void presentFired(OSObject *owner, IOTimerEventSource *sender);

public:
	uint64_t      fPhys;
	uint32_t      fWidth, fHeight, fRowBytes;
	uint32_t      fLogicalW, fLogicalH;     /* size reported to userland */
	int32_t       fOffX, fOffY;             /* logical origin of the panel */
	uintptr_t     fPixels;          /* kernel mapping of the panel memory */
	IOMemoryMap  *fMap;
	SweetSurface *fSurface;         /* the panel memory as an IOSurface */
	IOLock       *fLock;            /* swaps and client list */
	OSArray      *fClients;
	uint32_t      fSwapID;
	bool          fConsolePaused;
	uint8_t       fPending[SWEET_FB_SWAP_REC_SIZE];     /* latest swap, not yet shown */
	bool          fDirty;
	uint32_t      fPeriodUS;      /* present period cap, from sweetfb_hz */
	uint64_t      fPresents;      /* panel copies done */
	uint64_t      fCopyUs;        /* duration of the last panel copy */
	uint64_t      fFrameBytes;    /* bytes the last present copied */
	uint32_t      fCacheMode;     /* 1 = cached panel mapping + dc cvac */
	uint32_t      fFlipMode;      /* 1 = DPU SRC_ADDR flip when possible */
	uint64_t      fFlips;         /* zero-copy presents */
	bool          fPowerOff;      /* RequestPowerChange(0) */
	uint32_t      fFadeStep;      /* fade-out steps done while off */

	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void addClient(IOMobileFramebufferUserClient *client);
	void removeClient(IOMobileFramebufferUserClient *client);
	void swap(const uint8_t *rec);
	void present(const uint8_t *rec);
};

OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);

class IOMobileFramebufferUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

public:
	AppleCLCD          *fFB;
	task_t              fTask;
	mach_port_t         fPort;
	io_user_reference_t fFunc[SWEET_FB_NOTIFY_TYPES];
	io_user_reference_t fRefcon[SWEET_FB_NOTIFY_TYPES];

	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn registerNotificationPort(mach_port_t port, UInt32 type,
	    io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
	void notify(uint32_t type, const uint64_t *values, uint32_t count);
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

bool
AppleCLCD::start(IOService *provider)
{
	PE_Video video;

	if (!IOService::start(provider)) {
		return false;
	}
	PE_current_console(&video);
	fPhys = video.v_baseAddr;
	fWidth = (uint32_t)video.v_width;
	fHeight = (uint32_t)video.v_height;
	fRowBytes = (uint32_t)video.v_rowBytes;
	if (fPhys == 0 || fWidth == 0 || fHeight == 0 || fRowBytes < fWidth * 4) {
		IOLog("SWEET FB: no boot framebuffer\n");
		return false;
	}
	int native = 0;
	PE_parse_boot_argn("sweetfb_native", &native, sizeof(native));
	fLogicalW = fWidth;
	fLogicalH = fHeight;
	if (!native && fWidth <= SWEET_FB_LOGICAL_W && fHeight <= SWEET_FB_LOGICAL_H) {
		fLogicalW = SWEET_FB_LOGICAL_W;
		fLogicalH = SWEET_FB_LOGICAL_H;
	}
	fOffX = (int32_t)(fLogicalW - fWidth) / 2;
	fOffY = (int32_t)(fLogicalH - fHeight) / 2;
	fLock = IOLockAlloc();
	fClients = OSArray::withCapacity(4);
	fSurface = sweet_surface_create_physical(fPhys, fWidth, fHeight, fRowBytes, SWEET_FB_FORMAT_BGRA);
	fPixels = fSurface != NULL ? fSurface->kernelAddress() : 0;
	/*
	 * Write-combined stores into the panel run at a few GB/s at best: a
	 * 1080x2340 frame then costs several milliseconds per present. A cached
	 * mapping plus an explicit clean (dc cvac) is much faster because the
	 * copy runs at memory bandwidth; the display controller is not cache
	 * coherent, hence the flush in present(). Cached is the default,
	 * sweetfb_cache=0 selects the write-combined mapping again.
	 */
	fCacheMode = 1;
	PE_parse_boot_argn("sweetfb_cache", &fCacheMode, sizeof(fCacheMode));
	/*
	 * Splash pool: CA full-screen layers land in the ABL 24 MB carve-out the
	 * DPU already scans. Flip defaults ON so present is SRC_ADDR, not memcpy.
	 * sweetfb_flip=0 forces the CPU copy path again.
	 */
	{
		/* Two ~12 MB slots in the 24 MB splash: fits 1125x2436 BGRA. */
		sweet_surface_set_splash_pool(fPhys, 24u << 20, (24u << 20) / 2);
		fFlipMode = 1;
		PE_parse_boot_argn("sweetfb_flip", &fFlipMode, sizeof(fFlipMode));
	}
	if (fCacheMode != 0 && fSurface != NULL) {
		IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(fPhys,
		    (vm_size_t)fRowBytes * fHeight, kIODirectionInOut);
		fMap = md != NULL ? md->map(kIOMapCopybackCache) : NULL;
		OSSafeReleaseNULL(md);
		if (fMap != NULL && fMap->getVirtualAddress() != 0) {
			fPixels = (uintptr_t)fMap->getVirtualAddress();
			IOLog("SWEET FB: panel copy through a cached mapping + cvac\n");
		} else {
			fCacheMode = 0;
			IOLog("SWEET FB: cached panel mapping failed, keeping write-combined\n");
			OSSafeReleaseNULL(fMap);
		}
	}
	fWorkLoop = IOWorkLoop::workLoop();
	if (fLock == NULL || fClients == NULL || fPixels == 0 || fWorkLoop == NULL) {
		IOLog("SWEET FB: setup failed\n");
		OSSafeReleaseNULL(fMap);
		OSSafeReleaseNULL(fSurface);    /* stays published: also held by the table */
		OSSafeReleaseNULL(fWorkLoop);
		return false;
	}
	fVsync = IOTimerEventSource::timerEventSource(this, &AppleCLCD::vsyncFired);
	if (fVsync == NULL || fWorkLoop->addEventSource(fVsync) != kIOReturnSuccess) {
		IOLog("SWEET FB: no vsync timer\n");
		return false;
	}
	uint32_t hz = 60;
	PE_parse_boot_argn("sweetfb_hz", &hz, sizeof(hz));
	fPeriodUS = (hz == 0 || hz > 240) ? SWEET_FB_VSYNC_US : 1000000u / hz;
	fVsync->setTimeoutUS(SWEET_FB_VSYNC_US);
	/*
	 * The panel copy gets its own work loop and timer, so the vsync
	 * notifications above stay on time whatever a copy costs.
	 */
	fPresentLoop = IOWorkLoop::workLoop();
	fPresentTimer = fPresentLoop != NULL ?
	    IOTimerEventSource::timerEventSource(this, &AppleCLCD::presentFired) : NULL;
	if (fPresentTimer == NULL ||
	    fPresentLoop->addEventSource(fPresentTimer) != kIOReturnSuccess) {
		IOLog("SWEET FB: no present timer\n");
		return false;
	}
	fPresentTimer->setTimeoutUS(fPeriodUS);

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	setProperty("external", false);
	setProperty("DisplayPipePlaneBaseAlignment", 64ULL, 32);
	registerService();
	IOLog("SWEET FB: AppleCLCD %ux%u stride %u at 0x%llx, default surface %u, "
	    "presented as %ux%u, present every %u us, panel %s, flip %s\n",
	    fWidth, fHeight, fRowBytes, fPhys, fSurface->id, fLogicalW, fLogicalH, fPeriodUS,
	    fCacheMode != 0 ? "cached" : "write-combined",
	    fFlipMode != 0 ? "on" : "off");
	/* Discover the live SSPP; enables sweet_display_flip when found. */
	sweet_display_probe(fPhys, fRowBytes, fWidth, fHeight);
	if (fFlipMode != 0 && !sweet_display_can_flip()) {
		IOLog("SWEET FB: sweetfb_flip=1 but DPU layer not found, CPU copy stays\n");
	}
	return true;
}

void
AppleCLCD::vsyncFired(OSObject *owner, IOTimerEventSource *sender)
{
	AppleCLCD *fb = (AppleCLCD *)owner;
	uint64_t now = mach_absolute_time();
	uint64_t count = ++fb->fVsyncCount;
	uint64_t values[6] = {
		(uint32_t)count, (uint32_t)(count >> 32),
		(uint32_t)now, (uint32_t)(now >> 32),
		0, 0,
	};

	IOLockLock(fb->fLock);
	for (unsigned int i = 0; i < fb->fClients->getCount(); i++) {
		IOMobileFramebufferUserClient *client =
		    (IOMobileFramebufferUserClient *)fb->fClients->getObject(i);
		client->notify(SWEET_FB_NOTIFY_VSYNC, values, 6);
	}
	IOLockUnlock(fb->fLock);
	sender->setTimeoutUS(SWEET_FB_VSYNC_US);
}

/* Microseconds since start, for the present cost report. */
static uint32_t
sweet_fb_elapsed_us(uint64_t start)
{
	uint64_t delta = 0;

	absolutetime_to_nanoseconds(mach_absolute_time() - start, &delta);
	return delta > 1000000000ULL ? 1000000000u : (uint32_t)(delta / 1000);
}

/*
 * Clean the lines of a just-written panel region: the display controller is
 * not cache coherent. dc cvac (clean, keep the line) rather than civac, so a
 * dirty line someone else holds through another mapping is not discarded.
 */
static void
sweet_fb_clean_rows(uintptr_t dst, size_t row, int32_t h, uint32_t stride)
{
	uint64_t ctr;
	size_t line;

	__asm__ volatile ("mrs %0, ctr_el0" : "=r"(ctr));
	line = (size_t)4 << ((ctr >> 16) & 0xf);
	for (int32_t y = 0; y < h; y++) {
		uintptr_t start = dst + (uint64_t)y * stride;
		uintptr_t p = start & ~(uintptr_t)(line - 1);
		uintptr_t end = start + row;

		for (; p < end; p += line) {
			__asm__ volatile ("dc cvac, %0" :: "r"(p) : "memory");
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

/*
 * Panel copy, on its own work loop. The period follows the measured cost:
 * a copy may use about a third of the frame period, so a slow mapping
 * (write-combined stores run at a few GB/s at best) costs update rate
 * instead of CPU. sweetfb_hz caps the rate from above.
 */
void
AppleCLCD::presentFired(OSObject *owner, IOTimerEventSource *sender)
{
	AppleCLCD *fb = (AppleCLCD *)owner;
	uint8_t rec[SWEET_FB_SWAP_REC_SIZE];
	bool dirty, off;
	uint32_t step, period;

	IOLockLock(fb->fLock);
	dirty = fb->fDirty;
	if (dirty) {
		memcpy(rec, fb->fPending, sizeof(rec));
		fb->fDirty = false;
	}
	off = fb->fPowerOff;
	step = fb->fFadeStep;
	if (off && step <= 6) {
		fb->fFadeStep = step + 1;
	}
	IOLockUnlock(fb->fLock);

	if (off) {
		/* Fade must hit the buffer the DPU is scanning. */
		if (step == 0 && fb->fFlipMode != 0) {
			sweet_display_flip_reset();
		}
		/* Fade out: halve every channel each step, then black, then idle. */
		if (step <= 6) {
			for (uint32_t y = 0; y < fb->fHeight; y++) {
				volatile uint32_t *row = (volatile uint32_t *)(fb->fPixels +
				    (uint64_t)y * fb->fRowBytes);
				for (uint32_t x = 0; x < fb->fWidth; x++) {
					row[x] = step < 6 ? (row[x] >> 1) & 0x7f7f7f7f : 0;
				}
			}
			if (fb->fCacheMode != 0) {
				sweet_fb_clean_rows(fb->fPixels, (size_t)fb->fWidth * 4,
				    (int32_t)fb->fHeight, fb->fRowBytes);
			}
		}
	} else if (dirty) {
		uint64_t start = mach_absolute_time();
		fb->fFrameBytes = 0;
		fb->present(rec);
		fb->fCopyUs = sweet_fb_elapsed_us(start);
		fb->fPresents++;
	}

	period = fb->fPeriodUS;
	if (fb->fCopyUs * 3 > (uint64_t)period) {
		uint64_t want = fb->fCopyUs * 3;
		period = (uint32_t)(want > 50000 ? 50000 : want);
	}
	if ((fb->fPresents % 120) == 0 && fb->fPresents != 0) {
		uint64_t mbps = fb->fCopyUs != 0 ?
		    (fb->fFrameBytes * 1000000ULL) / (fb->fCopyUs * (1024ULL * 1024ULL)) : 0;
		IOLog("SWEET FB: present %llu %u us %llu KB %llu MB/s every %u us "
		    "(flips %llu)\n",
		    (unsigned long long)fb->fPresents, (uint32_t)fb->fCopyUs,
		    (unsigned long long)(fb->fFrameBytes / 1024),
		    (unsigned long long)mbps, period,
		    (unsigned long long)fb->fFlips);
	}
	sender->setTimeoutUS(period);
}

void
AppleCLCD::addClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	fClients->setObject(client);
	IOLockUnlock(fLock);
}

void
AppleCLCD::removeClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	unsigned int i = fClients->getNextIndexOfObject(client, 0);
	if (i != (unsigned int)-1) {
		fClients->removeObject(i);
	}
	IOLockUnlock(fLock);
}

static uint32_t
get32(const uint8_t *p, uint32_t off)
{
	uint32_t v;
	memcpy(&v, p + off, 4);
	return v;
}

static uint32_t sweet_fb_swaps_logged;
static uint64_t sweet_fb_swap_count;

/*
 * A swap only records the frame: copying 10 MB into the panel on every swap
 * (backboardd presents at 60 Hz) took the whole CPU, and doing it on the
 * vsync work loop also delayed the notifications back to CoreAnimation.
 * presentFired() does the copy and the cost report; swap() stays a small
 * memcpy under the lock.
 */
void
AppleCLCD::swap(const uint8_t *rec)
{
	IOLockLock(fLock);
	memcpy(fPending, rec, SWEET_FB_SWAP_REC_SIZE);
	fDirty = true;
	IOLockUnlock(fLock);
	if ((++sweet_fb_swap_count % 1000) == 0) {
		IOLog("SWEET FB: %llu swaps\n", sweet_fb_swap_count);
	}
}

/*
 * Zero-copy present: one full-screen BGRA layer whose backing store is a
 * single contiguous PA below 4 GB -> point the live SSPP at it. Anything else
 * (multi-layer, partial rect, non-contiguous) falls through to the CPU copy.
 */
static bool
sweet_fb_try_flip(AppleCLCD *fb, const uint8_t *rec)
{
	uint32_t mask = get32(rec, 0x104);
	uint32_t found = 0, id = 0;
	int32_t sx, sy, dx, dy, w, h;
	const uint8_t *a, *b;
	SweetSurface *s;
	uint64_t phys;
	uintptr_t src;
	static uint32_t flip_logged;

	if (fb->fFlipMode == 0 || !sweet_display_can_flip()) {
		return false;
	}
	for (uint32_t layer = 0; layer < SWEET_FB_LAYERS; layer++) {
		if (!(mask & (1u << layer))) {
			continue;
		}
		id = get32(rec, 0x54 + 4 * layer);
		if (id == 0) {
			continue;
		}
		found++;
		if (found > 1) {
			return false;
		}
		a = rec + 0x64 + 16 * layer;
		b = rec + 0xc4 + 16 * layer;
		sx = (int32_t)get32(a, 0);
		sy = (int32_t)get32(a, 4);
		dx = (int32_t)get32(b, 0) - fb->fOffX;
		dy = (int32_t)get32(b, 4) - fb->fOffY;
		w = (int32_t)get32(b, 8);
		h = (int32_t)get32(b, 12);
	}
	if (found != 1) {
		return false;
	}
	s = sweet_surface_copy(id);
	if (s == NULL || s == fb->fSurface) {
		OSSafeReleaseNULL(s);
		return false;
	}
	if (w <= 0 || h <= 0) {
		w = (int32_t)s->width;
		h = (int32_t)s->height;
	}
	if (sx < 0 || sy < 0 || dx != 0 || dy != 0 ||
	    w != (int32_t)fb->fWidth || h != (int32_t)fb->fHeight ||
	    sx != 0 || sy != 0 ||
	    s->bytesPerElement != 4 || s->elementWidth != 1 || s->elementHeight != 1) {
		s->release();
		return false;
	}
	/* Surface must cover the panel; stride may differ (CA pads to 64 B). */
	if (s->width < fb->fWidth || s->height < fb->fHeight) {
		s->release();
		return false;
	}
	phys = sweet_surface_phys(s);
	src = s->kernelAddress();
	if (phys == 0 || phys > 0xffffffffull || src == 0) {
		s->release();
		return false;
	}
	/* DPU is not cache coherent: clean what CA just drew. */
	sweet_fb_clean_rows(src + s->offset, (size_t)s->bytesPerRow,
	    (int32_t)fb->fHeight, s->bytesPerRow);
	if (!sweet_display_flip((uint32_t)phys, s->bytesPerRow)) {
		s->release();
		return false;
	}
	fb->fFlips++;
	fb->fFrameBytes = 0;
	if (flip_logged < 8) {
		flip_logged++;
		IOLog("SWEET FB: flip surface %u pa 0x%llx stride %u\n",
		    s->id, (unsigned long long)phys, s->bytesPerRow);
	}
	s->release();
	return true;
}

/* Copy each enabled layer's surface into the panel memory (no scaling). */
void
AppleCLCD::present(const uint8_t *rec)
{
	uint32_t mask = get32(rec, 0x104);

	if (!fConsolePaused) {
		int quiet = 0;
		fConsolePaused = true;
		if (PE_parse_boot_argn("sweetfb_noconsole", &quiet, sizeof(quiet)) && quiet != 0) {
			/* Not PE_initialize_console(kPEDisableScreen): its console switch
			 * writes the read-only-late cons_ops_index and panics. */
			sweet_console_muted = 1;
		}
	}

	if (sweet_fb_try_flip(this, rec)) {
		return;
	}

	for (uint32_t layer = 0; layer < SWEET_FB_LAYERS; layer++) {
		uint32_t id = get32(rec, 0x54 + 4 * layer);
		const uint8_t *a = rec + 0x64 + 16 * layer;
		const uint8_t *b = rec + 0xc4 + 16 * layer;
		int32_t sx = (int32_t)get32(a, 0), sy = (int32_t)get32(a, 4);
		int32_t dx = (int32_t)get32(b, 0), dy = (int32_t)get32(b, 4);
		int32_t w = (int32_t)get32(b, 8), h = (int32_t)get32(b, 12);

		if (!(mask & (1u << layer)) || id == 0) {
			continue;
		}
		SweetSurface *s = sweet_surface_copy(id);
		if (sweet_fb_swaps_logged < 16) {
			sweet_fb_swaps_logged++;
			IOLog("SWEET FB: swap %u layer %u surface %u %ux%u fmt 0x%x bpr %u a %d,%d %ux%u b %d,%d %dx%d\n",
			    get32(rec, 0x50), layer, id, s ? s->width : 0, s ? s->height : 0,
			    s ? s->pixelFormat : 0, s ? s->bytesPerRow : 0, sx, sy, get32(a, 8),
			    get32(a, 12), dx, dy, w, h);
		}
		if (s == NULL || s == fSurface) {
			OSSafeReleaseNULL(s);
			continue;
		}
		uintptr_t src = s->kernelAddress();
		uint32_t bpe = s->bytesPerElement ? s->bytesPerElement : 4;
		if (w <= 0 || h <= 0) {
			w = (int32_t)s->width;
			h = (int32_t)s->height;
		}
		if (sx < 0 || sy < 0) {
			sx = sy = 0;
		}
		if (dx < 0 || dy < 0) {
			dx = dy = 0;
		}
		/* Logical (presented) display coordinates -> panel coordinates. */
		dx -= fOffX;
		dy -= fOffY;
		if (dx < 0) {
			sx -= dx;
			w += dx;
			dx = 0;
		}
		if (dy < 0) {
			sy -= dy;
			h += dy;
			dy = 0;
		}
		if (w > (int32_t)s->width - sx) {
			w = (int32_t)s->width - sx;
		}
		if (h > (int32_t)s->height - sy) {
			h = (int32_t)s->height - sy;
		}
		if (w > (int32_t)fWidth - dx) {
			w = (int32_t)fWidth - dx;
		}
		if (h > (int32_t)fHeight - dy) {
			h = (int32_t)fHeight - dy;
		}
		uint64_t last = (uint64_t)s->offset + (uint64_t)(sy + h - 1) * s->bytesPerRow +
		    (uint64_t)(sx + w) * 4;
		if (w <= 0 || h <= 0 || s->elementWidth != 1 || s->elementHeight != 1 ||
		    s->memory == NULL || last > s->memory->getLength()) {
			src = 0;
		}
		if (src != 0 && bpe == 4) {
			size_t row = (size_t)w * 4;
			uintptr_t dst = fPixels + (uint64_t)dy * fRowBytes + (uint64_t)dx * 4;
			const uint8_t *from = (const uint8_t *)(src + s->offset +
			    (uint64_t)sy * s->bytesPerRow + (uint64_t)sx * 4);

			fFrameBytes += (uint64_t)row * (uint64_t)h;
			if (row == fRowBytes && row == s->bytesPerRow) {
				/* Full-width layer: one big memcpy beats per-row calls. */
				memcpy((void *)dst, from, row * (size_t)h);
			} else {
				for (int32_t y = 0; y < h; y++) {
					memcpy((void *)(dst + (uint64_t)y * fRowBytes),
					    from + (uint64_t)y * s->bytesPerRow, row);
				}
			}
			if (fCacheMode != 0) {
				sweet_fb_clean_rows(dst, row, h, fRowBytes);
			}
		}
		s->release();
	}
}

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService *provider)
{
	fFB = OSDynamicCast(AppleCLCD, provider);
	if (fFB == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fFB->addClient(this);
	return true;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fFB != NULL) {
		fFB->removeClient(this);
		IOLockLock(fFB->fLock);
		mach_port_t old = fPort;
		fPort = MACH_PORT_NULL;
		IOLockUnlock(fFB->fLock);
		if (old != MACH_PORT_NULL) {
			releaseNotificationPort(old);
		}
	}
	terminate();
	return kIOReturnSuccess;
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, __unused UInt32 type,
    __unused io_user_reference_t refCon)
{
	mach_port_t old;
	IOLockLock(fFB->fLock);
	old = fPort;
	fPort = port;
	IOLockUnlock(fFB->fLock);
	if (old != MACH_PORT_NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

/* Called with the framebuffer lock held. */
void
IOMobileFramebufferUserClient::notify(uint32_t type, const uint64_t *values, uint32_t count)
{
	OSAsyncReference64 ref;

	if (type >= SWEET_FB_NOTIFY_TYPES || fFunc[type] == 0 || fPort == MACH_PORT_NULL) {
		return;
	}
	bzero(ref, sizeof(ref));
	setAsyncReference64(ref, fPort, (mach_vm_address_t)fFunc[type], fRefcon[type], fTask);
	sendAsyncResult64WithOptions(ref, kIOReturnSuccess, (io_user_reference_t *)values, count,
	    kIOUserNotifyOptionCanDrop);
}

static uint64_t sweet_fb_logged;        /* selectors already logged (< 64) */

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	IOReturn ret = kIOReturnSuccess;
	uint64_t out[2] = { 0, 0 };
	uint32_t outCount = 0;

	switch (selector) {
	case kSweetFBGetLayerDefaultSurface:
		out[0] = sweet_surface_port_in_task(fFB->fSurface, fTask);
		outCount = 1;
		break;
	case kSweetFBSwapBegin:
		IOLockLock(fFB->fLock);
		out[0] = ++fFB->fSwapID;
		IOLockUnlock(fFB->fLock);
		outCount = 1;
		break;
	case kSweetFBSwapEnd: {
		uint8_t rec[SWEET_FB_SWAP_REC_SIZE];
		if (args->structureInputDescriptor != NULL ||
		    args->structureInputSize < SWEET_FB_SWAP_REC_SIZE) {
			ret = kIOReturnBadArgument;
			break;
		}
		memcpy(rec, args->structureInput, sizeof(rec));
		fFB->swap(rec);
		break;
	}
	case kSweetFBGetID:
		out[0] = 0;
		outCount = 1;
		break;
	case kSweetFBGetDisplaySize:
		out[0] = fFB->fLogicalW;
		out[1] = fFB->fLogicalH;
		outCount = 2;
		break;
	case kSweetFBIsMainDisplay:
		out[0] = 1;
		outCount = 1;
		break;
	case kSweetFBGetDotPitch:
		out[0] = SWEET_FB_DOT_PITCH;
		outCount = 1;
		break;
	case kSweetFBNotifications:
		if (args->scalarInputCount >= 3 && args->scalarInput[2] < SWEET_FB_NOTIFY_TYPES) {
			uint32_t type = (uint32_t)args->scalarInput[2];
			IOLockLock(fFB->fLock);
			fFunc[type] = args->scalarInput[0];
			fRefcon[type] = args->scalarInput[1];
			IOLockUnlock(fFB->fLock);
			char name[32];
			proc_selfname(name, sizeof(name));
			IOLog("SWEET FB: %s notifications type %u %s\n", name, type,
			    args->scalarInput[0] ? "on" : "off");
		}
		break;
	case kSweetFBRequestPowerChange:
		if (args->scalarInputCount >= 1) {
			bool off = args->scalarInput[0] == 0;
			IOLockLock(fFB->fLock);
			if (off != fFB->fPowerOff) {
				fFB->fPowerOff = off;
				fFB->fFadeStep = 0;
				fFB->fDirty = true;     /* on: redraw the latest frame */
			}
			IOLockUnlock(fFB->fLock);
			IOLog("SWEET FB: display %s\n", off ? "off" : "on");
		}
		break;
	case kSweetFBSwapWait: {
		/*
		 * Returns at once: whether CoreAnimation passes a swap id (and a
		 * timeout) here decides whether blocking would pace the present
		 * loop better. Log the first calls, do not guess the ABI.
		 */
		static uint32_t sweet_fb_wait_logged;
		if (sweet_fb_wait_logged < 8) {
			sweet_fb_wait_logged++;
			char name[32];
			proc_selfname(name, sizeof(name));
			IOLog("SWEET FB: %s SwapWait in %u %llu %llu %llu (presented %llu)\n",
			    name, args->scalarInputCount,
			    args->scalarInputCount > 0 ? (unsigned long long)args->scalarInput[0] : 0ULL,
			    args->scalarInputCount > 1 ? (unsigned long long)args->scalarInput[1] : 0ULL,
			    args->scalarInputCount > 2 ? (unsigned long long)args->scalarInput[2] : 0ULL,
			    (unsigned long long)fFB->fPresents);
		}
		break;
	}
	default:
		break;
	}

	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = i < outCount ? out[i] : 0;
	}
	if (args->structureOutputDescriptor == NULL && args->structureOutput != NULL) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	if (selector >= 64 || !(sweet_fb_logged & (1ULL << selector))) {
		if (selector < 64) {
			sweet_fb_logged |= 1ULL << selector;
		}
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("SWEET FB: %s sel %u in %u/%u out %u/%u -> 0x%x\n", name, selector,
		    args->scalarInputCount, args->structureInputSize,
		    args->scalarOutputCount, args->structureOutputSize, ret);
	}
	return ret;
}
#endif /* SWEET */

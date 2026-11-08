/*
 * DAVINCI / SWEET: Adreno 618 IOKit Accelerator & DRM Engine.
 *
 * Exposes HakiosGPU and AGXAccelerator for Metal / CoreAnimation.
 * Answers msm DRM requests (kMsmGemNew, kMsmGemSubmit, kHakiosSurface)
 * and dispatches PM4 command streams directly to the Adreno 618 CP ring.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <sys/proc.h>
#include <pexpert/pexpert.h>
#include "SweetSurface.h"

extern "C" {
int sweet_gpu_mode(void);
bool sweet_gpu_is_on(void);
uint32_t sweet_gpu_chip_id(void);
bool sweet_gpu_sqe_up(void);
uint32_t sweet_gpu_submit_ib1(uint64_t pa, uint32_t dwords);
bool sweet_gpu_is_fence_done(uint32_t fence);
}

enum {
	kDrmGemClose            = 0x09,
	kMsmGetParam            = 0x40,
	kMsmSetParam            = 0x41,
	kMsmGemNew              = 0x42,
	kMsmGemInfo             = 0x43,
	kMsmGemCpuPrep          = 0x44,
	kMsmGemCpuFini          = 0x45,
	kMsmGemSubmit           = 0x46,
	kMsmWaitFence           = 0x47,
	kMsmGemMadvise          = 0x48,
	kMsmSubmitQueueNew      = 0x4a,
	kMsmSubmitQueueClose    = 0x4b,
	kMsmSubmitQueueQuery    = 0x4c,
	kHakiosSurface          = 0x7e,
};

struct msm_param { uint32_t pipe, param; uint64_t value; uint32_t len, pad; };
struct msm_gem_new { uint64_t size; uint32_t flags, handle; };
struct msm_gem_info { uint32_t handle, info; uint64_t value; uint32_t len, pad; };
struct msm_submit_cmd { uint32_t type, submit_idx, submit_offset, size, pad, nr_relocs; uint64_t relocs; };
struct msm_submit_bo { uint32_t flags, handle; uint64_t presumed; };
struct msm_gem_submit {
	uint32_t flags, fence, nr_bos, nr_cmds;
	uint64_t bos, cmds;
	int32_t fence_fd;
	uint32_t queueid;
	uint64_t in_syncobjs, out_syncobjs;
	uint32_t nr_in_syncobjs, nr_out_syncobjs, syncobj_stride, pad;
};
struct msm_wait_fence { uint32_t fence, flags; int64_t timeout_sec, timeout_nsec; uint32_t queueid; };
struct hakios_surface { uint32_t surface, handle; uint64_t size; uint32_t offset, rowBytes, width, height, pixelFormat, planes; };

#define MAX_BUFFERS 256
struct gpu_buf {
	uint32_t handle;
	uint64_t size;
	uint64_t physAddr;
	IOBufferMemoryDescriptor *bmd;
	SweetSurface *surface;
};

static struct gpu_buf g_buffers[MAX_BUFFERS];
static IOLock *g_gpu_lock = NULL;

class IOAcceleratorES : public IOService {
	OSDeclareDefaultStructors(IOAcceleratorES);
public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

bool
IOAcceleratorES::start(IOService *provider)
{
	return IOService::start(provider);
}

class HakiosGPU : public IOAcceleratorES {
	OSDeclareDefaultStructors(HakiosGPU);
public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosGPU, IOAcceleratorES);

class HakiosGPUUserClient : public IOUserClient {
	OSDeclareDefaultStructors(HakiosGPUUserClient);
	task_t fTask;
public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosGPUUserClient, IOUserClient);

bool
HakiosGPU::start(IOService *provider)
{
	if (!IOAcceleratorES::start(provider)) {
		return false;
	}
	if (!g_gpu_lock) {
		g_gpu_lock = IOLockAlloc();
	}
	setProperty(kIOUserClientClassKey, "HakiosGPUUserClient");
	setProperty("MetalPluginName", "HakiosMetal");
	setProperty("MetalPluginClassName", "HakiosMetalDevice");
	setProperty("IOGLBundleName", "SweetMetal.bundle");
	setProperty("SweetGPUChipID", (uint64_t)sweet_gpu_chip_id(), 64);
	setProperty("SweetGPUPowered", sweet_gpu_is_on());
	setName("HakiosGPU");
	registerService();
	IOLog("SWEET GPU: HakiosGPU online for Metal (chip 0x%08x)\n",
	    sweet_gpu_chip_id());
	return true;
}

bool
HakiosGPUUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

IOReturn
HakiosGPUUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

IOReturn
HakiosGPUUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
	IOLockLock(g_gpu_lock);
	for (int i = 0; i < MAX_BUFFERS; i++) {
		if (g_buffers[i].handle == type && g_buffers[i].bmd != NULL) {
			*options = kIOMapAnywhere;
			*memory = g_buffers[i].bmd;
			(*memory)->retain();
			IOLockUnlock(g_gpu_lock);
			return kIOReturnSuccess;
		}
	}
	IOLockUnlock(g_gpu_lock);
	return kIOReturnNotFound;
}

IOReturn
HakiosGPUUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch,
    __unused OSObject *target, __unused void *reference)
{
	IOReturn ret = kIOReturnSuccess;
	IOLockLock(g_gpu_lock);

	switch (selector) {
	case kMsmGetParam: {
		if (args->structureInputSize >= sizeof(struct msm_param)) {
			struct msm_param *p = (struct msm_param *)args->structureInput;
			if (p->param == 0) {
				p->value = 0x06010800u;
			} else {
				p->value = 0;
			}
			if (args->structureOutput != NULL) {
				memcpy(args->structureOutput, p, sizeof(*p));
			}
		}
		break;
	}
	case kMsmGemNew: {
		if (args->structureInputSize >= sizeof(struct msm_gem_new)) {
			struct msm_gem_new *g = (struct msm_gem_new *)args->structureInput;
			uint64_t sz = (g->size + 4095u) & ~4095ULL;
			int slot = -1;
			for (int i = 1; i < MAX_BUFFERS; i++) {
				if (g_buffers[i].handle == 0) { slot = i; break; }
			}
			if (slot > 0) {
				IOBufferMemoryDescriptor *bmd = IOBufferMemoryDescriptor::inTaskWithOptions(
				    kernel_task, kIOMemoryPhysicallyContiguous | kIODirectionInOut, sz, PAGE_SIZE);
				if (bmd) {
					g_buffers[slot].handle = slot;
					g_buffers[slot].size = sz;
					g_buffers[slot].physAddr = bmd->getPhysicalAddress();
					g_buffers[slot].bmd = bmd;
					g_buffers[slot].surface = NULL;
					g->handle = slot;
					if (args->structureOutput != NULL) {
						memcpy(args->structureOutput, g, sizeof(*g));
					}
				} else {
					ret = kIOReturnNoMemory;
				}
			} else {
				ret = kIOReturnNoResources;
			}
		}
		break;
	}
	case kMsmGemInfo: {
		if (args->structureInputSize >= sizeof(struct msm_gem_info)) {
			struct msm_gem_info *info = (struct msm_gem_info *)args->structureInput;
			uint32_t h = info->handle;
			if (h < MAX_BUFFERS && g_buffers[h].handle == h) {
				info->value = g_buffers[h].physAddr;
				if (args->structureOutput != NULL) {
					memcpy(args->structureOutput, info, sizeof(*info));
				}
			} else {
				ret = kIOReturnBadArgument;
			}
		}
		break;
	}
	case kMsmGemSubmit: {
		if (args->structureInputSize >= sizeof(struct msm_gem_submit)) {
			struct msm_gem_submit *sub = (struct msm_gem_submit *)args->structureInput;
			if (sub->cmds != 0 && sub->nr_cmds > 0) {
				struct msm_submit_cmd cmd;
				copyin((user_addr_t)sub->cmds, &cmd, sizeof(cmd));
				uint32_t h = cmd.submit_idx;
				if (h < MAX_BUFFERS && g_buffers[h].handle == h) {
					uint64_t pa = g_buffers[h].physAddr + cmd.submit_offset;
					uint32_t dwords = cmd.size / 4;
					sub->fence = sweet_gpu_submit_ib1(pa, dwords);
				}
			}
			if (args->structureOutput != NULL) {
				memcpy(args->structureOutput, sub, sizeof(*sub));
			}
		}
		break;
	}
	case kHakiosSurface: {
		if (args->structureInputSize >= sizeof(struct hakios_surface)) {
			struct hakios_surface *hs = (struct hakios_surface *)args->structureInput;
			SweetSurface *s = sweet_surface_copy(hs->surface);
			if (s != NULL) {
				int slot = -1;
				for (int i = 1; i < MAX_BUFFERS; i++) {
					if (g_buffers[i].handle == 0) { slot = i; break; }
				}
				if (slot > 0) {
					g_buffers[slot].handle = slot;
					g_buffers[slot].size = s->allocSize;
					g_buffers[slot].physAddr = s->physAddr;
					g_buffers[slot].bmd = NULL;
					g_buffers[slot].surface = s;
					hs->handle = slot;
					hs->size = s->allocSize;
					hs->offset = s->offset;
					hs->rowBytes = s->bytesPerRow;
					hs->width = s->width;
					hs->height = s->height;
					hs->pixelFormat = s->pixelFormat;
					hs->planes = s->planeCount;
					if (args->structureOutput != NULL) {
						memcpy(args->structureOutput, hs, sizeof(*hs));
					}
				} else {
					s->release();
					ret = kIOReturnNoResources;
				}
			} else {
				ret = kIOReturnNotFound;
			}
		}
		break;
	}
	case kMsmWaitFence: {
		if (args->structureInputSize >= sizeof(struct msm_wait_fence)) {
			struct msm_wait_fence *wf = (struct msm_wait_fence *)args->structureInput;
			if (!sweet_gpu_is_fence_done(wf->fence)) {
				ret = kIOReturnTimeout;
			}
		}
		break;
	}
	case kDrmGemClose: {
		if (args->structureInputSize >= 8) {
			uint32_t h = *(uint32_t *)args->structureInput;
			if (h < MAX_BUFFERS && g_buffers[h].handle == h) {
				if (g_buffers[h].bmd) {
					g_buffers[h].bmd->release();
				}
				if (g_buffers[h].surface) {
					g_buffers[h].surface->release();
				}
				bzero(&g_buffers[h], sizeof(g_buffers[h]));
			}
		}
		break;
	}
	default:
		break;
	}

	IOLockUnlock(g_gpu_lock);
	return ret;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */

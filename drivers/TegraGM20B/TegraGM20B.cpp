// SPDX-License-Identifier: MIT
// TegraGM20B entry point. tegra_gm20b_probe() brings the GPU up through every stage (kStage), unless boot-arg
// switch-no-gpu is set (then SwitchAccelStub stands in, and Core Animation renders on the CPU):
//   1  M1: VDD_GPU on, clocks/reset/clamp/MC flush (nvgpu gm20b_tegra_unrailgate order), BOOT_0, PRIV ring, GPCPLL lock
//   2  M2: + nvkm device init (ACR HS boot, LS PMU), GR (FECS/GPCCS through ACR, golden context)
//   3  M3: + GPU self-test (VM, GPFIFO channel, copy-engine fill, semaphore fence)
//   4  M4: + publish AGXAccelerator : IOAcceleratorES (MetalPluginName TegraGM20BMetal) for the NVK userland
// The work runs on its own kernel thread, so the probe returns at once; switch_root_boot_prepare() calls it once the
// SD disk is published. From the end of stage 1 the GPU interrupts (SPI 157/158) drive nvkm; there is no polled mode.
// Logs one line per stage reached ("tegra-gm20b: M1 ok ..."), or the line of the stage that failed.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <stdarg.h>
#include "../SwitchSuspend/SwitchWake.h"
#include "TegraGM20BGpu.h"
#include "TegraGM20BSleep.h"

extern "C" {
boolean_t PE_parse_boot_argn(const char *arg_string, void *arg_ptr, int max_arg);
kern_return_t OSKextLoadKextWithIdentifier(const char *kextIdentifier);
int nvxnu_trace_paced(void);  // shim/xnu_glue.cpp
// The staged accelerator class (TegraGM20BAccelerator.cpp, a late object like SwitchAccelStub.cpp): weak, so this file links
// and stages 1..3 work without it.
void tegra_gm20b_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_TegraGM20BAccelerator.cpp")
	__attribute__((weak));
IOService *tegra_gm20b_accelerator_allocate(void) __attribute__((weak));
unsigned tegra_gm20b_accelerator_instance_size(void) __attribute__((weak));
}
extern kmod_info_t g_kernel_kmod_info;

// drivers/SwitchPower/SwitchThermal.cpp: a CPU raise held back for the bring-up goes ahead (a no-op before that service
// runs).
extern "C" void switch_thermal_gpu_ready(void);

extern "C" int vsnprintf(char *, size_t, const char *, va_list);

namespace
{
char gLine[256];
volatile int gStarted, gBringupDone;
// From tegra_gm20b_suspend() to the end of tegra_gm20b_resume(): the wake's power-up, GPCPLL raise and GR init are the
// bring-up's again, so SwitchThermal holds its CPU raise for them as it does at boot (tegra_gm20b_bringup_busy()).
volatile int gSleeping;

// The bring-up has ended (done or failed): SwitchThermal's CPU raise, which waits for it on the shared MAX77812, goes
// ahead at once, and a system sleep waiting for it goes on.
void bringupDone()
{
	__atomic_store_n(&gBringupDone, 1, __ATOMIC_RELEASE);
	thread_wakeup(const_cast<int *>(&gBringupDone));
	switch_thermal_gpu_ready();
}

// System sleep may not power the GPU down under its bring-up thread. No bring-up can start later: tegra_gm20b_probe()
// runs when the SD disk is published, before the userland that asks for sleep exists.
void waitBringup()
{
	constexpr uint32_t kBringupWaitMs = 30000;
	uint64_t deadline;
	clock_interval_to_deadline(kBringupWaitMs, kMillisecondScale, &deadline);
	while (!__atomic_load_n(&gBringupDone, __ATOMIC_ACQUIRE)) {
		assert_wait_deadline(const_cast<int *>(&gBringupDone), THREAD_UNINT, deadline);
		if (__atomic_load_n(&gBringupDone, __ATOMIC_ACQUIRE)) {
			clear_wait(current_thread(), THREAD_AWAKENED);
			break;
		}
		if (thread_block(THREAD_CONTINUE_NULL) == THREAD_TIMED_OUT &&
		    !__atomic_load_n(&gBringupDone, __ATOMIC_ACQUIRE))
			panic("tegra-gm20b: system sleep: the GPU bring-up still runs %u ms into "
			      "it",
			      kBringupWaitMs);
	}
}

// Staged as UniXNU-sheng's publishMetal() (package/GpuPowerProbe.cpp:1217-1251) and SwitchAccelStubPublish.cpp: load
// IOGPUFamily (already loaded by the stub; idempotent) so the IOAcceleratorES metaclass exists, run our class's static
// initializer inside preModLoad/postModLoad, then publish with the keys the original discovery reads.
// NativeSandboxGrantProcess: the comma-separated process names whose open our XNU grants in place of the sandbox
// verdict (IOUserClient.cpp native_sandbox_grant(), as in sheng's zzw-native-adreno-sandbox-grant.patch), each matched
// on at most its first 16 characters.
IOReturn publish()
{
	if (!tegra_gm20b_accelerator_static_init || !tegra_gm20b_accelerator_allocate ||
	    !tegra_gm20b_accelerator_instance_size)
		return kIOReturnUnsupported;
	kern_return_t kr = OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
	if (kr)
		return kr;
	const OSSymbol *name = OSSymbol::withCString("IOAcceleratorES");
	const OSMetaClass *parent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
	OSSafeReleaseNULL(name);
	if (!parent || parent->getClassSize() != 136 ||
	    tegra_gm20b_accelerator_instance_size() != 136)
		return kIOReturnNotFound;
	void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
	if (!handle)
		return kIOReturnNoResources;
	tegra_gm20b_accelerator_static_init();
	IOReturn ret = OSMetaClass::postModLoad(handle);
	if (ret)
		return ret;
	IOService *accelerator = tegra_gm20b_accelerator_allocate();
	if (!accelerator)
		return kIOReturnNoMemory;
	if (!accelerator->init()) {
		accelerator->release();
		return kIOReturnError;
	}
	accelerator->setProperty("MetalPluginName", "TegraGM20BMetal");
	accelerator->setProperty("MetalPluginClassName", "TegraGM20BMetalDevice");
	// OpenGL ES (libGFXShared's _gfxInitializeLibrary) loads the renderer this names, as on J120's AGXG9G: Apple's
	// GLES over Metal, which runs on the Metal device above. Without it every EAGLContext is nil.
	accelerator->setProperty("IOGLESBundleName", "AppleMetalGLRenderer");
	// backboardd is the render server. iconservicesagent: with no Metal device Core Image 21H461 falls back to OpenGL ES,
	// which has no driver here, and the agent crash-loops on every SF Symbol (Settings then waits on it forever).
	// ScreenshotServicesService (the screenshot thumbnail and markup): PencilKit's -[PKMetalResourceHandler
	// newPaintShaderKernelPipelineState] takes a nil device's nil paint_kernel and Metal aborts the process
	// (setComputeFunction: nil), so every screenshot's thumbnail dies 1-2 s after the flash. Names match on their first 16
	// characters, so this also admits ClassroomKit's ScreenshotService; the plugin compares the whole name and gives that
	// one no device. Every other process too: "*" is any name for native_sandbox_grant()
	// (IOUserClient.cpp); a kernel without that change matches "*" to no process and still grants the named ones.
	accelerator->setProperty(
		"NativeSandboxGrantProcess",
		tgm_all_clients() ? "*,backboardd,iconservicesagent,ScreenshotServicesService"
				  : "backboardd,iconservicesagent,ScreenshotServicesService");
	if (!accelerator->attach(IOService::getPlatform())) {
		accelerator->release();
		return kIOReturnNotAttached;
	}
	accelerator->registerService();
	accelerator->release();
	return kIOReturnSuccess;
}

void bringup(void *arg, wait_result_t)
{
	const int stage = static_cast<int>(reinterpret_cast<intptr_t>(arg));
	for (int s = 1; s <= (stage < 3 ? stage : 3); s++) {
		const int ret = tgm_bringup(s, gLine, sizeof(gLine));
		IOLog("tegra-gm20b: %s\n", gLine[0] ? gLine : "no result");
		if (ret) {
			bringupDone();
			return;
		}
		// Bring-up: the text console lags the log, and logd flushes every few seconds; let both catch up so a stage
		// that hangs the machine still leaves the previous stage's result on screen and on Data.
		if (nvxnu_trace_paced())
			IOSleep(3000);
	}
	if (stage >= 4) {
		const IOReturn ret = publish();
		if (ret)
			IOLog("tegra-gm20b: M4 FAIL publish AGXAccelerator 0x%x\n", ret);
		else
			IOLog("tegra-gm20b: M4 ok published AGXAccelerator (MetalPluginName "
			      "TegraGM20BMetal)\n");
	}
	bringupDone();
}

bool gpuDisabled()
{
	unsigned off = 0;
	return PE_parse_boot_argn("switch-no-gpu", &off, sizeof(off)) && off;
}
}  // namespace

// 1 from boot until the bring-up (VDD_GPU on, GPCPLL raise, ACR/PMU/GR, self-test) has finished or failed: for a driver
// whose own rail or clock changes must not overlap it (SwitchThermal's CPU raise on the same MAX77812, which the board
// thread starts before tegra_gm20b_probe()). 0 with switch-no-gpu.
extern "C" int tegra_gm20b_bringup_busy(void)
{
	static int wanted = -1;
	if (wanted < 0)
		wanted = !gpuDisabled();
	return wanted && (!__atomic_load_n(&gBringupDone, __ATOMIC_ACQUIRE) ||
			  __atomic_load_n(&gSleeping, __ATOMIC_ACQUIRE));
}

extern "C" void tgm_journal(const char *fmt, ...)
{
	char line[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	switch_power_journal("%s", line);
}

extern "C" void tegra_gm20b_suspend(void)
{
	if (!__atomic_load_n(&gStarted, __ATOMIC_ACQUIRE))
		return;
	waitBringup();
	__atomic_store_n(&gSleeping, 1, __ATOMIC_RELEASE);
	tgm_sleep();
}

extern "C" void tegra_gm20b_resume(void)
{
	if (!__atomic_load_n(&gSleeping, __ATOMIC_ACQUIRE))
		return;
	tgm_wake();
	__atomic_store_n(&gSleeping, 0, __ATOMIC_RELEASE);
	switch_thermal_gpu_ready();
}

extern "C" int tegra_gm20b_probe(void)
{
	constexpr int kStage = 4;
	if (gpuDisabled())
		return 0;
	if (__atomic_exchange_n(&gStarted, 1, __ATOMIC_SEQ_CST))
		return 0;
	thread_t thread;
	if (kernel_thread_start(bringup, reinterpret_cast<void *>(static_cast<intptr_t>(kStage)),
				&thread) != KERN_SUCCESS) {
		IOLog("tegra-gm20b: could not start the bring-up thread\n");
		bringupDone();
		return -1;
	}
	thread_deallocate(thread);
	return 0;
}

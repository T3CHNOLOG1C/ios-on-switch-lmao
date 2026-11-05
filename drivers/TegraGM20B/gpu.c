// SPDX-License-Identifier: MIT
/*
 * GM20B bring-up and the per-connection GPU model, on top of the vendored nouveau nvkm/nvif code.
 * Mirrors what nouveau's DRM driver does for the same jobs:
 *   bring-up      nouveau_platform.c (gm20b_platform_data) + nvkm_device_tegra_new() + nvkm_device_init(), the latter
 *                 as m2_device_init() (no fini pass, GPCPLL up before L2/FB/PMU, as nvgpu)
 *   address space nouveau_uvmm.c VM_INIT (RAW vmm, kernel range [1<<39, 1<<40)) and VM_BIND (raw get/map/unmap)
 *   memory        nvkm_mem_new_host() with a caller-owned DMA address array (nvif_mem_ram_v0.dma)
 *   channels      nouveau_chan.c nouveau_channel_ctor()/_init() + nvif/chan906f.c GPFIFO/pushbuf helpers
 *   submission    nouveau_exec.c (one GPFIFO entry per push) + nv84/gf100 fence semaphore release
 * Interrupts, no polled mode: from the end of M1 the GPU's stall/nonstall SPIs drive nvkm_intr from a work loop
 * (nvxnu_irq_start, shim/xnu_glue.cpp) and every wait sleeps in nvxnu_idle(); each fence also raises a nonstall
 * interrupt (NON_STALL_INTERRUPT after the semaphore release, as nouveau's nvc0_fence_emit32) that a per-channel
 * NON_STALL_INTR event turns into a wake. A fence found complete with no wake panics (nvxnu_wake_due).
 * GPU L2 (boot-arg tegra-gpu-l2=1, off by default): user mappings are L2-cacheable instead of volatile, as nvgpu maps
 * the buffers its clients ask to be cacheable (gk20a/mm_gk20a.c __update_pte: VOL only for !cacheable); see l2_on().
 * GPU present (boot-arg tegra-gpu-present=1): the display's frames are swizzled into its block-linear scanout by the copy
 * engine on a kernel channel that waits for the compositor's work on the GPU (see "GPU present" below).
 * GPU failures panic from the end of the self-test (M3) on, each with one line naming it: an error nvkm's interrupt
 * handler reports (MMU fault, PBDMA, GR, FECS, ...: nvxnu_error), a channel nvkm kills, and work that stops completing
 * with no error at all (fence waits, sync_for_cpu, GPFIFO full, channel teardown: tgm_chan_hung). With
 * tegra-gpu-all-clients=1 an error nvkm pins on one channel of a client other than the compositor kills only that
 * channel (see "client fault isolation").
 */
#include <core/device.h>
#include <core/tegra.h>
#include <engine/device/priv.h> /* nvkm_device_fini: system sleep */
#include <engine/fifo.h>
#include <engine/fifo/runl.h> /* nvkm_runl_chan_get_chid, nvkm_runl_rc_cgrp: client fault isolation */
#include <engine/fifo/cgrp.h> /* nvkm_cgrp.rc: a channel nvkm itself put into recovery */
#include <core/gpuobj.h>      /* nvkm_wo32 on a channel's RAMFC: compositor timeslice */
#include <engine/gr.h>
#include <subdev/acr.h>
#include <subdev/clk.h>
#include <subdev/clk/priv.h> /* nvkm_clk_func: clk_raise() runs calc/prog as gm20b_clk_init() does */
#include <subdev/clk/gk20a.h> /* gk20a_clk.parent_rate: clk_measure()'s reference */
#include <subdev/devinit.h>
#include <subdev/fb.h>
#include <subdev/ltc.h>
#include <subdev/timer.h>
#include <subdev/top.h>
#include <nvif/class.h>
#include <nvif/cl0080.h>
#include <nvif/client.h>
#include <nvif/device.h>
#include <nvif/driver.h>
#include <nvif/event.h>
#include <nvif/fifo.h>
#include <nvif/if000a.h>
#include <nvif/if0020.h>
#include <nvif/if900d.h>
#include <nvif/ifb00d.h>
#include <nvif/mem.h>
#include <nvif/mmu.h>
#include <nvif/parent.h>
#include <nvif/vmm.h>
#include <nvif/chan.h>
#include <nvif/push906f.h>
#include <nvhw/class/cla0b5.h>
#include "TegraGM20BGpu.h"
#include "TegraGM20BSleepGate.h"

/* mainline tegra210.dtsi gpu@57000000: BAR0, BAR1, SPI 157 ("stall") / 158 ("nonstall") */
static struct resource gpu_res[] = {
	{.start = 0x57000000, .end = 0x57ffffff, .name = "bar0", .flags = IORESOURCE_MEM},
	{.start = 0x58000000, .end = 0x58ffffff, .name = "bar1", .flags = IORESOURCE_MEM},
};
static struct platform_device gpu_pdev = {
	.name = "gpu",
	.id = 0,
	.dev = {.init_name = "57000000.gpu"},
	.num_resources = 2,
	.resource = gpu_res,
	.irq_stall = 157 + 32,
	.irq_nonstall = 158 + 32,
};
/* nouveau_platform.c gm20b_platform_data */
static const struct nvkm_device_tegra_func gm20b_platform_data = {
	.iommu_bit = 34,
	.require_vdd = true,
	.require_ref_clk = true,
};

static struct {
	int stage;
	struct nvkm_device *device;
	u32 boot0;
	struct nvif_client client;
	struct nvif_device dev;
	struct nvif_mmu mmu;
	int mem_type;
	struct mutex lock; /* serialises every nvif call (nouveau holds cli->mutex likewise) */
} gpu = {.lock = {NULL}};

int tgm_stage(void)
{
	return gpu.stage;
}
uint32_t tgm_wake_seq(void)
{
	return nvxnu_wake_seq();
}
int tgm_idle(uint32_t seq, uint64_t deadline_ns)
{
	return nvxnu_idle(seq, deadline_ns);
}
void tgm_fence_unwoken(uint32_t seq, uint32_t seqno)
{
	nvxnu_wake_due(seq, "fence", seqno);
}

static int badf(u32 v)
{
	return (v & 0xffff0000) == 0xbadf0000;
}

/* System sleep's gate (TegraGM20BSleepGate.h, "system sleep" below): what touches the GPU outside gpu.lock goes through it. */
static struct tgm_gate sc7_gate;
static DEFINE_MUTEX(gate_lock);

static void gate_wake_closer(void)
{
	mutex_lock(&gate_lock);
	nvxnu_wakeup(&sc7_gate.inside);
	mutex_unlock(&gate_lock);
}

/* In, waiting for the wake while the gate is shut. */
static void gate_enter(void)
{
	enum tgm_gate_entry entry;

	while ((entry = tgm_gate_try_enter(&sc7_gate)) != TGM_GATE_IN) {
		mutex_lock(&gate_lock);
		if (entry == TGM_GATE_SHUT_WAKE)
			nvxnu_wakeup(&sc7_gate.inside);
		while (tgm_gate_is_shut(&sc7_gate))
			nvxnu_mutex_sleep(gate_lock.lck, &sc7_gate.shut, ~0ULL);
		mutex_unlock(&gate_lock);
	}
}

/* In (1), or refused while the gate is shut (0). */
static int gate_try(void)
{
	enum tgm_gate_entry entry = tgm_gate_try_enter(&sc7_gate);

	if (entry == TGM_GATE_SHUT_WAKE)
		gate_wake_closer();
	return entry == TGM_GATE_IN;
}

static void gate_leave(void)
{
	if (tgm_gate_leave(&sc7_gate))
		gate_wake_closer();
}

static int m1(char *line, unsigned int len)
{
	struct nvkm_device *device;
	u32 boot0, start, gpcs, fbps, cfg, coeff, rail = 0;
	long long v;
	int ret, uv = 0;

	/* Speedo ID 0 keeps nouveau's gm20b clk on the legacy fixed-frequency GPCPLL path (lock-detect, no fused
	 * calibration), which nvgpu also runs on B01's C1 PLL when NA mode is off (clk_gm20b.c:1211-1215,1250-1257).
	 * L4T uses 1 = NAPLL on B01 (speedo-tegra210.c:189), but nouveau's NAPLL path carries T210 B1 constants
	 * (nouveau gm20b.c:99-103, nvgpu clk_gm20b.c:56-69) where B01 needs C1 (clk_gm20b.c:71-85,
	 * platform_gk20a_tegra.c:815-817), so it is refused. */
	/* Bring-up trace: let the console's backlog drain first, so every step line below is on screen before its step. */
	if (nvxnu_trace_paced())
		nvxnu_usleep(2000000);
	nvxnu_step("tegra-gm20b: M1 step: start\n");
	if (nvxnu_bootarg_int("tegra-gpu-speedo-id", &v) && v) {
		scnprintf(line, len,
			  "M1 FAIL tegra-gpu-speedo-id=%lld: nouveau NAPLL has T210 B1 params, B01 "
			  "needs C1",
			  v);
		return -EINVAL;
	}
	ret = tegra_gm20b_platform_map();
	if (ret) {
		scnprintf(line, len, "M1 FAIL map CAR/PMC/FUSE/MC %d", ret);
		return ret;
	}
	/* T210B01 fuses the GPU speedo value in CPU_SPEEDO_2 (L4T speedo-tegra210.c:286-294) */
	tegra_sku_info.gpu_speedo_value = (int)tegra_gm20b_fuse_read(0x030);
	tegra_sku_info.gpu_speedo_id = 0;
	nvxnu_step("tegra-gm20b: M1 step: CAR/PMC/FUSE/MC mapped, GPU speedo %d; pool\n",
		   tegra_sku_info.gpu_speedo_value);
	ret = nvxnu_pool_setup();
	if (ret) {
		scnprintf(line, len, "M1 FAIL pool %d", ret);
		return ret;
	}
	{
		char dbg[32] = "error";
		nvxnu_bootarg_str("tegra-gpu-debug", dbg, sizeof(dbg));
		/* power-up (shim/tegra_shim.c), then BOOT_0 and every subdev constructor */
		ret = nvkm_device_tegra_new(&gm20b_platform_data, &gpu_pdev, "", dbg, &device);
	}
	if (ret) {
		scnprintf(line, len, "M1 FAIL nvkm_device_tegra_new %d", ret);
		return ret;
	}
	gpu.device = device;
	nvxnu_step("tegra-gm20b: M1 step: device constructed, claim GPU interrupts\n");
	/* nvkm's handler is recorded (request_irq in nvkm_device_ctor) and nothing is armed yet: claim the GPU lines
	 * before the first nvkm init that may wait */
	nvxnu_irq_start();
	boot0 = nvkm_rd32(device, 0x000000);
	gpu.boot0 = boot0;
	tegra_gm20b_rail_get_uv(&uv);
	nvxnu_step("tegra-gm20b: M1 step: BOOT_0 %08x, rail %d uV; init PRI ring\n", boot0, uv);

	ret = nvkm_subdev_init(device->privring);
	if (!ret)
		ret = nvkm_subdev_init(&device->timer->subdev);
	start = nvkm_rd32(device,
			  0x120050); /* PRI_RINGMASTER_START_RESULTS, bit 0 = connectivity pass */
	fbps = nvkm_rd32(device, 0x120074); /* PRI_RINGMASTER_ENUM_FBP */
	gpcs = nvkm_rd32(device, 0x120078); /* PRI_RINGMASTER_ENUM_GPC */
	nvxnu_step("tegra-gm20b: M1 step: ring start %08x gpc %08x fbp %08x ret %d; init clk "
		   "(GPCPLL)\n",
		   start, gpcs, fbps, ret);
	if (!ret)
		ret = nvkm_subdev_init(&device->clk->subdev);
	if (!ret) {
		/* nouveau's clk init puts the bypass path back on the 1/31 boot divider (GPC2CLK_OUT_INIT_VAL BYPDIV31,
		 * clk/gk20a.h:91-99); nvgpu runs it 1:1 (gm20b_init_clk_setup_hw, clk_gm20b.c:1362-1371). The GPU is on the
		 * VCO here, so this changes nothing now; any later trip through bypass (PLL reprogramming) then stays at
		 * 38.4 MHz instead of running the whole GPU, PRI ring included, at 1.2 MHz. */
		nvkm_mask(device, 0x137250, 0x0000003f, 0x00000000);
		nvxnu_step("tegra-gm20b: M1 step: GPC2CLK_OUT %08x (bypass divider 1:1), SEL_VCO "
			   "%08x\n",
			   nvkm_rd32(device, 0x137250), nvkm_rd32(device, 0x137100));
	}
	cfg = nvkm_rd32(device, 0x137000);   /* GPCPLL_CFG: bit 0 enable, bit 17 lock */
	coeff = nvkm_rd32(device, 0x137004); /* GPCPLL_COEFF: M 7:0, N 15:8, PL 21:16 */
	rail = uv;
	scnprintf(line, len,
		  "M1 %s boot0=%08x chip=%03x rail=%uuV ring=%s gpc=%u fbp=%u gpcpll cfg=%08x "
		  "coeff=%08x %s %dkHz ret=%d",
		  ret ? "FAIL" : "ok", boot0, (boot0 >> 20) & 0x1ff, rail,
		  badf(start)   ? "BADF"
		  : (start & 1) ? "pass"
				: "fail",
		  gpcs & 0x1f, fbps & 0x1f, cfg, coeff,
		  (cfg & 0x20001) == 0x20001 ? "locked" : "UNLOCKED",
		  ret ? 0 : nvkm_clk_read(device->clk, nv_clk_src_gpc), ret);
	if (!ret && ((boot0 >> 20) & 0x1ff) != 0x12b && ((boot0 >> 20) & 0x1ff) != 0x12e)
		ret = -ENODEV;
	return ret;
}

/* GPCPLL above 76.8 MHz (boot-arg tegra-gpu-mhz):
 * Off unless tegra-gpu-mhz=<MHz> names one of L4T's B01 GPU rates (76.8 MHz x n; 460 or 461 both mean 460.8). Runs
 * between M1 and M2, with the GPU as quiet as at the end of M1's own clk init (PRI ring, timer and GPCPLL up, no
 * falcon running yet), so the relock below happens under the conditions M1 has already proven; ACR, PMU, GR (M2) and
 * the channel test (M3) then run at the new rate, as nvgpu runs them at the rate its PLL holds (217.6 MHz on a
 * first power-on, gm20b_init_clk_setup_sw() clk_gm20b.c:1230-1242; the last DVFS rate after a railgate).
 *
 * PLL: nouveau's legacy fixed-frequency path (speedo ID 0, see m1), gk20a_clk_calc() + gk20a_clk_prog() as
 * gm20b_clk_init() runs them: NDIV slide to NDIV_LO, GPU on bypass (38.4 MHz with M1's 1:1 divider), relock with lock
 * detect, NDIV slide up. For every L4T rate it picks M=1 (38.4 MHz update rate) and the same N/PL that B01's C1 PLL
 * limits allow (VCO 1300-2600 MHz, u 19.2-38.4 MHz: nvgpu gpc_pll_params_c1, clk_gm20b.c:71-85), so this mode needs no
 * C1 parameters.
 *
 * Voltage: L4T has only noise-aware (NAPLL) tables for B01 and WARNs without NA mode (tegra210-dvfs.c:821-929,2089);
 * platform.c's tegra_gm20b_dvfs_na_uv() reproduces them. A fixed-frequency PLL cannot back off under supply droop and
 * needs more: on T210, where L4T has both, the fixed table asks 6-19% more than the NA table at equal speedo
 * (FIXED_FREQ_CVB_TABLE vs NA_FREQ_CVB_TABLE, :682-746, evaluated at speedo 1400-1900). So a rate is taken only if its
 * worst-case NA voltage plus FIXED_PLL_MARGIN_PCT fits the rail ceiling, 800 mV (switchroot's cap,
 * nx-common.dtsi:548). The rail is held at that ceiling for every rate: it is also L4T's floor at any rate below 20 C
 * (gpu_scaling_cdev trip0, nx-common.dtsi:1365), and nothing here reads the GPU temperature. Voltage goes up (with
 * readback) before the clock; this hook never lowers it.
 * For GPU speedo 1673 the 20% margin admits up to 768 MHz (worst NA 665 mV); HOS also stops retail units at 768 MHz
 * (nx-common.dtsi:521-523). tegra-gpu-pll-margin=<pct> (13..50) lowers it for a test: 13 is T210's fixed/NA ratio at
 * 921.6 MHz for speedo 1700 (+10..13% across 768-921.6), the only measured basis for less, and admits 844.8 MHz
 * (685 mV + 13% = 774 mV) but not 921.6 (710 mV + 13% > 800 mV). */
#define FIXED_PLL_MARGIN_PCT 20
#define FIXED_PLL_MARGIN_MIN 13
#define BOOT_KHZ 76800 /* gm20b_pstates[0], set by gm20b_clk_init() in M1 */

static int clk_prog_khz(struct nvkm_device *device, u32 khz)
{
	struct nvkm_clk *clk = device->clk;
	struct nvkm_cstate cstate = {.domain[nv_clk_src_gpc] = khz};
	int ret = clk->func->calc(clk, &cstate);

	if (!ret) {
		ret = clk->func->prog(clk);
		clk->func->tidy(clk);
	}
	return ret;
}

/* One CLK trace line after a PLL change: GPCPLL state and, over the PRI ring, whether the ring survived (as M1 reads
 * it). Returns 1 if the GPU runs from a locked GPCPLL at `khz` with the 1:1 bypass divider and a live ring. */
static int clk_state(struct nvkm_device *device, const char *what, int ret, u32 khz)
{
	u32 cfg = nvkm_rd32(device, 0x137000);   /* GPCPLL_CFG: 0 enable, 17 lock */
	u32 coeff = nvkm_rd32(device, 0x137004); /* GPCPLL_COEFF: M 7:0, N 15:8, PL 21:16 */
	u32 vco = nvkm_rd32(device, 0x137100);   /* SEL_VCO: 0 = GPU on bypass */
	u32 out =
		nvkm_rd32(device, 0x137250); /* GPC2CLK_OUT: 5:0 bypass divider, 13:8 VCO divider */
	u32 ring = nvkm_rd32(device,
			     0x120050); /* PRI_RINGMASTER_START_RESULTS: 0 = connectivity pass */
	u32 bus = nvkm_rd32(device, 0x001100); /* BUS_INTR_0: 3 = PRI timeout */
	int now = nvkm_clk_read(device->clk, nv_clk_src_gpc);

	nvxnu_step("tegra-gm20b: CLK step: %s, ret %d: GPCPLL_CFG %08x COEFF %08x (M %u N %u PL "
		   "%u) SEL_VCO %x "
		   "GPC2CLK_OUT %08x = %d kHz; ring %08x BUS_INTR %08x\n",
		   what, ret, cfg, coeff, coeff & 0xff, (coeff >> 8) & 0xff, (coeff >> 16) & 0x3f,
		   vco, out, now, ring, bus);
	return !ret && (cfg & 0x20001) == 0x20001 && (vco & 1) && !(out & 0x3f3f) &&
	       now == (int)khz && !badf(ring) && (ring & 1) && !(bus & 0x8);
}

static int clk_raise(char *line, unsigned int len)
{
	struct nvkm_device *device = gpu.device;
	int speedo = tegra_sku_info.gpu_speedo_value, ceil_uv = tegra_gm20b_rail_max_uv();
	int uv = 0, na_uv, need_uv, ret, back, i, fits = 1, margin = FIXED_PLL_MARGIN_PCT;
	u32 khz = 0, best = 0, k;
	long long mhz, pct;

	if (!nvxnu_bootarg_int("tegra-gpu-mhz", &mhz))
		return 0;
	if (nvxnu_bootarg_int("tegra-gpu-pll-margin", &pct))
		margin = pct < FIXED_PLL_MARGIN_MIN ? FIXED_PLL_MARGIN_MIN
			 : pct > 50                 ? 50
						    : (int)pct;
	for (i = 0; (k = tegra_gm20b_dvfs_khz(i)); i++) {
		int v = tegra_gm20b_dvfs_na_uv(k, speedo);
		if (mhz > 0 && mhz < 10000 && mhz * 1000 > (long long)k - 1000 &&
		    mhz * 1000 < (long long)k + 1000)
			khz = k;
		fits = fits && v > 0 && DIV_ROUND_UP(v * (100 + margin), 100) <= ceil_uv;
		if (fits)
			best = k; /* L4T drops every rate above the first one that does not fit */
	}
	if (!khz) {
		nvxnu_log("tegra-gm20b: tegra-gpu-mhz=%lld is not an L4T B01 GPU rate (76.8 MHz x "
			  "1..16, 1267.2): GPU stays at "
			  "%d kHz\n",
			  mhz, nvkm_clk_read(device->clk, nv_clk_src_gpc));
		return 0;
	}
	if (nvkm_clk_read(device->clk, nv_clk_src_gpc) == (int)khz)
		return 0; /* the boot rate, or a retried stage 2 */
	na_uv = tegra_gm20b_dvfs_na_uv(khz, speedo);
	if (na_uv < 0) {
		nvxnu_log("tegra-gm20b: tegra-gpu-mhz=%lld refused: no B01 table voltage for GPU "
			  "speedo %d; GPU stays at %d kHz\n",
			  mhz, speedo, nvkm_clk_read(device->clk, nv_clk_src_gpc));
		return 0;
	}
	need_uv = DIV_ROUND_UP(na_uv * (100 + margin), 100);
	if (need_uv > ceil_uv) {
		nvxnu_log("tegra-gm20b: tegra-gpu-mhz=%lld refused: %u kHz needs %d uV in "
			  "fixed-frequency mode (B01 NA table %d "
			  "uV + %d%%) for GPU speedo %d, above the %d uV rail ceiling; GPU stays "
			  "at %d kHz (highest rate that fits: "
			  "%u kHz)\n",
			  mhz, khz, need_uv, na_uv, margin, speedo, ceil_uv,
			  nvkm_clk_read(device->clk, nv_clk_src_gpc), best);
		return 0;
	}
	ret = tegra_gm20b_rail_get_uv(&uv);
	nvxnu_step("tegra-gm20b: CLK step: tegra-gpu-mhz=%lld -> %u kHz, GPU speedo %d: B01 NA "
		   "table %d uV, +%d%% fixed PLL "
		   "%d uV, ceiling %d uV (highest rate that fits %u kHz); VDD_GPU %d uV (ret %d)\n",
		   mhz, khz, speedo, na_uv, margin, need_uv, ceil_uv, best, uv, ret);

	/* voltage first */
	if (!ret && uv < ceil_uv) {
		ret = tegra_gm20b_rail_set_uv(
			ceil_uv); /* MAX77812 write + readback, then the ramp delay */
		if (!ret)
			ret = tegra_gm20b_rail_get_uv(&uv);
		nvxnu_step("tegra-gm20b: CLK step: VDD_GPU raised to %d uV (ret %d) before the "
			   "clock\n",
			   uv, ret);
	}
	if (ret || uv < ceil_uv) {
		nvxnu_log("tegra-gm20b: tegra-gpu-mhz=%lld: VDD_GPU %d uV (ret %d), %d uV needed "
			  "first; GPU stays at %d kHz\n",
			  mhz, uv, ret, ceil_uv, nvkm_clk_read(device->clk, nv_clk_src_gpc));
		return 0;
	}

	/* then the clock */
	nvxnu_step("tegra-gm20b: CLK step: GPCPLL_CFG %08x COEFF %08x = %d kHz -> %u kHz (NDIV "
		   "slide, bypass, relock, slide)\n",
		   nvkm_rd32(device, 0x137000), nvkm_rd32(device, 0x137004),
		   nvkm_clk_read(device->clk, nv_clk_src_gpc), khz);
	ret = clk_prog_khz(device, khz);
	if (clk_state(device, "raised", ret, khz)) {
		/* the one result line (nvxnu_step prints only with tegra-gpu-trace): the rate the PLL now holds, from
		 * its own registers; the stats line measures it every window (clk_measure) */
		u32 coeff = nvkm_rd32(device, 0x137004);
		nvxnu_log("tegra-gm20b: GPU clock %d kHz (GPCPLL M %u N %u PL %u, "
			  "tegra-gpu-mhz=%lld), "
			  "VDD_GPU %d uV\n",
			  nvkm_clk_read(device->clk, nv_clk_src_gpc), coeff & 0xff,
			  (coeff >> 8) & 0xff, (coeff >> 16) & 0x3f, mhz, uv);
		return 0;
	}

	/* not at the target: one way back to the M1 rate, at the same voltage */
	back = clk_prog_khz(device, BOOT_KHZ);
	if (clk_state(device, "back to the boot rate", back, BOOT_KHZ)) {
		nvxnu_log("tegra-gm20b: tegra-gpu-mhz=%lld: GPCPLL did not settle at %u kHz (ret "
			  "%d); back at %u kHz\n",
			  mhz, khz, ret, BOOT_KHZ);
		return 0;
	}
	scnprintf(line, len,
		  "M2 FAIL GPCPLL to %u kHz (ret %d) and back to %u kHz (ret %d): GPCPLL_CFG %08x "
		  "COEFF %08x "
		  "SEL_VCO %x",
		  khz, ret, BOOT_KHZ, back, nvkm_rd32(device, 0x137000),
		  nvkm_rd32(device, 0x137004), nvkm_rd32(device, 0x137100));
	return -EIO;
}

/* One M2 trace line: the host's PMC/bus registers (always answer) and, over the PRI ring, the ring master's interrupt
 * status and the GPCPLL output, so a step that leaves the ring or the GPU clock dead shows on the next line. */
static void m2_step(struct nvkm_device *device, const char *what, const char *name)
{
	u32 pmc = nvkm_rd32(device, 0x000200);  /* PMC_ENABLE */
	u32 elpg = nvkm_rd32(device, 0x00020c); /* PMC_ELPG_ENABLE: xbar 2, pfb 20, hub 29 */
	u32 bus = nvkm_rd32(device, 0x001100);  /* BUS_INTR_0: 1 squash, 2 fecserr, 3 PRI timeout */
	u32 ring = nvkm_rd32(
		device, 0x120058); /* PRI_RINGMASTER_INTR_STATUS0: 0 start fault, 1 disconnect */
	u32 pll = nvkm_rd32(device, 0x137000); /* GPCPLL_CFG: 0 enable, 1 IDDQ, 17 lock */
	u32 vco = nvkm_rd32(device, 0x137100); /* SEL_VCO: 0 = GPU on bypass, 1 = on GPCPLL */
	u32 out = nvkm_rd32(device, 0x137250); /* GPC2CLK_OUT: 5:0 bypass divider */

	nvxnu_step("tegra-gm20b: M2 step: %s%s: PMC_ENABLE %08x ELPG %08x BUS_INTR %08x ring %08x "
		   "GPCPLL %08x SEL_VCO %x "
		   "GPC2CLK_OUT %08x\n",
		   what, name, pmc, elpg, bus, ring, pll, vco, out);
}

/* nvkm_device_init() (engine/device/base.c) without its nvkm_device_fini() pass, one trace line per subdev init.
 * M1 left privring, timer and clk running, GPU on the GPCPLL. nvkm_device_init() first fini's every subdev, which drops
 * the GPU to bypass (gk20a_clk_fini: SEL_VCO bypass, PLL off, IDDQ) on the 1/31 divider nouveau's clk init re-armed,
 * then inits in core/layout.h order: privring (PRI ring reset through PMC_ENABLE bit 5 right after a posted write of
 * that divider, privring/gk20a.c:29-33), fb, ltc, mmu, bar, acr (HS ACR), pmu (LS firmware start), volt, and clk only
 * once the PMU firmware runs. That order timed out at the first L2 access (ltc zbc 0x17e338), then on the PMU's own
 * PRI reads around the point clk reprograms the PLL. nvgpu's gk20a_finalize_poweron()
 * (gk20a/gk20a.c:195-254) brings up bus, priv ring, clk (GPCPLL on, slowdown-on-boot divider removed) and only then
 * fifo, ltc and mm.
 * Without the fini, nvkm_subdev_init() passes over the three running subdevs and the rest come up in nvgpu's order on
 * the GPCPLL. The GPU left CAR reset in M1, so the fini pass's engine PMC resets have nothing to clear. */
static int m2_device_init(struct nvkm_device *device, const char **at)
{
	struct nvkm_subdev *subdev;
	int ret;

	/* nvkm_device_preinit(); Tegra has no device preinit and GM20B no devinit (nvkm_devinit_post takes NULL) */
	nvkm_intr_unarm(device);
	list_for_each_entry(subdev, &device->subdev, head)
	{
		*at = subdev->name;
		ret = nvkm_subdev_preinit(subdev);
		if (ret)
			return ret;
	}
	*at = "preinit";
	ret = nvkm_devinit_post(device->devinit);
	if (!ret)
		ret = nvkm_top_parse(device);
	if (!ret)
		ret = nvkm_fb_mem_unlock(device->fb);
	if (ret)
		return ret;
	m2_step(device, "preinit done, privring/timer/clk still up from M1", "");

	nvkm_intr_rearm(device);
	list_for_each_entry(subdev, &device->subdev, head)
	{
		/* what nvkm_subdev_init() skips: running since M1, or an engine with no user yet (GR comes up by ref below) */
		if (subdev->use.enabled || !refcount_read(&subdev->use.refcount))
			continue;
		*at = subdev->name;
		if (subdev == &device->fb->subdev) {
			/* nvgpu gm20b_mc_fb_reset() (common/mc/mc_gm20b.c:340-351), run by nvgpu_init_mm_reset_enable_hw()
			 * (common/mm/mm.c:347-351) before any FB/MMU programming; nouveau never writes it */
			u32 elpg = nvkm_rd32(device, 0x00020c);
			nvkm_wr32(device, 0x00020c, elpg | 0x00000004 | 0x00100000 | 0x20000000);
			nvxnu_step("tegra-gm20b: M2 step: ELPG_ENABLE %08x -> %08x (xbar|pfb|hub, "
				   "nvgpu gm20b_mc_fb_reset)\n",
				   elpg, nvkm_rd32(device, 0x00020c));
		}
		m2_step(device, "init ", subdev->name);
		ret = nvkm_subdev_init(subdev);
		if (ret)
			return ret;
	}
	/* nvkm_acpi_init() and nvkm_therm_clkgate_enable() have nothing to do here: no ACPI, no therm subdev on GM20B */
	m2_step(device, "subdevs up, next GR (FECS/GPCCS booted by the PMU)", "");
	return 0;
}

/* nvif's object printer (NVIF_ERROR/NVIF_ERRON in nvif/printf.h dereference object->parent->func): nouveau_drm.c installs
 * one on its client before creating objects; without it the first nvif error message is a NULL dereference (a failed
 * channel event in M3 panics in nvif_event_ctor_). Errors go to the log, debug output nowhere. */
static void tgm_nvif_errorf(struct nvif_object *object, const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	(void)object;
	va_start(ap, fmt);
	nvxnu_vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	nvxnu_log("tegra-gm20b nvif: %s", buf);
}

static void tgm_nvif_debugf(struct nvif_object *object, const char *fmt, ...)
{
	(void)object;
	(void)fmt;
}

static const struct nvif_parent_func tgm_nvif_parent_func = {
	.debugf = tgm_nvif_debugf,
	.errorf = tgm_nvif_errorf,
};
static struct nvif_parent tgm_nvif_parent;

static int m2(char *line, unsigned int len)
{
	struct nvkm_device *device = gpu.device;
	struct nvkm_engine *gr;
	const char *at = "";
	u64 units = 0;
	int ret = m2_device_init(
		device,
		&at); /* fb, ltc, mmu, bar, ACR (HS on PMU), LS PMU, volt; privring/clk: M1 */

	if (ret) {
		scnprintf(line, len, "M2 FAIL nvkm init at %s: %d", at, ret);
		return ret;
	}
	/* The device reference nvkm_udevice_init() takes for the first nvif device (engine/device/user.c:277-295): held
	 * here, so the nvif device made below does not run nvkm_device_init() again (its fini pass would drop the GPU to
	 * bypass and reset GR under the running PMU) */
	mutex_lock(&device->mutex);
	device->refcount++;
	mutex_unlock(&device->mutex);

	gr = nvkm_engine_ref(
		&device->gr->engine); /* oneinit+init: FECS/GPCCS via ACR, golden context */
	ret = IS_ERR(gr) ? (int)PTR_ERR(gr) : 0;
	if (!ret)
		units = nvkm_gr_units(device->gr);
	scnprintf(
		line, len,
		"M2 %s wpr=[%llx-%llx] acr=%s fecs_mb0=%08x gpccs_cpuctl=%08x gpc=%u tpc=%u ret=%d",
		ret ? "FAIL" : "ok", device->acr ? device->acr->wpr_start : 0,
		device->acr ? device->acr->wpr_end : 0,
		device->acr && device->acr->done ? "done" : "no", nvkm_rd32(device, 0x409040),
		nvkm_rd32(device, 0x41a100), (u32)(units & 0xff), (u32)((units >> 8) & 0xff), ret);
	if (ret)
		return ret;
	/* GR is up: this line is the stage's real result, so it reaches the log even if the nvif setup below fails */
	nvxnu_step("tegra-gm20b: %s\n", line);

	mutex_init(&gpu.lock);
	at = "nvif_driver_init";
	ret = nvif_driver_init(NULL, "", "error", "tegra-gm20b", device->handle, &gpu.client);
	if (!ret) {
		/* every object made below inherits this printer (nvif/object.c: object->parent = parent->parent) */
		nvif_parent_ctor(&tgm_nvif_parent_func, &tgm_nvif_parent);
		gpu.client.object.parent = &tgm_nvif_parent;
		at = "nvif_device_ctor";
		ret = nvif_device_ctor(&gpu.client, "dev", &gpu.dev);
	}
	if (!ret) {
		/* the MMU class is a child of the device object, as nouveau_drm.c creates it (on the client: -22) */
		at = "nvif_mmu_ctor";
		ret = nvif_mmu_ctor(&gpu.dev.object, "mmu", NVIF_CLASS_MMU_GF100, &gpu.mmu);
	}
	if (!ret) {
		at = "nvif_mmu_type";
		gpu.mem_type = nvif_mmu_type(&gpu.mmu, NVIF_MEM_HOST | NVIF_MEM_KIND);
		if (gpu.mem_type < 0)
			gpu.mem_type = nvif_mmu_type(&gpu.mmu, NVIF_MEM_HOST);
		ret = gpu.mem_type < 0 ? gpu.mem_type : 0;
	}
	if (ret)
		scnprintf(line, len, "M2 FAIL %s %d (GR came up: see the M2 ok line above)", at,
			  ret);
	return ret;
}

void tgm_get_info(struct tgm_info *info)
{
	u64 units = gpu.stage >= 2 ? nvkm_gr_units(gpu.device->gr) : 0;
	memset(info, 0, sizeof(*info));
	if (!gpu.device)
		return;
	info->boot0 = gpu.boot0;
	info->chipset = gpu.device->chipset;
	info->gpc_count = units & 0xff;
	info->tpc_count = (units >> 8) & 0xffff;
}

uint64_t tgm_timestamp_ns(void)
{
	u64 t;
	if (gpu.stage < 2)
		return 0;
	mutex_lock(&gpu.lock);
	t = nvif_device_time(&gpu.dev);
	mutex_unlock(&gpu.lock);
	return t;
}

void *tgm_pool_alloc(uint64_t bytes, uint64_t *pa)
{
	unsigned long long h = 0;
	void *va = nvxnu_pool_alloc_clean(bytes, &h);
	*pa = va ? h : 0;
	return va;
}

void tgm_pool_free_zeroed(void *va, uint64_t bytes, uint64_t pa)
{
	memset(va, 0, bytes);
	/* the write-combining buffers drained before the pages are handed out again */
	__asm__ volatile("dsb sy" ::: "memory");
	nvxnu_pool_free_zeroed(pa, bytes);
}

static void scrub_kick(void);

/* Back dirty: the scrubber zeroes it on the GPU (see "pool scrub") unless an allocation takes it first and zeroes it. */
void tgm_pool_free(void *va, uint64_t bytes, uint64_t pa)
{
	dma_free_attrs(&gpu_pdev.dev, bytes, va, pa, 0);
	scrub_kick();
}

uint64_t tgm_pool_avail(void)
{
	return nvxnu_pool_size() - ((u64)nvxnu_pool_used_pages() << PAGE_SHIFT);
}

/* GPU L2 for user memory (tegra-gpu-l2=1):
 * GM20B's L2 caches system memory unless a PTE is volatile; the GPU does not snoop the CPU and the CPU does not see the
 * L2. Volatile user mappings (the default, as nouveau maps all system memory) keep every texture fetch and ROP access
 * out of the L2. With the boot-arg, user mappings are cacheable and the L2 is kept coherent at submission boundaries,
 * which are Metal's visibility points:
 *  - CPU -> GPU: before a submission is kicked, the CPU invalidates the L2's clean system-memory lines (FLUSH
 *    L2_SYSTEM_INVALIDATE 0x70004), so what the CPU wrote since (WC pool BOs, cleaned cached BOs, imports, push
 *    buffers, which the host may prefetch) is read from DRAM. Dirty lines are not touched by it (nvgpu
 *    gk20a_mm_l2_invalidate_locked) and there are none from finished work, see next.
 *  - GPU -> CPU: the kernel's fence push writes the L2's dirty lines back (host WFI, FB_FLUSH, MEM_OP_D L2_FLUSH_DIRTY,
 *    FB_FLUSH; clb06f.h: gm20x moved MEM_OP_A/B to C/D) before it releases the seqno, so anyone who sees a fence complete
 *    (WaitFence, the client's mapped fence page, sync_for_cpu) finds the data in DRAM.
 *  - Pages leave the GPU only after their fences passed (BoFree's zombies, channel teardown), so no dirty line of a
 *    page the kernel gives back is written back later; nvgpu flushes on unmap for the same reason
 *    (common/mm/gmmu.c gk20a_gmmu_cache_maint_unmap). nvkm invalidates the L2 after its own page-table writes
 *    (instmem/gk20a.c release). Kernel mappings (rings, fence pages) stay volatile. */
#define FLUSH_L2_SYSTEM_INVALIDATE \
	0x070004         /* nvgpu hw_flush_gm20b.h: bit 0 pending, bit 1 outstanding */
#define L2_OP_NS 1000000 /* nvgpu allows 200 x 5 us for the invalidate */
static DEFINE_MUTEX(l2_lock);
static struct {
	u64 sum, max;
	u32 n;
} l2_time; /* tegra-gpu-stats: the invalidate's CPU time, lock wait included */

static int l2_on(void)
{
	static int on = -1;
	long long v = 0;
	if (on < 0)
		on = nvxnu_bootarg_int("tegra-gpu-l2", &v) && v;
	return on;
}

static void l2_invalidate(void)
{
	struct nvkm_device *device = gpu.device;
	u64 start = nvxnu_nanotime(), end, took;
	u32 v;

	mutex_lock(&l2_lock);
	nvkm_wr32(device, FLUSH_L2_SYSTEM_INVALIDATE, 0x00000001);
	end = nvxnu_nanotime() + L2_OP_NS;
	while ((v = nvkm_rd32(device, FLUSH_L2_SYSTEM_INVALIDATE)) & 0x00000003) {
		if (nvxnu_nanotime() >= end)
			nvxnu_panic("tegra-gm20b: L2 system invalidate still busy after 1 ms "
				    "(FLUSH_L2_SYSTEM_INVALIDATE %08x)",
				    v);
		nvxnu_udelay(1);
	}
	took = nvxnu_nanotime() - start;
	l2_time.sum += took;
	l2_time.n++;
	if (took > l2_time.max)
		l2_time.max = took;
	mutex_unlock(&l2_lock);
}

struct tgm_vm {
	struct nvif_vmm vmm;
	struct list_head kmaps;
	u64 pfence_va; /* the present channel's fence page, read-only (tegra-gpu-present-order), 0: not mapped */
	u64 fences_va; /* every connection's fence page, read-only (tgm_chan_order's acquires), 0: not mapped */
	struct list_head
		imports; /* struct tgm_import: first pages of the host imports made in it (live_lock) */
	int imports_lost; /* one could not be recorded: taken as importing every page */
};
struct tgm_import {
	struct list_head head;
	u64 pa;
	u32 refs;
};
struct tgm_mem {
	struct nvif_mem mem;
	u64 bytes;
};
struct tgm_kmap {
	struct list_head head;
	struct nvif_vma vma;
};

static int vm_kmap(struct tgm_vm *vm, struct tgm_mem *mem, uint64_t bytes, int read_only,
		   uint64_t *va);
static int fences_map(struct tgm_vm *vm);

/* The present channel's fence page (see "GPU present"): one pool page, made with the first address space that needs it
 * and kept for good, so it exists before the present channel does. With tegra-gpu-present-order (on unless =0) every
 * address space maps it read-only in its kernel range, where user channels wait on it with a semaphore acquire. */
static struct {
	void *va;
	u64 pa;
	struct tgm_mem *mem;
} pfence;
static DEFINE_MUTEX(pfence_lock);

static int present_order_on(void)
{
	static int on = -1;
	long long v = 1;
	if (on < 0) {
		nvxnu_bootarg_int("tegra-gpu-present-order", &v);
		on = v != 0;
	}
	return on;
}

static int pfence_get(struct tgm_vm *vm)
{
	int ret = 0;
	mutex_lock(&pfence_lock);
	if (!pfence.mem) {
		if (!pfence.va)
			pfence.va = tgm_pool_alloc(PAGE_SIZE, &pfence.pa);
		ret = pfence.va ? tgm_mem_new(vm, &pfence.pa, 1, &pfence.mem) : -ENOMEM;
	}
	mutex_unlock(&pfence_lock);
	return ret;
}

int tgm_vm_new(struct tgm_vm **pvm)
{
	/* bigpage selects the big-page size, 16 (64 KiB) or 17 (128 KiB, nouveau's default); anything else is -EINVAL
	 * (vmmgm200.c gm200_vmm_new_; 0 fails M3). 4 KiB pages stay available, and the ABI binds only those. */
	struct gm200_vmm_v0 args = {.version = 0, .bigpage = 17};
	struct tgm_vm *vm;
	int ret;

	if (gpu.stage < 2)
		return -ENODEV;
	if (!(vm = kzalloc(sizeof(*vm), GFP_KERNEL)))
		return -ENOMEM;
	INIT_LIST_HEAD(&vm->kmaps);
	INIT_LIST_HEAD(&vm->imports);
	mutex_lock(&gpu.lock);
	/* RAW: the client manages [0, 1<<39) with raw binds; nvkm allocates in [1<<39, 1<<40) */
	ret = nvif_vmm_ctor(&gpu.mmu, "uvmm", NVIF_CLASS_VMM_GM200, RAW, TGM_KERNEL_VA,
			    TGM_VA_LIMIT - TGM_KERNEL_VA, &args, sizeof(args), &vm->vmm);
	mutex_unlock(&gpu.lock);
	if (ret) {
		kfree(vm);
		return ret;
	}
	if (present_order_on()) {
		ret = pfence_get(vm);
		if (!ret)
			ret = vm_kmap(vm, pfence.mem, PAGE_SIZE, 1, &vm->pfence_va);
		if (ret) {
			tgm_vm_del(&vm);
			return ret;
		}
	}
	/* for the task order's acquires (tgm_chan_order); without the view, Order refuses and the client orders itself */
	if (fences_map(vm))
		vm->fences_va = 0;
	*pvm = vm;
	return 0;
}

void tgm_vm_del(struct tgm_vm **pvm)
{
	struct tgm_vm *vm = *pvm;
	struct tgm_kmap *k, *t;
	struct tgm_import *im, *it;
	if (!vm)
		return;
	list_for_each_entry_safe(im, it, &vm->imports, head) /* no channel of it is left to look */
		kfree(im);
	mutex_lock(&gpu.lock);
	list_for_each_entry_safe(k, t, &vm->kmaps, head)
	{
		nvif_vmm_put(&vm->vmm, &k->vma);
		list_del(&k->head);
		kfree(k);
	}
	nvif_vmm_dtor(&vm->vmm);
	mutex_unlock(&gpu.lock);
	kfree(vm);
	*pvm = NULL;
}

int tgm_mem_new(struct tgm_vm *vm, const uint64_t *pas, uint32_t npages, struct tgm_mem **pmem)
{
	struct nvif_mem_ram_v0 args = {.version = 0, .dma = (dma_addr_t *)pas, .sgl = NULL};
	struct tgm_mem *mem;
	int ret;
	(void)vm;
	if (gpu.stage < 2)
		return -ENODEV;
	if (!(mem = kzalloc(sizeof(*mem), GFP_KERNEL)))
		return -ENOMEM;
	mem->bytes = (u64)npages << PAGE_SHIFT;
	mutex_lock(&gpu.lock);
	ret = nvif_mem_ctor_type(&gpu.mmu, "bo", NVIF_CLASS_MEM_GF100, gpu.mem_type, PAGE_SHIFT,
				 mem->bytes, &args, sizeof(args), &mem->mem);
	mutex_unlock(&gpu.lock);
	if (ret) {
		kfree(mem);
		return ret;
	}
	*pmem = mem;
	return 0;
}

void tgm_mem_del(struct tgm_mem **pmem)
{
	if (!*pmem)
		return;
	mutex_lock(&gpu.lock);
	nvif_mem_dtor(&(*pmem)->mem);
	mutex_unlock(&gpu.lock);
	kfree(*pmem);
	*pmem = NULL;
}

/* Sysmem is mapped volatile (GPU L2 does not keep its lines): CONTRACT.md §3(b) option one; cacheable with
 * tegra-gpu-l2=1 (l2_on). Compressible kinds are not offered (no comptags); nvkm rejects invalid kinds. */
static int vm_map(struct tgm_vm *vm, uint64_t va, uint64_t range, struct tgm_mem *mem,
		  uint64_t offset, uint8_t kind, int read_only, int vol)
{
	struct gf100_vmm_map_v0 args = {
		.version = 0, .vol = !!vol, .ro = !!read_only, .priv = 0, .kind = kind};
	int ret;
	if (va < TGM_USER_VA_START || range > TGM_KERNEL_VA || va > TGM_KERNEL_VA - range ||
	    offset > mem->bytes || range > mem->bytes - offset ||
	    !nvif_mmu_kind_valid(&gpu.mmu, kind))
		return -EINVAL;
	mutex_lock(&gpu.lock);
	ret = nvif_vmm_raw_get(&vm->vmm, va, range, PAGE_SHIFT);
	if (!ret) {
		ret = nvif_vmm_raw_map(&vm->vmm, va, range, PAGE_SHIFT, &args, sizeof(args),
				       &mem->mem, offset);
		if (ret)
			nvif_vmm_raw_put(&vm->vmm, va, range, PAGE_SHIFT);
	}
	mutex_unlock(&gpu.lock);
	return ret;
}

int tgm_vm_map(struct tgm_vm *vm, uint64_t va, uint64_t range, struct tgm_mem *mem, uint64_t offset,
	       uint8_t kind, int read_only)
{
	return vm_map(vm, va, range, mem, offset, kind, read_only, !l2_on());
}

int tgm_vm_unmap(struct tgm_vm *vm, uint64_t va, uint64_t range)
{
	int ret;
	if (va < TGM_USER_VA_START || range > TGM_KERNEL_VA || va > TGM_KERNEL_VA - range)
		return -EINVAL;
	mutex_lock(&gpu.lock);
	ret = nvif_vmm_raw_unmap(&vm->vmm, va, range, PAGE_SHIFT, false);
	nvif_vmm_raw_put(&vm->vmm, va, range, PAGE_SHIFT);
	mutex_unlock(&gpu.lock);
	return ret;
}

int tgm_vm_kmap(struct tgm_vm *vm, struct tgm_mem *mem, uint64_t bytes, uint64_t *va)
{
	return vm_kmap(vm, mem, bytes, 0, va);
}

static int vm_kmap(struct tgm_vm *vm, struct tgm_mem *mem, uint64_t bytes, int read_only,
		   uint64_t *va)
{
	struct gf100_vmm_map_v0 args = {
		.version = 0, .vol = 1, .ro = !!read_only, .priv = 0, .kind = 0};
	struct tgm_kmap *k = kzalloc(sizeof(*k), GFP_KERNEL);
	int ret;
	if (!k)
		return -ENOMEM;
	mutex_lock(&gpu.lock);
	ret = nvif_vmm_get(&vm->vmm, ADDR, false, PAGE_SHIFT, 0, bytes, &k->vma);
	if (!ret) {
		ret = nvif_vmm_map(&vm->vmm, k->vma.addr, bytes, &args, sizeof(args), &mem->mem, 0);
		if (ret)
			nvif_vmm_put(&vm->vmm, &k->vma);
	}
	if (!ret)
		list_add(&k->head, &vm->kmaps);
	mutex_unlock(&gpu.lock);
	if (ret) {
		kfree(k);
		return ret;
	}
	*va = k->vma.addr;
	return 0;
}

#define CHAN_PUSH_BYTES 0x10000  /* nouveau_channel_ctor: plength */
#define CHAN_GPFIFO_BYTES 0x2000 /* ilength: 1024 entries */
#define CHAN_SEMA_BYTES \
	0x2000 /* nvif's GET semaphore at +0; tegra-gpu-stats stamps at CHAN_STAMPS, push profile at CHAN_PROF */
#define CHAN_BYTES (CHAN_PUSH_BYTES + CHAN_GPFIFO_BYTES + CHAN_SEMA_BYTES)
#define CHAN_STAMPS (CHAN_PUSH_BYTES + CHAN_GPFIFO_BYTES + 0x800)
#define CHAN_PROF                              \
	(CHAN_PUSH_BYTES + CHAN_GPFIFO_BYTES + \
	 0x1000) /* one 16-byte stamp per push, TGM_PROF_MAX */
#define CHAN_NSTAMPS \
	64 /* by seqno; one record = two 16-byte semaphore releases (payload, 0, PTIMER ns) */

/* What a 16-byte semaphore release writes (NV906F SEMAPHORED RELEASE_SIZE_16BYTE) */
struct tgm_stamp {
	u32 payload, pad;
	u64 ns;
};
struct tgm_stamps {
	struct tgm_stamp start,
		end; /* the GPU starts the submission (WFI: the one before finished) / finished it */
};

/* An acquire tgm_chan_order queued for a channel's next submission: another channel's fence word reaching seqno. */
struct tgm_order {
	u64 va;                  /* the source's fence word in the waiter's address space */
	const volatile u32 *cpu; /* and in the kernel's */
	u64 id;                  /* the source channel */
	u32 seqno;
};
struct tgm_chan {
	struct tgm_vm *vm;
	struct nvif_object user;
	struct nvif_object obj[5];
	struct nvif_event kill;
	struct nvif_event nonstall;
	struct nvif_chan chan;
	void *ring; /* push | gpfifo | sema, pool memory */
	u64 ring_pa, ring_va;
	u64 *ring_pas;
	struct tgm_mem *ring_mem;
	struct nvif_vma ring_vma;
	volatile u32 *fence_cpu;
	u64 fence_gpu;
	u32 submitted;
	u32 error;
	u32 chid;
	int nobj;
	int kernel; /* a kernel channel, no client waits on it and the present does not wait for it: CHAN_PRESENT, CHAN_SCRUB */
	int critical; /* a GPU error on it panics even with tegra-gpu-all-clients (compositor, kernel) */
	int isolating; /* client fault isolation: its error was taken as its own, the kill is under way */
	struct list_head live; /* live_chans, for tegra_gm20b_sync_for_cpu */
	u64 id;
	char owner[17];  /* process that created it */
	char fault[200]; /* the nvkm report it was isolated for */
	u64 submit_ns;   /* when the last fence was kicked */
	u32 stall_done;  /* tgm_chan_check_progress: completed seqno first seen stalled, and when */
	u64 stall_ns;
	u32 gov_done; /* idle clock: last seqno counted retired (gov_fenced) */
	u32 stat_done; /* tegra-gpu-stats: last seqno whose latency was counted, submit times by seqno */
	u64 stat_sub_ns[CHAN_NSTAMPS];
	u64 stat_sub_ptimer[CHAN_NSTAMPS];
	u64 stat_exec,
		stat_exec_max; /* tegra-gpu-stats: this window's stamped exec time (PTIMER units), for its owner */
	u32 stat_exec_n;
	/* push profile: the profiled submission in flight (0: none), its pushes' classes, and this window's GPU time by
	 * class (PTIMER units) and the pushes counted, for its owner */
	u32 prof_seqno, prof_count, prof_subs;
	u8 prof_cls[TGM_PROF_MAX];
	u16 prof_draws[TGM_PROF_MAX], prof_sdraws[TGM_PROF_MAX];
	u64 prof_ns[TGM_PC_COUNT];
	u32 prof_n[TGM_PC_COUNT];
	u64 prof_dn[TGM_PC_COUNT],
		prof_sdn[TGM_PC_COUNT]; /* draws, and draws shaded per sample, by class */
	/* tgm_chan_order: its connection's task (tgm_chan_set_group), the acquires queued for its next submission
	 * (live_lock), and the last one kicked (a hang's line names it while it has not passed) */
	const void *group;
	u32 norder;
	struct tgm_order order[TGM_ORDER_MAX];
	struct tgm_order waited;
};

#define CHAN_PRESENT 1 /* the GPU present's channel */
#define CHAN_SCRUB 2   /* the pool scrubber's */

/* Every user channel, so a CPU reader outside the GPU client can wait for all GPU work (tegra_gm20b_sync_for_cpu). */
static LIST_HEAD(live_chans);
static DEFINE_MUTEX(live_lock);
static u64 live_ids;

/* Host imports per address space, by first physical page: who can write a surface the GPU present copies (only an
 * address space that maps its pages; an IOSurface is imported from its page-aligned base, see "GPU present"). */
void tgm_vm_import_note(struct tgm_vm *vm, uint64_t pa, int add)
{
	struct tgm_import *im;

	mutex_lock(&live_lock);
	list_for_each_entry(im, &vm->imports, head)
	{
		if (im->pa == pa)
			break;
	}
	if (&im->head == &vm->imports) {
		im = add ? kzalloc(sizeof(*im), GFP_KERNEL) : NULL;
		if (im) {
			im->pa = pa;
			list_add(&im->head, &vm->imports);
		} else if (add) {
			vm->imports_lost = 1;
		}
	}
	if (im && add) {
		im->refs++;
	} else if (im && !--im->refs) {
		list_del(&im->head);
		kfree(im);
	}
	mutex_unlock(&live_lock);
}

static int vm_imports(struct tgm_vm *vm, u64 pa) /* live_lock held */
{
	struct tgm_import *im;
	if (vm->imports_lost)
		return 1;
	list_for_each_entry(im, &vm->imports, head)
	{
		if (im->pa == pa)
			return 1;
	}
	return 0;
}

/* host-import coherency registry:
 * Lazy CPU<->GPU coherency for imported host memory (IOSurfaces, no-copy buffers): the render path pays nothing, and the
 * cache maintenance is paid by whoever touches a surface on the CPU, at its IOSurface lock/unlock (the XNU hook that calls
 * tgm_coherency_*). The GPU does not snoop the CPU caches on Tegra.
 *  - GPU -> CPU: a writable import the GPU may have written is marked dirty at each submission (tgm_coherency_wrote, from the
 *    UserClient's exec). The reader's read lock invalidates the surface's lines once and clears the mark, so the CPU reads
 *    DRAM (what the GPU wrote), not lines it cached before (a fresh surface's zeros: 16x1 black runs in a launch
 *    snapshot rendered on the GPU and read back on the CPU to encode a PNG).
 *  - CPU -> GPU: a writer's non-read-only unlock of a surface the GPU imported cleans its lines, so the GPU reads what the
 *    CPU wrote; read-only imports included (the GPU reads what the app redraws into them). A per-commit clean in the
 *    frontend would see only in-process writers, and a surface's writers are often other processes.
 *  - Import: the importer always cleans the surface's lines, for what the CPU wrote before the GPU imported it, even when
 *    another import of the same pages is live. An earlier import proves nothing about writes since: no-copy buffers (Core
 *    Animation's shared-memory images, most read-only imports) never see an IOSurface lock, and IOSurfaceLock/Unlock are
 *    IOKit traps (IOConnectTrap3, index 2/3), not the external methods the XNU hook watches, so no unlock cleans
 *    anything. Without the clean a recycled backing store reaches the GPU with its newest lines still in the CPU caches
 *    (text as 16x1 blocks of old and new, a selected row showing the buffer's previous text). Cleaning costs ~80 us per
 *    MiB.
 * Entries are keyed by the surface's first physical page (its page-aligned base, as the import and the XNU hook both have it),
 * in a hash under coh_lock for the hook's lookups. Each import holds a reference to its entry, which stays where it was made,
 * so the per-submission dirty mark is one store through the import's own pointer, no lookup and no lock. */
struct tgm_coh {
	struct tgm_coh *next; /* hash chain (coh_lock) */
	u64 pa; /* the surface's first physical page: the key, as the import and the XNU hook both have it */
	u64 *pas; /* the largest page list imported for it, so the cache ops reach a non-contiguous surface */
	u32 npages, refs;
	int writable;  /* some import lets the GPU write it: submissions mark it GPU-dirty */
	int gpu_dirty; /* atomic: the GPU may have written it since the last CPU read lock */
};
/* Registers this driver's read_lock/write_unlock with the kernel's IOSurface lock/unlock hook (XNU IOUserClient.cpp). */
extern void tegra_coherency_register(uint64_t (*read_lock)(uint64_t),
				     uint64_t (*write_unlock)(uint64_t));
#define TGM_COH_BUCKETS 256
static struct tgm_coh *coh_hash[TGM_COH_BUCKETS];
static DEFINE_MUTEX(coh_lock);
/* tegra-gpu-stats: the IOSurface lock/unlock hooks' work (one 5 s window), on the push line */
static struct {
	u64 inval_n, inval_bytes, clean_n, clean_bytes;
} coh_stats;

static struct tgm_coh **
coh_slot(u64 pa) /* coh_lock held: the link that points at pa's entry, or the chain's end */
{
	struct tgm_coh **p =
		&coh_hash[((pa >> PAGE_SHIFT) ^ (pa >> (PAGE_SHIFT + 8))) & (TGM_COH_BUCKETS - 1)];
	while (*p && (*p)->pa != pa)
		p = &(*p)->next;
	return p;
}

/* A host import of the surface whose pages are pas[0..npages) was made: its entry, with a reference for the import (NULL
 * when there is no memory for one: the import then simply has no lazy coherency). The importer
 * cleans the pages itself, always (see above). `writable` is 0 for a read-only import. */
struct tgm_coh *tgm_coherency_add(u64 pa, const u64 *pas, u32 npages, int writable)
{
	struct tgm_coh **p, *c;
	if (!npages)
		return NULL;
	mutex_lock(&coh_lock);
	p = coh_slot(pa);
	c = *p;
	if (c && npages > c->npages) {
		/* a larger import of the same surface: keep its page list, so the hooks reach every page either imports */
		u64 *copy = kmalloc_array(npages, sizeof(u64), GFP_KERNEL);
		if (copy) {
			memcpy(copy, pas, (size_t)npages * sizeof(u64));
			kfree(c->pas);
			c->pas = copy;
			c->npages = npages;
		}
	} else if (!c && (c = kzalloc(sizeof(*c), GFP_KERNEL))) {
		c->pas = kmalloc_array(npages, sizeof(u64), GFP_KERNEL);
		if (!c->pas) {
			kfree(c);
			c = NULL;
		} else {
			memcpy(c->pas, pas, (size_t)npages * sizeof(u64));
			c->pa = pa;
			c->npages = npages;
			*p = c;
		}
	}
	if (c) {
		c->refs++;
		c->writable |= writable;
	}
	mutex_unlock(&coh_lock);
	return c;
}

/* The import holding `c` was released. */
void tgm_coherency_put(struct tgm_coh *c)
{
	struct tgm_coh **p;
	if (!c)
		return;
	mutex_lock(&coh_lock);
	if (!--c->refs) {
		p = coh_slot(c->pa);
		if (*p == c)
			*p = c->next;
		kfree(c->pas);
		kfree(c);
	}
	mutex_unlock(&coh_lock);
}

/* A submission that may write the writable import holding `c`: mark it dirty, so the next CPU read lock invalidates it.
 * Called from the UserClient's exec, per writable import: one store, no lookup, no lock, no cache maintenance. */
void tgm_coherency_wrote(struct tgm_coh *c)
{
	if (c)
		__atomic_store_n(&c->gpu_dirty, 1, __ATOMIC_RELEASE);
}

/* The XNU IOSurface read-lock hook (a CPU read lock, not the GPU's): if the surface whose first page is `pa` is a GPU
 * import the GPU may have written, invalidate its pages for the CPU (so the read sees DRAM, what the GPU wrote, not lines
 * the CPU cached before) and clear the mark. Nothing for a surface that is not our import or that the GPU has not written
 * since the last read lock. Cache ops are by VA through the kernel's physical aperture, broadcast within the inner-
 * shareable domain (no IPI). Returns the bytes invalidated (0 if nothing). */
u64 tgm_coherency_read_lock(u64 pa)
{
	struct tgm_coh *c;
	u64 bytes = 0;
	mutex_lock(&coh_lock);
	c = *coh_slot(pa);
	if (c && __atomic_exchange_n(&c->gpu_dirty, 0, __ATOMIC_ACQ_REL)) {
		tgm_cpu_clean_pages(
			c->pas,
			c->npages); /* clean+invalidate: no dirty CPU line survives either */
		bytes = (u64)c->npages << PAGE_SHIFT;
		coh_stats.inval_n++;
		coh_stats.inval_bytes += bytes;
	}
	mutex_unlock(&coh_lock);
	return bytes;
}

/* The XNU IOSurface write-unlock hook (a CPU writer unlocking, not read-only): if the surface is a GPU import (read-only
 * imports included: the GPU reads what the CPU wrote), clean its pages so the GPU reads what the CPU wrote. A pure-CPU
 * surface (not our import) is not touched and costs nothing. A writer that stores into an imported surface without
 * IOSurfaceLock/Unlock is NOT covered here: the import-time clean (BoImport) covers writes before the import, but writes
 * made after it with no lock reach no hook (as on stock Metal, where such a write is a program error). Returns the bytes
 * cleaned (0 if not ours). */
u64 tgm_coherency_write_unlock(u64 pa)
{
	struct tgm_coh *c;
	u64 bytes = 0;
	mutex_lock(&coh_lock);
	c = *coh_slot(pa);
	if (c) {
		tgm_cpu_clean_pages(c->pas, c->npages);
		bytes = (u64)c->npages << PAGE_SHIFT;
		coh_stats.clean_n++;
		coh_stats.clean_bytes += bytes;
	}
	mutex_unlock(&coh_lock);
	return bytes;
}

uint32_t tgm_chan_submitted(struct tgm_chan *chan)
{
	return chan->submitted;
}
uint32_t tgm_chan_completed(struct tgm_chan *chan)
{
	return READ_ONCE(*chan->fence_cpu);
}
uint32_t tgm_chan_error(struct tgm_chan *chan)
{
	return chan->error;
}

/* One line of what the GPU is doing about a channel: owner, hardware channel id, error, completed/submitted fence and
 * how long ago the last one was kicked; GPFIFO GET/PUT as the host sees them (USERD 0x88/0x8c) and the PUT the kernel
 * wrote; PGRAPH_STATUS (0x400700, bit 0 busy), PFIFO engine 0 (GR) status (0x2640); PBDMA 0: status (0x3080, bits 13-15
 * busy, gk104_runq_idle), the channel it serves (0x40120) and the method/data it is on (0x400c0/0x400c4: subchannel
 * 18:16, method 13:2, as gf100_runq_intr decodes them; a NV906F semaphore method there means a semaphore acquire that is
 * not satisfied); PMC_INTR_0; and the memory controller's last refused access (MC_INTSTATUS, MC_ERR_STATUS, MC_ERR_ADR). */
static void chan_describe(struct tgm_chan *chan, char *buf, unsigned int len)
{
	struct nvkm_device *device = gpu.device;
	volatile u32 *userd = chan->user.map.ptr;
	u32 mc_int, mc_err, mc_adr;

	tegra_gm20b_mc_errors(&mc_int, &mc_err, &mc_adr);
	scnprintf(buf, len,
		  "%s ch %u err %u fence %u/%u kicked %llu ms ago, gp %u/%u (put %u), PGRAPH %08x "
		  "ENG0 %08x, "
		  "PBDMA0 %08x ch %u mthd %08x data %08x, PMC %08x, MC %08x err %08x at %08x",
		  chan->owner[0] ? chan->owner : "?", chan->chid, chan->error,
		  tgm_chan_completed(chan), chan->submitted,
		  chan->submit_ns ? (nvxnu_nanotime() - chan->submit_ns) / 1000000 : 0ULL,
		  userd ? userd[0x88 / 4] : 0, userd ? userd[0x8c / 4] : 0, chan->chan.gpfifo.cur,
		  nvkm_rd32(device, 0x400700), nvkm_rd32(device, 0x002640),
		  nvkm_rd32(device, 0x003080), nvkm_rd32(device, 0x040120) & 0xfff,
		  nvkm_rd32(device, 0x0400c0), nvkm_rd32(device, 0x0400c4),
		  nvkm_rd32(device, 0x000100), mc_int, mc_err, mc_adr);
}

void tgm_chan_hung(struct tgm_chan *chan, const char *who, uint32_t seqno, uint64_t waited_ns)
{
	char state[320], order[64] = "";
	const struct tgm_order *w = &chan->waited;
	chan_describe(chan, state, sizeof(state));
	/* still waiting for another channel of its task (tgm_chan_order) */
	if (w->cpu && (s32)(READ_ONCE(*w->cpu) - w->seqno) < 0)
		scnprintf(order, sizeof(order), ", order wait on chan %llu fence %u (at %u)", w->id,
			  w->seqno, READ_ONCE(*w->cpu));
	nvxnu_panic("tegra-gm20b: %s: fence %u still pending after %llu ms and no GPU error "
		    "reported: %s%s",
		    who, seqno, waited_ns / 1000000, state, order);
}

#define TGM_HANG_NS (2 * NSEC_PER_SEC)

void tgm_chan_check_progress(struct tgm_chan *chan, const char *who, uint32_t seqno)
{
	u32 done = tgm_chan_completed(chan);
	u64 now = nvxnu_nanotime(), since = READ_ONCE(chan->stall_ns);

	if ((s32)(done - chan->submitted) >= 0) {
		WRITE_ONCE(chan->stall_ns, 0);
	} else if (!since || READ_ONCE(chan->stall_done) != done) {
		WRITE_ONCE(chan->stall_done, done);
		WRITE_ONCE(chan->stall_ns, now);
	} else if (now - since >= TGM_HANG_NS) {
		tgm_chan_hung(chan, who, seqno, now - since);
	}
}

/* client fault isolation (tegra-gpu-all-clients=1):
 * With every process on the GPU, one app's bad shader or stale address must not take the system down. nvgpu's policy is
 * taken (gk20a_fifo_recover, gk20a_gr_isr: the faulting channel gets an error and only it is torn down): after an
 * interrupt pass in which nvkm reported errors (xnu_glue.cpp gpuInterrupt), errors_contained() takes the channels the
 * reports name ("on channel N [" for MMU faults, "ch N" for PBDMA errors and GR exceptions) and those nvkm put into
 * recovery in that pass without naming them (a context-switch timeout, e.g. a shader that never ends:
 * gf100_fifo_intr_ctxsw_timeout -> nvkm_runl_rc_engn marks the channel on the engine). If each is a client channel
 * that is not critical (not the compositor's, not the present's), the errors are that client's: nvkm's recovery
 * (nvkm_runl_rc_cgrp: runlist blocked and preempted, the channel disabled and removed, an engine still on it reset; nvkm
 * starts it itself for MMU faults, errors_contained() for the rest, which nvkm only reports) kills each one,
 * chan_killed() marks it dead and releases its fence word so no acquire or CPU wait stays on it, and the GPU goes on.
 * Everything else panics: an error on a critical channel, a report naming no channel or an unknown one (one
 * naming none is taken as fallout while a recovery is under way), or a kill not done TGM_ISOLATE_NS after its report.
 * Hangs with no error report still panic (tgm_chan_hung). */
#define TGM_ISOLATE_NS (500 * NSEC_PER_MSEC)

int tgm_all_clients(void)
{
	static int on = -1;
	long long v = 0;
	if (on <
	    0) /* on by default (every process that asks gets a device); tegra-gpu-all-clients=0 turns it off */
		on = nvxnu_bootarg_int("tegra-gpu-all-clients", &v) ? v != 0 : 1;
	return on;
}

static struct {
	void *deadline; /* iso_deadline */
	u64 until;      /* end of the recovery window, 0: none (live_lock) */
} iso;

/* CONTRACT.md error codes: 1 MMU fault, 2 GR exception, 3 host error */
static u32 fault_code(const char *report)
{
	if (strstr(report, "fault "))
		return 1;
	if (strstr(report, "TRAP") || strstr(report, "ILLEGAL_") || strstr(report, "DATA_ERROR"))
		return 2;
	return 3;
}

/* nvkm killed the channel (nvkm_chan_error, from its recovery work). Before M3, M3 reads chan->error. From M3 on: an
 * isolated client channel is dead (see above); any other panics, with the report that started it if the interrupt pass
 * did not panic first. */
static int chan_killed(struct nvif_event *event, void *repv, u32 repc)
{
	struct tgm_chan *chan = container_of(event, struct tgm_chan, kill);
	const char *report = chan->fault[0] ? chan->fault : nvxnu_gpu_errors();
	char state[320];

	(void)repv;
	(void)repc;
	if (gpu.stage >= 3 && (chan->critical || !tgm_all_clients())) {
		chan_describe(chan, state, sizeof(state));
		nvxnu_panic("tegra-gm20b: channel killed by nvkm recovery after \"%s\": %s",
			    nvxnu_gpu_errors(), state);
	}
	if (gpu.stage < 3) {
		if (!chan->error)
			chan->error = 3;
		return NVIF_EVENT_DROP;
	}
	/* the runlist dropped it, so nothing writes its fence any more: release what it will never complete */
	WRITE_ONCE(chan->error, fault_code(report));
	WRITE_ONCE(*chan->fence_cpu, chan->submitted);
	__asm__ volatile("dsb sy" ::: "memory");
	nvxnu_log("tegra-gm20b: GPU error in %s (channel %u): channel killed, GPU running: %s\n",
		  chan->owner, chan->chid, report);
	nvxnu_wake();
	return NVIF_EVENT_DROP;
}

/* The next channel id a report names after p ("channel N [" or "ch N"), -2 when there is none; -1 is nvkm's "unknown". */
static const char *next_chid(const char *p, int *chid)
{
	static const char *const tags[] = {"channel ", "ch "};
	const char *best = NULL, *at;
	unsigned int i, len = 0;

	for (i = 0; i < ARRAY_SIZE(tags); i++) {
		for (at = p; (at = strstr(at, tags[i])); at++) {
			if (at == p || at[-1] == ' ' || at[-1] == '|')
				break;
		}
		if (at && (!best || at < best)) {
			best = at;
			len = strlen(tags[i]);
		}
	}
	if (!best)
		return NULL;
	best += len;
	*chid = -2;
	if (*best == '-' || (*best >= '0' && *best <= '9'))
		*chid = (int)simple_strtol(best, (char **)&best, 10);
	return best;
}

/* 1 if nvkm marked the channel's group for recovery (nvkm_runl_rc_cgrp), or, after a context-switch timeout (which
 * names no channel: nvkm first triggers an MMU fault on the engine, gf100_fifo_intr_ctxsw_timeout, and recovers in the
 * next pass), if an engine is still on it: the one that does not yield. */
static int chan_marked(u32 chid, int ctxsw_timeout)
{
	struct nvkm_runl *runl;
	struct nvkm_engn *engn;
	struct nvkm_chan *nchan;
	unsigned long flags;
	bool cgid;
	int id, marked = 0;

	nvkm_runl_foreach(runl, gpu.device->fifo)
	{
		nchan = nvkm_runl_chan_get_chid(runl, chid, &flags);
		if (!nchan)
			continue;
		marked = atomic_read(&nchan->cgrp->rc) != NVKM_CGRP_RC_NONE;
		nvkm_runl_foreach_engn_cond(engn, runl, ctxsw_timeout && engn->func->cxid)
		{
			id = engn->func->cxid(engn, &cgid);
			marked |= id >= 0 && id == (cgid ? nchan->cgrp->id : nchan->id);
		}
		nvkm_chan_put(&nchan, flags);
		break;
	}
	return marked;
}

static void chan_recover(u32 chid)
{
	struct nvkm_runl *runl;
	struct nvkm_chan *nchan;
	unsigned long flags;

	nvkm_runl_foreach(runl, gpu.device->fifo)
	{
		nchan = nvkm_runl_chan_get_chid(runl, chid, &flags);
		if (nchan) {
			nvkm_runl_rc_cgrp(nchan->cgrp); /* a no-op if nvkm already started it */
			nvkm_chan_put(&nchan, flags);
			return;
		}
	}
}

/* From the GPU interrupt work loop after a pass with error reports (nvxnu_errors_fatal's filter): 1 if they stay with
 * non-critical client channels, whose kills are then under way; 0 to panic. */
static int errors_contained(const char *errors)
{
	struct tgm_chan *named[8], *chan, *found;
	const char *p = errors;
	unsigned int n = 0, i;
	int contained = 1, chid;
	u64 now = nvxnu_nanotime();

	mutex_lock(&live_lock);
	while (contained && (p = next_chid(p, &chid))) {
		if (chid == -2)
			continue; /* "ch" with no number: not an attribution */
		found = NULL;
		list_for_each_entry(chan, &live_chans, live)
		{
			if ((int)chan->chid == chid) {
				found = chan;
				break;
			}
		}
		if (!found || found->critical)
			contained = 0;
		for (i = 0; contained && i < n && named[i] != found; i++)
			;
		if (contained && i == n && n < ARRAY_SIZE(named))
			named[n++] = found;
	}
	/* channels nvkm put into recovery itself, or the one a context-switch timeout is about; not yet dead or isolated */
	list_for_each_entry(chan, &live_chans, live)
	{
		if (!contained || chan->error || chan->isolating ||
		    !chan_marked(chan->chid, strstr(errors, "CTXSW_TIMEOUT") != NULL))
			continue;
		for (i = 0; i < n && named[i] != chan; i++)
			;
		if (chan->critical)
			contained = 0;
		else if (i == n && n < ARRAY_SIZE(named))
			named[n++] = chan;
	}
	if (!n && (!iso.until || now >= iso.until))
		contained = 0;
	if (contained) {
		for (i = 0; i < n; i++) {
			if (named[i]->isolating || named[i]->error)
				continue;
			named[i]->isolating = 1;
			strscpy(named[i]->fault, errors, sizeof(named[i]->fault));
			chan_recover(named[i]->chid);
		}
		if (n) {
			iso.until = now + TGM_ISOLATE_NS;
			nvxnu_work_schedule_after(iso.deadline, TGM_ISOLATE_NS);
		}
	}
	mutex_unlock(&live_lock);
	return contained;
}

static void iso_deadline(void *arg)
{
	struct tgm_chan *chan;
	char state[320];

	(void)arg;
	mutex_lock(&live_lock);
	list_for_each_entry(chan, &live_chans, live)
	{
		if (chan->isolating && !READ_ONCE(chan->error)) {
			chan_describe(chan, state, sizeof(state));
			nvxnu_panic("tegra-gm20b: GPU error in %s not recovered %llu ms after it "
				    "(\"%s\"): %s",
				    chan->owner,
				    (unsigned long long)(TGM_ISOLATE_NS / NSEC_PER_MSEC),
				    chan->fault, state);
		}
	}
	if (iso.until && nvxnu_nanotime() >= iso.until)
		iso.until = 0;
	mutex_unlock(&live_lock);
}

/* tegra-gpu-stats=1:
 * GPU-side times come from 16-byte semaphore releases, which write PTIMER. On this Tegra PTIMER is not in nanoseconds:
 * its source is 19.2 MHz while its 32 ns step assumes 31.25 MHz (nvgpu gm20b_tegra_platform.ptimer_src_freq,
 * os/linux/platform_gk20a_tegra.c; PTIMER_REF_FREQ_HZ, include/nvgpu/ptimer.h), so it runs at 0.6144 x real time and nvgpu
 * scales every PTIMER timeout by it; nouveau's gk20a timer programs no rate (subdev/timer/gk20a.c has no init). The
 * user fence latency (CPU clock) measures 1.63-1.65 x the stamped queue + exec in every window. Each window's
 * stamps are converted to nanoseconds with PTIMER's rate measured over that window against the CPU clock, which the
 * line prints ("PTIMER xR"). */
#define STATS_WINDOW_NS (5 * NSEC_PER_SEC)
static struct {
	int on;         /* -1 until the boot-arg is read */
	u64 since;      /* start of the current window, 0 before the first submission */
	u64 cal_ptimer; /* PTIMER at `since`: its rate over the window */
	u32 submits, fences, imports, timed;
	u32 ordered, idle,
		late; /* present order: submits held behind a copy; offers that found no user work pending; of
				   * those, the ones a user submit followed while the copy was still pending */
	u32 writer; /* presents that waited for their surface's importers only */
	u32 allocs; /* BoAllocs */
	u64 alloc_bytes, alloc_ns;
	u64 lat_sum, lat_max;                         /* user submissions only */
	u64 queue_sum, queue_max, exec_sum, exec_max; /* GPU-side, from the stamps (PTIMER units) */
	u32 copies,
		gaps; /* presents stamped: the copy's own GPU time; the GPU's gap from the end of the last user
				   * work the copy waited for to the copy's start (scheduling and semaphore acquire latency) */
	u64 copy_sum, copy_max, gap_sum, gap_max; /* PTIMER units */
	s64 live_bytes;                           /* host imports */
	s32 live;
} stats = {.on = -1};

static int stats_on(void)
{
	long long v = 0;
	if (stats.on < 0)
		stats.on = nvxnu_bootarg_int("tegra-gpu-stats", &v) && v;
	return stats.on;
}

int tgm_stats_on(void)
{
	return stats_on();
}

static void stats_add(u64 *sum, u64 *max, u64 v)
{
	u64 m = __atomic_load_n(max, __ATOMIC_RELAXED);
	__atomic_fetch_add(sum, v, __ATOMIC_RELAXED);
	while (v > m &&
	       !__atomic_compare_exchange_n(max, &m, v, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
		;
}

/* tegra-gpu-stats: the "mem" line's window (tgm_stats_bo_alloc and the rest, in TegraGM20BGpu.h). Times in ns; every
 * field is summed atomically and taken (zeroed) when the window's line is logged. `recent` holds the last host imports
 * released, by first page, so an import of a surface released less than REIMPORT_NS ago counts as a re-import: what
 * keeping a released import around for a while would have saved. */
#define REIMPORT_NS (2 * NSEC_PER_SEC)
#define REIMPORT_RING 128
static struct {
	u64 cached_n, cached_bytes, cached_ns, cached_max, cached_phase[TGM_PHASES];
	u64 pool_n, pool_bytes, pool_ns, pool_max, small_n;
	u64 import_n, import_bytes, import_ns, import_max, import_phase[TGM_PHASES], import_ro,
		import_skipped, reimports;
	u64 map_n, map_pages, map_ns, map_max, unmap_n, unmap_ns, free_n, free_ns, free_max,
		prefault_n, prefault_bytes;
	u64 spare_n, spare_bytes, spare_ns, spare_max;
} mstat;
/* Who paid the window's slowest cached BoAlloc made on its own thread (best effort: the raiser of cached_max writes it). */
static char mstat_max_owner[17];
static u64 compositor_pool; /* the compositor's pool BOs (tgm_stats_compositor_pool) */
static struct {
	u64 pa, ns;
} recent[REIMPORT_RING];
static u32 recent_next;
static DEFINE_MUTEX(recent_lock);

void tgm_stats_import(int64_t bytes, uint64_t pa)
{
	u64 now;
	u32 i;
	if (bytes > 0)
		__atomic_fetch_add(&stats.imports, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&stats.live, bytes > 0 ? 1 : -1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&stats.live_bytes, bytes, __ATOMIC_RELAXED);
	if (!stats_on())
		return;
	now = nvxnu_nanotime();
	mutex_lock(&recent_lock);
	if (bytes < 0) {
		recent[recent_next++ % REIMPORT_RING] = (typeof(recent[0])){pa, now};
	} else {
		for (i = 0; i < REIMPORT_RING; i++) {
			if (recent[i].pa == pa && recent[i].ns &&
			    now - recent[i].ns < REIMPORT_NS) {
				recent[i].ns = 0;
				mstat.reimports++;
				break;
			}
		}
	}
	mutex_unlock(&recent_lock);
}

void tgm_stats_compositor_pool(int64_t bytes)
{
	__atomic_fetch_add(&compositor_pool, (u64)bytes, __ATOMIC_RELAXED);
}

void tgm_stats_bo_alloc(uint64_t bytes, int kind, uint64_t ns, const uint64_t phase_ns[TGM_PHASES],
			const char *owner)
{
	int i;
	if (!stats_on())
		return;
	__atomic_fetch_add(&stats.allocs, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&stats.alloc_bytes, bytes, __ATOMIC_RELAXED);
	__atomic_fetch_add(&stats.alloc_ns, ns, __ATOMIC_RELAXED);
	if (bytes <= (64 << 10))
		__atomic_fetch_add(&mstat.small_n, 1, __ATOMIC_RELAXED);
	if (kind == TGM_BO_POOL) {
		__atomic_fetch_add(&mstat.pool_n, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&mstat.pool_bytes, bytes, __ATOMIC_RELAXED);
		stats_add(&mstat.pool_ns, &mstat.pool_max, ns);
		return;
	}
	if (kind == TGM_BO_SPARE) {
		__atomic_fetch_add(&mstat.spare_n, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&mstat.spare_bytes, bytes, __ATOMIC_RELAXED);
		stats_add(&mstat.spare_ns, &mstat.spare_max, ns);
		return;
	}
	__atomic_fetch_add(&mstat.cached_n, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&mstat.cached_bytes, bytes, __ATOMIC_RELAXED);
	if (owner && ns > __atomic_load_n(&mstat.cached_max, __ATOMIC_RELAXED))
		strlcpy(mstat_max_owner, owner, sizeof(mstat_max_owner));
	stats_add(&mstat.cached_ns, &mstat.cached_max, ns);
	for (i = 0; i < TGM_PHASES; i++)
		__atomic_fetch_add(&mstat.cached_phase[i], phase_ns[i], __ATOMIC_RELAXED);
}

void tgm_stats_bo_import(uint64_t bytes, int read_only, int skipped, uint64_t ns,
			 const uint64_t phase_ns[TGM_PHASES])
{
	int i;
	if (!stats_on())
		return;
	__atomic_fetch_add(&mstat.import_n, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&mstat.import_bytes, bytes, __ATOMIC_RELAXED);
	__atomic_fetch_add(&mstat.import_ro, !!read_only, __ATOMIC_RELAXED);
	__atomic_fetch_add(&mstat.import_skipped, !!skipped, __ATOMIC_RELAXED);
	stats_add(&mstat.import_ns, &mstat.import_max, ns);
	for (i = 0; i < TGM_PHASES; i++)
		__atomic_fetch_add(&mstat.import_phase[i], phase_ns[i], __ATOMIC_RELAXED);
}

void tgm_stats_bind(int map, uint64_t pages, uint64_t ns)
{
	if (!stats_on())
		return;
	if (map) {
		__atomic_fetch_add(&mstat.map_n, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&mstat.map_pages, pages, __ATOMIC_RELAXED);
		stats_add(&mstat.map_ns, &mstat.map_max, ns);
	} else {
		__atomic_fetch_add(&mstat.unmap_n, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&mstat.unmap_ns, ns, __ATOMIC_RELAXED);
	}
}

void tgm_stats_bo_free(uint64_t ns)
{
	if (!stats_on())
		return;
	__atomic_fetch_add(&mstat.free_n, 1, __ATOMIC_RELAXED);
	stats_add(&mstat.free_ns, &mstat.free_max, ns);
}

void tgm_stats_prefault(uint64_t bytes)
{
	if (!stats_on())
		return;
	__atomic_fetch_add(&mstat.prefault_n, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&mstat.prefault_bytes, bytes, __ATOMIC_RELAXED);
}

/* The window's "mem" line: BoAllocs (cached: mean/max and each phase's total; pool; how many were 64 KiB or less),
 * BoFrees, prefaulted views, and the system's page faults over the window (every vm_fault, and those that zero-filled a
 * page: XNU's own counters; with the kernel's user-fault timing, their mean and longest); and its "mem2" line: host
 * imports (mean/max, phase totals, read-only ones, cleans the registry skipped, re-imports of a surface released < 2 s
 * before) and VmBind maps (pages, mean/max) and unmaps. Each only when something happened. */
static void stats_mem(u64 ms)
{
	u64 v[sizeof(mstat) / sizeof(u64)];
	struct nvxnu_faults f;
	static struct nvxnu_faults last;
	static char line
		[800]; /* one caller at a time: the submission that won the window (stats_tick) */
	char user[96] = "";
	unsigned int k;
#define MS(field) v[offsetof(typeof(mstat), field) / sizeof(u64)]
#define US(x) ((x) / 1000)
#define MEAN_US(sum, n) ((n) ? (sum) / (n) / 1000 : 0)
	for (k = 0; k < ARRAY_SIZE(v); k++)
		v[k] = __atomic_exchange_n(&((u64 *)&mstat)[k], 0, __ATOMIC_RELAXED);
	nvxnu_vm_faults(&f);
	if (!last.faults)
		last = f;
	if (f.user_n > last.user_n)
		scnprintf(user, sizeof(user), "; user faults %llu mean %llu us max %llu us",
			  f.user_n - last.user_n,
			  (f.user_ns - last.user_ns) / (f.user_n - last.user_n) / 1000,
			  f.user_max_ns / 1000);
	if (MS(cached_n) || MS(pool_n) || MS(free_n) || MS(spare_n)) {
		uint64_t kept = 0, ready = 0, zeroing = 0;
		tgm_spare_counts(&kept, &ready, &zeroing);
		scnprintf(
			line, sizeof(line),
			"BoAlloc spare %llu (%llu KiB) mean %llu us max %llu us, cached %llu (%llu "
			"KiB) mean %llu us max %llu us (%s) [alloc+wire %llu, clean %llu, pages "
			"%llu, "
			"nvkm %llu us], pool %llu (%llu KiB) mean %llu us max %llu us, %llu of <= "
			"64 "
			"KiB; spares %llu KiB in %llu, pool %llu KiB zeroing; BoFree %llu "
			"mean %llu us max %llu us; %llu prefaulted maps (%llu KiB); faults "
			"%llu/s (%llu/s zero-fill)%s",
			MS(spare_n), MS(spare_bytes) >> 10, MEAN_US(MS(spare_ns), MS(spare_n)),
			US(MS(spare_max)), MS(cached_n), MS(cached_bytes) >> 10,
			MEAN_US(MS(cached_ns), MS(cached_n)), US(MS(cached_max)),
			MS(cached_n) ? mstat_max_owner : "-", US(MS(cached_phase[0])),
			US(MS(cached_phase[1])), US(MS(cached_phase[2])), US(MS(cached_phase[3])),
			MS(pool_n), MS(pool_bytes) >> 10, MEAN_US(MS(pool_ns), MS(pool_n)),
			US(MS(pool_max)), MS(small_n), (unsigned long long)(kept >> 10),
			(unsigned long long)ready, (unsigned long long)(zeroing >> 10), MS(free_n),
			MEAN_US(MS(free_ns), MS(free_n)), US(MS(free_max)), MS(prefault_n),
			MS(prefault_bytes) >> 10, (f.faults - last.faults) * 1000 / (ms ? ms : 1),
			(f.zero_fill - last.zero_fill) * 1000 / (ms ? ms : 1), user);
		nvxnu_stats("tegra-gm20b: mem %s", line);
	}
	if (MS(import_n) || MS(map_n) || MS(unmap_n)) {
		scnprintf(line, sizeof(line),
			  "BoImport %llu (%llu KiB, %llu read-only, %llu clean skipped, %llu "
			  "re-imports) mean "
			  "%llu us max %llu us [wire %llu, pages %llu, nvkm %llu, clean %llu us]; "
			  "VmBind %llu maps (%llu pages) mean "
			  "%llu us max %llu us, %llu unmaps %llu us",
			  MS(import_n), MS(import_bytes) >> 10, MS(import_ro), MS(import_skipped),
			  MS(reimports), MEAN_US(MS(import_ns), MS(import_n)), US(MS(import_max)),
			  US(MS(import_phase[0])), US(MS(import_phase[1])), US(MS(import_phase[2])),
			  US(MS(import_phase[3])), MS(map_n), MS(map_pages),
			  MEAN_US(MS(map_ns), MS(map_n)), US(MS(map_max)), MS(unmap_n),
			  US(MS(unmap_ns)));
		nvxnu_stats("tegra-gm20b: mem2 %s", line);
	}
	if (tgm_owner_stats(line, sizeof(line)))
		nvxnu_stats("tegra-gm20b: mem3 BoAllocs by process: %s", line);
	last = f;
#undef MS
#undef US
#undef MEAN_US
}

static volatile struct tgm_stamps *chan_stamps(struct tgm_chan *chan, u32 seqno)
{
	return (volatile struct tgm_stamps *)((u8 *)chan->ring + CHAN_STAMPS) +
	       seqno % CHAN_NSTAMPS;
}

static u64 chan_stamps_va(struct tgm_chan *chan, u32 seqno)
{
	return chan->ring_va + CHAN_STAMPS +
	       (u64)(seqno % CHAN_NSTAMPS) * sizeof(struct tgm_stamps);
}

/* Submit-to-fence latency (CPU clock), seen from the fence's nonstall interrupt (every channel's handler runs on each
 * one), and from the GPU's stamps (PTIMER): queue = the GPU started the submission - the kernel kicked it, exec = the
 * GPU finished it (L2 flush included) - started it. The present channel's stamps time its copy instead
 * (present_gap: and the gap before it). */
static u64 present_gap(u32 seqno, u64 start);

/* push profile (tegra-gpu-stats=1):
 * What each push of a submission costs the GPU, sampled: one submission in 16 of each connection (the user client's
 * choice, with each push's class from the method stream it read, struct tgm_prof) stamps the GPU's time after each of its
 * pushes, a 16-byte semaphore release after wait-for-idle (push_stamp, as the submission's own start and end stamps) into
 * its own slot of the channel's CHAN_PROF page, each kicked as an entry of its own after the push's. So each push's time
 * is the GPU's from the end of the push before (the start stamp for the first) until its own work has drained, and the
 * fence push (WFI, L2 flush) takes the rest up to the end stamp; a push not stamped (TGM_PC_SKIP: a method may continue
 * into the next push, or the method stream was not read) has its time counted with the next one stamped. The stamps' wait
 * for idle keeps pushes from overlapping in a profiled submission, so those run slightly longer than others: one in 16
 * per connection, no more than one in flight per channel, and none with more than TGM_PROF_MAX pushes. Each window, the
 * "gpuprof" line gives every process's time by class (stats_exec_owners). */
static void prof_collect(struct tgm_chan *chan, u32 seqno, u64 start, u64 end)
{
	volatile struct tgm_stamp *slot =
		(volatile struct tgm_stamp *)((u8 *)chan->ring + CHAN_PROF);
	u64 prev = start, t;
	u32 i, n = min_t(u32, READ_ONCE(chan->prof_count), TGM_PROF_MAX), draws = 0, sdraws = 0;

	for (i = 0; i < n; i++) {
		u8 c = chan->prof_cls[i];
		draws +=
			chan->prof_draws
				[i]; /* a push not stamped: its draws ran in the next stamped one's time */
		sdraws += chan->prof_sdraws[i];
		if (c == TGM_PC_SKIP)
			continue;
		if (c >= TGM_PC_COUNT || slot[i].payload != seqno)
			return; /* not this submission's stamp: drop the rest */
		t = slot[i].ns;
		if (t < prev)
			return;
		__atomic_fetch_add(&chan->prof_ns[c], t - prev, __ATOMIC_RELAXED);
		__atomic_fetch_add(&chan->prof_n[c], 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&chan->prof_dn[c], draws, __ATOMIC_RELAXED);
		__atomic_fetch_add(&chan->prof_sdn[c], sdraws, __ATOMIC_RELAXED);
		draws = sdraws = 0;
		prev = t;
	}
	if (end >= prev) {
		__atomic_fetch_add(&chan->prof_ns[TGM_PC_FENCE], end - prev, __ATOMIC_RELAXED);
		__atomic_fetch_add(&chan->prof_n[TGM_PC_FENCE], 1, __ATOMIC_RELAXED);
	}
	__atomic_fetch_add(&chan->prof_subs, 1, __ATOMIC_RELAXED);
}

static const char *prof_name(unsigned int c, char *buf, unsigned int len)
{
	static const char *const size[] = {"full", "mid", "small"};
	static const char *const other[] = {"pass", "sync",  "compute", "copy",
					    "2D",   "state", "fence"};
	if (c < TGM_PC_PASS)
		scnprintf(buf, len, "%s %s %s%s", c < TGM_PC_CLEAR ? "draw" : "clear",
			  size[(c % 12) / 4], c & 2 ? "pitch" : "block", c & 1 ? " MSAA" : "");
	else
		scnprintf(buf, len, "%s", other[c - TGM_PC_PASS]);
	return buf;
}

static void stats_fences(struct tgm_chan *chan)
{
	u32 done = tgm_chan_completed(chan), s = chan->stat_done, i;
	u64 now = nvxnu_nanotime(), start, end, gap;
	volatile struct tgm_stamps *st;

	if ((s32)(done - s) > CHAN_NSTAMPS)
		s = done - CHAN_NSTAMPS;
	for (; (s32)(done - s) > 0; s++) {
		i = (s + 1) % CHAN_NSTAMPS;
		if (!chan->kernel) {
			__atomic_fetch_add(&stats.fences, 1, __ATOMIC_RELAXED);
			stats_add(&stats.lat_sum, &stats.lat_max,
				  now - READ_ONCE(chan->stat_sub_ns[i]));
		}
		st = chan_stamps(chan, s + 1);
		if (!chan->kernel && s + 1 == READ_ONCE(chan->prof_seqno)) {
			if (st->start.payload == s + 1 && st->end.payload == s + 1)
				prof_collect(chan, s + 1, st->start.ns, st->end.ns);
			WRITE_ONCE(chan->prof_seqno, 0);
		}
		if (st->start.payload != s + 1 || st->end.payload != s + 1)
			continue; /* overwritten by a later submission, or not stamped */
		start = st->start.ns;
		end = st->end.ns;
		if (chan->kernel) {
			__atomic_fetch_add(&stats.copies, 1, __ATOMIC_RELAXED);
			stats_add(&stats.copy_sum, &stats.copy_max, end - start);
			if ((gap = present_gap(s + 1, start)) != ~0ULL) {
				__atomic_fetch_add(&stats.gaps, 1, __ATOMIC_RELAXED);
				stats_add(&stats.gap_sum, &stats.gap_max, gap);
			}
			continue;
		}
		__atomic_fetch_add(&stats.timed, 1, __ATOMIC_RELAXED);
		stats_add(&stats.queue_sum, &stats.queue_max,
			  start - READ_ONCE(chan->stat_sub_ptimer[i]));
		stats_add(&stats.exec_sum, &stats.exec_max, end - start);
		__atomic_fetch_add(&chan->stat_exec_n, 1, __ATOMIC_RELAXED);
		stats_add(&chan->stat_exec, &chan->stat_exec_max, end - start);
	}
	chan->stat_done = done;
}

static void present_stats(u32 *presents, u64 *sum, u64 *max);
static void scrub_stats(char *buf, unsigned int len, u64 window_ms);

/* The stats line's GPU clock, every window: the rate the GPCPLL is programmed for (nvkm_clk_read: its M/N/PL registers),
 * whether the GPU runs from it (SEL_VCO bit 0, GPC2CLK_OUT's bypass divider 0 = 1:1), and the rate GPC 0's clock
 * counter measured (nvgpu gm20b_clk_get_gpcclk_clock_counter: NCGPCCLK cycles counted over 800 cycles of the PLL's
 * 38.4 MHz reference, hw_trim_gm20b.h trim_gpc_clk_cntr_ncgpcclk_cfg/cnt). A count takes 21 us; each window reads the
 * count the previous window started and starts the next, so nothing waits for it. A measured rate below the programmed
 * one would show a slowdown or gating the PLL's registers do not: with it the line prints THERM's idle slowdown factor
 * (CLK_SLOWDOWN_0 bits 21:16, 0 = off; gm20b_clk_init clears it) and GR's engine clock gating mode (THERM_GATE_CTRL(0)
 * bits 1:0: 0 run, 1 auto, 2 stop). Called by one submission at a time (the window's winner, stats_tick). */
#define TRIM_GPC0_NCGPCCLK_CFG 0x134124
#define TRIM_GPC0_NCGPCCLK_CNT 0x134128
#define NCGPCCLK_CFG_RESET 0x01000000
#define NCGPCCLK_CFG_ENABLE 0x00100000
#define NCGPCCLK_CFG_WRITE_EN 0x00010000
#define NCGPCCLK_CYCLES 800
static void clk_measure(char *buf, unsigned int len)
{
	static int started;
	struct nvkm_device *device = gpu.device;
	u32 cnt = nvkm_rd32(device, TRIM_GPC0_NCGPCCLK_CNT);
	u32 ref_khz = gk20a_clk(device->clk)->parent_rate / 1000;
	u32 vco = nvkm_rd32(device, 0x137100), out = nvkm_rd32(device, 0x137250);
	u32 slow = nvkm_rd32(device, 0x020160), gate = nvkm_rd32(device, 0x020200);
	int pll = nvkm_clk_read(device->clk, nv_clk_src_gpc);
	char meas[40] = "-";

	if (started && !badf(cnt))
		scnprintf(meas, sizeof(meas), "%llu kHz",
			  (unsigned long long)ref_khz * (cnt & 0xfffff) / NCGPCCLK_CYCLES);
	else if (started)
		scnprintf(meas, sizeof(meas), "unread (%08x)", cnt);
	nvkm_wr32(device, TRIM_GPC0_NCGPCCLK_CFG, NCGPCCLK_CFG_RESET);
	nvkm_wr32(device, TRIM_GPC0_NCGPCCLK_CFG,
		  NCGPCCLK_CFG_ENABLE | NCGPCCLK_CFG_WRITE_EN | NCGPCCLK_CYCLES);
	started = 1;
	scnprintf(buf, len, "GPU clock %d kHz%s, measured %s (slowdown %u, ELCG %u)", pll,
		  !(vco & 1)     ? " BYPASSED"
		  : out & 0x3f00 ? " DIVIDED"
				 : "",
		  meas, (slow >> 16) & 0x3f, gate & 3);
}

/* The window's "exec" line: each process's stamped GPU time per submission (start to end stamp, the L2 flush included:
 * what its work held the GPU for, other channels' time slices inside it included), summed over its channels and taken from
 * them; the window's first EXEC_OWNERS - 1 processes by name, the rest as "(others)". `rate` converts PTIMER units to ns
 * (16.16, stats_tick). Called from the submission that won the window, never with live_lock held. */
#define EXEC_OWNERS 5
static void stats_exec_owners(char *line, unsigned int len, u64 rate)
{
	/* static: one caller at a time (the window's winner, stats_tick) */
	static struct {
		char name[17];
		u64 sum, max;
		u32 n, subs;
		u64 prof_ns[TGM_PC_COUNT];
		u32 prof_n[TGM_PC_COUNT];
		u64 prof_dn[TGM_PC_COUNT], prof_sdn[TGM_PC_COUNT];
	} o[EXEC_OWNERS];
	struct tgm_chan *chan;
	unsigned int used = 0, i, c, at = 0;
	char name[32];

	memset(o, 0, sizeof(o));
	mutex_lock(&live_lock);
	list_for_each_entry(chan, &live_chans, live)
	{
		u32 n = __atomic_exchange_n(&chan->stat_exec_n, 0, __ATOMIC_RELAXED);
		u64 sum = __atomic_exchange_n(&chan->stat_exec, 0, __ATOMIC_RELAXED);
		u64 max = __atomic_exchange_n(&chan->stat_exec_max, 0, __ATOMIC_RELAXED);
		u32 subs = __atomic_exchange_n(&chan->prof_subs, 0, __ATOMIC_RELAXED);
		if ((!n && !subs) || chan->kernel)
			continue;
		for (i = 0; i < used && strncmp(o[i].name, chan->owner, sizeof(o[i].name)); i++)
			;
		if (i == used && used < EXEC_OWNERS - 1)
			strlcpy(o[used++].name, chan->owner, sizeof(o[0].name));
		else if (i == used) {
			i = EXEC_OWNERS - 1;
			strlcpy(o[i].name, "(others)", sizeof(o[0].name));
		}
		o[i].n += n;
		o[i].sum += sum;
		if (max > o[i].max)
			o[i].max = max;
		o[i].subs += subs;
		for (c = 0; c < TGM_PC_COUNT; c++) {
			o[i].prof_ns[c] +=
				__atomic_exchange_n(&chan->prof_ns[c], 0, __ATOMIC_RELAXED);
			o[i].prof_n[c] +=
				__atomic_exchange_n(&chan->prof_n[c], 0, __ATOMIC_RELAXED);
			o[i].prof_dn[c] +=
				__atomic_exchange_n(&chan->prof_dn[c], 0, __ATOMIC_RELAXED);
			o[i].prof_sdn[c] +=
				__atomic_exchange_n(&chan->prof_sdn[c], 0, __ATOMIC_RELAXED);
		}
	}
	mutex_unlock(&live_lock);
	line[0] = 0;
	for (i = 0; i < EXEC_OWNERS; i++) {
		if (!o[i].n)
			continue;
		at += scnprintf(line + at, len - at, "%s%s %u x mean %llu us max %llu us",
				at ? "; " : "", o[i].name, o[i].n,
				((o[i].sum / o[i].n) * rate >> 16) / 1000,
				(o[i].max * rate >> 16) / 1000);
	}
	if (at)
		nvxnu_stats("tegra-gm20b: exec by process: %s", line);
	/* each process's profiled submissions: their GPU time by push class, the largest first, per submission */
	for (i = 0; i < EXEC_OWNERS; i++) {
		u64 total = 0, left;
		u32 shown = 0;
		if (!o[i].subs)
			continue;
		for (c = 0; c < TGM_PC_COUNT; c++)
			total += o[i].prof_ns[c];
		at = scnprintf(line, len,
			       "%s: %u profiled submits, %llu us of GPU each:", o[i].name,
			       o[i].subs, ((total / o[i].subs) * rate >> 16) / 1000);
		left = total;
		while (shown < 10 && left) {
			unsigned int best = 0;
			for (c = 1; c < TGM_PC_COUNT; c++)
				if (o[i].prof_ns[c] > o[i].prof_ns[best])
					best = c;
			if (!o[i].prof_ns[best])
				break;
			at += scnprintf(
				line + at, len - at, "%s %s %u x %llu us (%llu us/submit",
				shown ? ";" : "", prof_name(best, name, sizeof(name)),
				o[i].prof_n[best],
				((o[i].prof_ns[best] / max_t(u32, o[i].prof_n[best], 1)) * rate >>
				 16) / 1000,
				((o[i].prof_ns[best] / o[i].subs) * rate >> 16) / 1000);
			if (o[i].prof_dn[best])
				at += scnprintf(
					line + at, len - at, ", %llu draws/push, %llu per-sample",
					o[i].prof_dn[best] / max_t(u32, o[i].prof_n[best], 1),
					o[i].prof_sdn[best] / max_t(u32, o[i].prof_n[best], 1));
			at += scnprintf(line + at, len - at, ")");
			left -= o[i].prof_ns[best];
			o[i].prof_ns[best] = 0;
			shown++;
		}
		nvxnu_stats("tegra-gm20b: gpuprof %s", line);
	}
}

/* Run by each submission: the first one after a window's end logs it. */
static void stats_tick(void)
{
	u64 now = nvxnu_nanotime(), since = __atomic_load_n(&stats.since, __ATOMIC_RELAXED), ms,
	    sum, max, qs, qm, es, em, ps, pm;
	u32 submits, fences, imports, timed, presents, ordered, idle, late, writer, allocs, mc_int,
		mc_err, mc_adr, copies, gaps;
	u64 abytes, ans, pt, cal, rate, cs, cm, gs, gm, l2s, l2m, busy;
	u32 l2n;
	char line[896], clk[120], scrubbed[120];

	__atomic_fetch_add(&stats.submits, 1, __ATOMIC_RELAXED);
	if (!since) {
		if (__atomic_compare_exchange_n(&stats.since, &since, now, false, __ATOMIC_RELAXED,
						__ATOMIC_RELAXED))
			stats.cal_ptimer = nvkm_timer_read(gpu.device->timer);
		return;
	}
	if (now - since < STATS_WINDOW_NS ||
	    !__atomic_compare_exchange_n(&stats.since, &since, now, false, __ATOMIC_RELAXED,
					 __ATOMIC_RELAXED))
		return;
	ms = (now - since) / 1000000;
	/* nanoseconds per PTIMER unit over this window, 16.16 fixed point; the window's stamps were taken within it */
	pt = nvkm_timer_read(gpu.device->timer);
	cal = stats.cal_ptimer;
	stats.cal_ptimer = pt;
	rate = cal && pt > cal ? ((now - since) << 16) / (pt - cal) : 1 << 16;
#define PT_NS(v) (((v) * rate) >> 16)
	submits = __atomic_exchange_n(&stats.submits, 0, __ATOMIC_RELAXED);
	fences = __atomic_exchange_n(&stats.fences, 0, __ATOMIC_RELAXED);
	imports = __atomic_exchange_n(&stats.imports, 0, __ATOMIC_RELAXED);
	sum = __atomic_exchange_n(&stats.lat_sum, 0, __ATOMIC_RELAXED);
	max = __atomic_exchange_n(&stats.lat_max, 0, __ATOMIC_RELAXED);
	timed = __atomic_exchange_n(&stats.timed, 0, __ATOMIC_RELAXED);
	present_stats(&presents, &ps, &pm);
	ordered = __atomic_exchange_n(&stats.ordered, 0, __ATOMIC_RELAXED);
	idle = __atomic_exchange_n(&stats.idle, 0, __ATOMIC_RELAXED);
	late = __atomic_exchange_n(&stats.late, 0, __ATOMIC_RELAXED);
	writer = __atomic_exchange_n(&stats.writer, 0, __ATOMIC_RELAXED);
	allocs = __atomic_exchange_n(&stats.allocs, 0, __ATOMIC_RELAXED);
	abytes = __atomic_exchange_n(&stats.alloc_bytes, 0, __ATOMIC_RELAXED);
	ans = __atomic_exchange_n(&stats.alloc_ns, 0, __ATOMIC_RELAXED);
	qs = __atomic_exchange_n(&stats.queue_sum, 0, __ATOMIC_RELAXED);
	qm = __atomic_exchange_n(&stats.queue_max, 0, __ATOMIC_RELAXED);
	es = __atomic_exchange_n(&stats.exec_sum, 0, __ATOMIC_RELAXED);
	em = __atomic_exchange_n(&stats.exec_max, 0, __ATOMIC_RELAXED);
	copies = __atomic_exchange_n(&stats.copies, 0, __ATOMIC_RELAXED);
	cs = __atomic_exchange_n(&stats.copy_sum, 0, __ATOMIC_RELAXED);
	cm = __atomic_exchange_n(&stats.copy_max, 0, __ATOMIC_RELAXED);
	gaps = __atomic_exchange_n(&stats.gaps, 0, __ATOMIC_RELAXED);
	gs = __atomic_exchange_n(&stats.gap_sum, 0, __ATOMIC_RELAXED);
	gm = __atomic_exchange_n(&stats.gap_max, 0, __ATOMIC_RELAXED);
	mutex_lock(&l2_lock);
	l2s = l2_time.sum;
	l2m = l2_time.max;
	l2n = l2_time.n;
	l2_time.sum = l2_time.max = l2_time.n = 0;
	mutex_unlock(&l2_lock);
	tegra_gm20b_mc_errors(&mc_int, &mc_err, &mc_adr);
	/* the GPU's share of the window spent on stamped user submissions and copies */
	busy = PT_NS(es + cs) * 100 / (now - since);
	clk_measure(clk, sizeof(clk));
	scrub_stats(scrubbed, sizeof(scrubbed), ms);
	/* formatted here and logged as one string: os_log drops a message whose scalar arguments outgrow its 256-byte buffer
	 * (libkern/os/log.h OS_LOG_BUFFER_MAX_SIZE), which a string argument does not count against */
	scnprintf(line, sizeof(line),
		  "%llu ms: %u submits (%llu/s), fence latency mean %llu us max %llu us (%u), GPU "
		  "queue "
		  "mean %llu us max %llu us, exec mean %llu us max %llu us (%u stamped), busy "
		  "%llu%% (PTIMER x%llu.%03llu), "
		  "L2 %s (invalidate mean %llu us max %llu us), %u presents (%u writer-only), "
		  "offer to done mean %llu us max "
		  "%llu us, %s",
		  ms, submits, submits * 1000ULL / (ms ? ms : 1), fences ? sum / fences / 1000 : 0,
		  max / 1000, fences, timed ? PT_NS(qs) / timed / 1000 : 0, PT_NS(qm) / 1000,
		  timed ? PT_NS(es) / timed / 1000 : 0, PT_NS(em) / 1000, timed, busy, rate >> 16,
		  ((rate & 0xffff) * 1000) >> 16, l2_on() ? "cached" : "volatile",
		  l2n ? l2s / l2n / 1000 : 0, l2m / 1000, presents, writer,
		  presents ? ps / presents / 1000 : 0, pm / 1000, clk);
	nvxnu_stats("tegra-gm20b: stats %s", line);
	/* the pool by holder, so no share of it goes unnamed */
	uint64_t pool_clients, pool_orphan_bytes, pool_orphans, pool_system_bytes, pool_systems;
	tgm_pool_breakdown(&pool_clients, &pool_orphan_bytes, &pool_orphans, &pool_system_bytes,
			   &pool_systems);
	scnprintf(
		line, sizeof(line),
		"copy mean %llu us max %llu us (%u), after user work mean %llu us max %llu us "
		"(%u); "
		"order %s: %u submits behind a copy, %u offers idle (%u then a submit); %u imports "
		"(%llu/s), %lld KiB live in "
		"%d, %u BoAllocs (%llu KiB, %llu us), pool %lu/%llu MiB (compositor %llu, clients "
		"%llu, orphans %llu in "
		"%llu; coherent from system memory %llu MiB in %llu), %s, MC "
		"%08x "
		"err "
		"%08x at %08x",
		copies ? PT_NS(cs) / copies / 1000 : 0, PT_NS(cm) / 1000, copies,
		gaps ? PT_NS(gs) / gaps / 1000 : 0, PT_NS(gm) / 1000, gaps,
		present_order_on() ? "on" : "off", ordered, idle, late, imports,
		imports * 1000ULL / (ms ? ms : 1),
		(long long)(__atomic_load_n(&stats.live_bytes, __ATOMIC_RELAXED) >> 10),
		__atomic_load_n(&stats.live, __ATOMIC_RELAXED), allocs, abytes >> 10, ans / 1000,
		nvxnu_pool_used_pages() >> (20 - PAGE_SHIFT), nvxnu_pool_size() >> 20,
		__atomic_load_n(&compositor_pool, __ATOMIC_RELAXED) >> 20, pool_clients >> 20,
		pool_orphan_bytes >> 20, pool_orphans, pool_system_bytes >> 20, pool_systems,
		scrubbed, mc_int, mc_err, mc_adr);
	nvxnu_stats("tegra-gm20b: stats2 %s", line);
	stats_exec_owners(line, sizeof(line), rate);
	stats_mem(ms);
	/* The lazy IOSurface-coherency hooks' work this window (the XNU lock/unlock hook; tgm_coherency_*): GPU-written imports
	 * invalidated for a CPU read lock, and CPU-written imports cleaned for the GPU at a write unlock. Zero on the render
	 * path, so these count only surfaces the CPU actually locks. */
	{
		u64 in, ib, cn, cb;
		mutex_lock(&coh_lock);
		in = coh_stats.inval_n;
		ib = coh_stats.inval_bytes;
		cn = coh_stats.clean_n;
		cb = coh_stats.clean_bytes;
		coh_stats = (typeof(coh_stats)){0};
		mutex_unlock(&coh_lock);
		if (in || cn)
			nvxnu_log("tegra-gm20b: coherency %llu read-lock invalidations (%llu KiB), "
				  "%llu write-unlock cleans (%llu KiB)\n",
				  in, ib >> 10, cn, cb >> 10);
	}
#undef PT_NS
}

/* idle clock: the GPCPLL's output divider, so the GPU downclocks only where that costs no performance.
 * The GPU runs at tegra-gpu-mhz's rate (768 MHz, Horizon's docked maximum) whenever there is any GPU work, and drops
 * to 76.8 MHz (L4T's lowest B01 row, Horizon's own boost-mode GPU clock) only once it has been truly idle:
 *   * down: no submission kicked, nothing in flight and no display swap for IDLE_DROP_NS (500 ms), checked on the
 *     CPU governor's tick (SwitchPerfGovernor.cpp calls tegra_gm20b_idle_tick from XNU's perfcontrol deadline, so
 *     the GPU adds no timer of its own; without the CPU governor the GPU simply never drops). A swap arriving means
 *     Core Animation is animating, so the clock never drops during an animation, even one that submits little.
 *   * up: synchronously, before the submission's work is kicked (tgm_chan_exec, tgm_present_copy, and chan_fence as
 *     a backstop), so no GPU work ever starts at the idle clock; also at once (a work item) on switch_perf_hint's
 *     touch, missed-frame and launch hints, so the clock is usually back before the frame's first submission.
 * The change is the output divider alone, VCO left running and locked (1536 MHz: M 1, N 40 from the 38.4 MHz
 * reference), PL 1 <-> 10. GM20B's GPCPLL changes PL without a glitch (nvgpu clk_gm20b.c: PLDIV_GLITCHLESS; nouveau
 * gm20b.c gm20b_pllg_program_mnp's pdiv_only path, whose sequence this is: VCO divider 1:2 around it, an interim PL
 * sharing a bit with both ends). No NDIV slide, no relock, no bypass, no VDD_GPU write (the rail stays at clk_raise's
 * 800 mV): a few register writes and two 2 us delays, timed on every raise and logged (raise mean/max).
 * Before first use, gov_start runs one drop and raise at stage 3 (no client yet) and logs both times. Any failure
 * (the divider not reading back, the PLL not locked, the clock not at the rate) logs one line and leaves the GPU at
 * 768 MHz for the boot. */
#define IDLE_DROP_NS (500ULL * 1000000ULL)
#define IDLE_LOG_NS (10ULL * 1000000000ULL)
#define GPCPLL_CFG_R 0x137000   /* 0 enable, 17 lock */
#define GPCPLL_COEFF_R 0x137004 /* M 7:0, N 15:8, PL 21:16 */
#define GPC2CLK_OUT_R 0x137250  /* VCO divider 13:8: 0 = 1:1, 2 = 1:2 */
#define SEL_VCO_R 0x137100      /* 0 = GPU on bypass, 1 = on GPCPLL */
#define PLL_REF_KHZ 38400

extern uint64_t
native_display_last_swap_ns(void); /* NativeLegacyDisplay.cpp: validateSwap's last call, 0: none */

static struct {
	int on;       /* the idle clock is in use (atomic) */
	int low;      /* the GPCPLL runs the idle divider (atomic; changed under clk) */
	int dropping; /* a drop is queued or running (atomic) */
	void *clk;    /* mutex: every divider change */
	void *drop_w, *raise_w;
	u32 run_pl, idle_pl, run_khz, idle_khz;
	u64 last_busy; /* atomic: the last kick or hint (ns) */
	u64 kicked,
		retired; /* atomic: submissions kicked / seen complete, every channel, since boot */
	/* the periodic line (under clk) */
	u64 log_start, low_since, low_ns, raise_sum, raise_max;
	u32 drops, raises, hinted;
} idle;

static u64 idle_in_flight(void)
{
	return __atomic_load_n(&idle.kicked, __ATOMIC_SEQ_CST) -
	       __atomic_load_n(&idle.retired, __ATOMIC_SEQ_CST);
}

/* GM20B's glitchless PL change (nouveau gm20b.c gm20b_pllg_program_mnp, pdiv_only): returns 0 when the GPCPLL is
 * locked, on the VCO, at PL `pl`. Under idle.clk. */
static int pl_switch(u32 pl)
{
	struct nvkm_device *device = gpu.device;
	u32 coeff = nvkm_rd32(device, GPCPLL_COEFF_R), old = (coeff >> 16) & 0x3f, back;

	if (old != pl) {
		/* the VCO divider at 1:2 around the change, written twice (linear divider operation) */
		nvkm_mask(device, GPC2CLK_OUT_R, 0x3f00, 2 << 8);
		nvkm_mask(device, GPC2CLK_OUT_R, 0x3f00, 2 << 8);
		nvkm_rd32(device, GPC2CLK_OUT_R);
		udelay(2);
		if (!(old & pl)) { /* an interim PL that shares a set bit with both ends */
			u32 a = old | (1u << (__builtin_ffs((int)pl) - 1)),
			    b = pl | (1u << (__builtin_ffs((int)old) - 1));
			nvkm_wr32(device, GPCPLL_COEFF_R,
				  (coeff & ~(0x3fu << 16)) | ((a < b ? a : b) << 16));
		}
		nvkm_wr32(device, GPCPLL_COEFF_R, (coeff & ~(0x3fu << 16)) | (pl << 16));
		udelay(2);
		nvkm_mask(device, GPC2CLK_OUT_R, 0x3f00, 0);
		nvkm_mask(device, GPC2CLK_OUT_R, 0x3f00, 0);
		nvkm_rd32(device, GPC2CLK_OUT_R);
	}
	back = nvkm_rd32(device, GPCPLL_COEFF_R);
	return ((back >> 16) & 0x3f) == pl &&
			       (nvkm_rd32(device, GPCPLL_CFG_R) & 0x20001) == 0x20001 &&
			       (nvkm_rd32(device, SEL_VCO_R) & 1) &&
			       !(nvkm_rd32(device, GPC2CLK_OUT_R) & 0x3f3f)
		       ? 0
		       : -EIO;
}

/* Under idle.clk: a divider change failed. One line, then 768 MHz for the boot (the divider, else nouveau's full
 * reprogram of the rate), the idle clock off. */
static void idle_fail(const char *what, u32 pl)
{
	struct nvkm_device *device = gpu.device;
	int back;

	WRITE_ONCE(idle.on, 0);
	back = pl_switch(idle.run_pl);
	if (back)
		back = clk_prog_khz(device, idle.run_khz);
	WRITE_ONCE(idle.low, 0);
	nvxnu_log("tegra-gm20b: idle clock off: %s to PL %u did not take (GPCPLL_CFG %08x COEFF "
		  "%08x SEL_VCO %x "
		  "GPC2CLK_OUT %08x); %d kHz now (%d), for this boot\n",
		  what, pl, nvkm_rd32(device, GPCPLL_CFG_R), nvkm_rd32(device, GPCPLL_COEFF_R),
		  nvkm_rd32(device, SEL_VCO_R), nvkm_rd32(device, GPC2CLK_OUT_R),
		  nvkm_clk_read(device->clk, nv_clk_src_gpc), back);
}

/* Up to the run rate if the idle divider is in force; returns the ns it took (0: nothing to do). Thread context. */
static u64 idle_raise(void)
{
	u64 t0, dt = 0;

	nvxnu_mutex_lock(idle.clk);
	if (READ_ONCE(idle.low)) {
		t0 = nvxnu_nanotime();
		if (pl_switch(idle.run_pl)) {
			idle_fail("raise", idle.run_pl);
		} else {
			dt = nvxnu_nanotime() - t0;
			WRITE_ONCE(idle.low, 0);
			idle.raises++;
			idle.raise_sum += dt;
			if (dt > idle.raise_max)
				idle.raise_max = dt;
			idle.low_ns += t0 - idle.low_since;
		}
	}
	nvxnu_mutex_unlock(idle.clk);
	return dt;
}

/* Before a submission's work is kicked (the submitter's thread): busy now, and the run rate first. The order (busy
 * stamp, then the drop flags) pairs with gov_drop's (flag, then the stamp): one of the two always sees the other. */
static void idle_wake(void)
{
	if (!READ_ONCE(idle.on))
		return;
	__atomic_store_n(&idle.last_busy, nvxnu_nanotime(), __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&idle.low, __ATOMIC_SEQ_CST) ||
	    __atomic_load_n(&idle.dropping, __ATOMIC_SEQ_CST))
		(void)idle_raise();
}

static void idle_drop(void *arg)
{
	u64 now;

	(void)arg;
	nvxnu_mutex_lock(idle.clk);
	now = nvxnu_nanotime();
	if (READ_ONCE(idle.on) && !READ_ONCE(idle.low) && !idle_in_flight() &&
	    now - __atomic_load_n(&idle.last_busy, __ATOMIC_SEQ_CST) >= IDLE_DROP_NS &&
	    now - native_display_last_swap_ns() >= IDLE_DROP_NS) {
		if (pl_switch(idle.idle_pl)) {
			idle_fail("drop", idle.idle_pl);
		} else {
			WRITE_ONCE(idle.low, 1);
			idle.drops++;
			idle.low_since = nvxnu_nanotime();
		}
	}
	__atomic_store_n(&idle.dropping, 0, __ATOMIC_SEQ_CST);
	nvxnu_mutex_unlock(idle.clk);
}

static void idle_hint_raise(void *arg)
{
	(void)arg;
	if (idle_raise()) {
		nvxnu_mutex_lock(idle.clk);
		idle.hinted++;
		nvxnu_mutex_unlock(idle.clk);
	}
}

/* drivers/SwitchPower SwitchPerfGovernor.cpp, on the CPU governor's tick (XNU's perfcontrol deadline: interrupts
 * masked, scheduler context): a few loads, and a work item queued once the GPU has been idle long enough. */
void tegra_gm20b_idle_tick(void)
{
	u64 now;
	int expected = 0;

	if (!READ_ONCE(idle.on) || READ_ONCE(idle.low) || READ_ONCE(idle.dropping) ||
	    idle_in_flight())
		return;
	now = nvxnu_nanotime();
	if (now - __atomic_load_n(&idle.last_busy, __ATOMIC_SEQ_CST) < IDLE_DROP_NS ||
	    now - native_display_last_swap_ns() < IDLE_DROP_NS)
		return;
	if (__atomic_compare_exchange_n(&idle.dropping, &expected, 1, false, __ATOMIC_SEQ_CST,
					__ATOMIC_SEQ_CST))
		nvxnu_work_schedule(idle.drop_w);
}

/* drivers/SwitchPower switch_perf_hint (touch, missed frame, launch): busy now, and back up at once if low. */
void tegra_gm20b_perf_hint(unsigned int hint)
{
	if (!READ_ONCE(idle.on) || hint < 1 || hint > 3)
		return;
	__atomic_store_n(&idle.last_busy, nvxnu_nanotime(), __ATOMIC_SEQ_CST);
	if (READ_ONCE(idle.low))
		nvxnu_work_schedule(idle.raise_w);
}

/* chan_fence, after the kick; the nonstall pass (gov_fenced) and channel teardown retire. Counted from boot. */
static void idle_kicked(void)
{
	__atomic_fetch_add(&idle.kicked, 1, __ATOMIC_SEQ_CST);
}

static void idle_fenced(struct tgm_chan *chan)
{
	u32 done = tgm_chan_completed(chan), n = done - chan->gov_done;

	if ((s32)n <= 0)
		return;
	chan->gov_done = done;
	__atomic_fetch_add(&idle.retired, n, __ATOMIC_SEQ_CST);
}

/* The periodic line (log only), from a kick at most every IDLE_LOG_NS. */
static void idle_stats(void)
{
	u64 now, span, low, sum, max;
	u32 drops, raises, hinted;

	if (!READ_ONCE(idle.on) || nvxnu_nanotime() - READ_ONCE(idle.log_start) < IDLE_LOG_NS ||
	    !nvxnu_mutex_trylock(idle.clk))
		return;
	now = nvxnu_nanotime();
	if (now - idle.log_start < IDLE_LOG_NS) {
		nvxnu_mutex_unlock(idle.clk);
		return;
	}
	span = now - idle.log_start;
	low = idle.low_ns + (idle.low ? now - idle.low_since : 0);
	if (idle.low)
		idle.low_since = now;
	sum = idle.raise_sum;
	max = idle.raise_max;
	drops = idle.drops;
	raises = idle.raises;
	hinted = idle.hinted;
	idle.low_ns = idle.raise_sum = idle.raise_max = 0;
	idle.drops = idle.raises = idle.hinted = 0;
	idle.log_start = now;
	nvxnu_mutex_unlock(idle.clk);
	nvxnu_stats("tegra-gm20b: idle clock %llu ms: %llu.%llu%% at %u kHz, %u drops, %u raises "
		    "(%u by a hint), "
		    "raise mean %llu ns max %llu ns; %llu in flight; now %d kHz",
		    span / 1000000, low * 100 / span, low * 1000 / span % 10, idle.idle_khz, drops,
		    raises, hinted, raises ? sum / raises : 0, max, idle_in_flight(),
		    nvkm_clk_read(gpu.device->clk, nv_clk_src_gpc));
}

/* GPC 0's clock counter, as clk_measure reads it (NCGPCCLK cycles over 800 cycles of the 38.4 MHz reference, about
 * 21 us): started, waited for, read. In kHz of what it counts (about twice gpcclk if it counts gpc2clk). 0 if it could
 * not be read. idle_start only, before any client. */
static u32 gpc_counted_khz(void)
{
	struct nvkm_device *device = gpu.device;
	u32 cnt;

	nvkm_wr32(device, TRIM_GPC0_NCGPCCLK_CFG, NCGPCCLK_CFG_RESET);
	nvkm_wr32(device, TRIM_GPC0_NCGPCCLK_CFG,
		  NCGPCCLK_CFG_ENABLE | NCGPCCLK_CFG_WRITE_EN | NCGPCCLK_CYCLES);
	udelay(30);
	cnt = nvkm_rd32(device, TRIM_GPC0_NCGPCCLK_CNT);
	return badf(cnt) ? 0
			 : (u32)((u64)(gk20a_clk(device->clk)->parent_rate / 1000) *
				 (cnt & 0xfffff) / NCGPCCLK_CYCLES);
}

/* After stage 3 (no client yet), at the rate clk_raise left: the divider pair, then one drop and raise as a test, each
 * timed and each rate read back from GPC 0's clock counter. */
static void idle_start(void)
{
	struct nvkm_device *device = gpu.device;
	u32 coeff = nvkm_rd32(device, GPCPLL_COEFF_R), m = coeff & 0xff, n = (coeff >> 8) & 0xff,
	    pl = (coeff >> 16) & 0x3f, vco, k, best = 0, i;
	int run = nvkm_clk_read(device->clk, nv_clk_src_gpc);
	u64 t0, down, up;
	u32 at_idle, at_run;

	if (idle.clk || run <= BOOT_KHZ)
		return; /* tegra-gpu-mhz did not raise it: it already runs the lowest rate */
	vco = m ? PLL_REF_KHZ * n / m : 0;
	/* the lowest L4T B01 rate this VCO reaches by its output divider alone (gpcclk = VCO / 2 / PL) */
	for (i = 0; (k = tegra_gm20b_dvfs_khz(i)); i++)
		if (k < (u32)run && vco && !(vco % (2 * k)) && vco / (2 * k) <= 31 &&
		    (!best || k < best))
			best = k;
	if (!m || !pl || vco / (2 * pl) != (u32)run || !best) {
		nvxnu_log("tegra-gm20b: idle clock not used: GPCPLL M %u N %u PL %u (%u kHz VCO) "
			  "runs %d kHz, "
			  "no L4T rate below it by the divider alone; the GPU stays at %d kHz\n",
			  m, n, pl, vco, run, run);
		return;
	}
	idle.run_pl = pl;
	idle.run_khz = (u32)run;
	idle.idle_pl = vco / (2 * best);
	idle.idle_khz = best;
	idle.clk = nvxnu_mutex_alloc();
	idle.drop_w = nvxnu_work_alloc(idle_drop, NULL);
	idle.raise_w = nvxnu_work_alloc(idle_hint_raise, NULL);
	if (!idle.clk || !idle.drop_w || !idle.raise_w) {
		nvxnu_log("tegra-gm20b: idle clock not used: no memory; the GPU stays at %d kHz\n",
			  run);
		return;
	}
	nvxnu_mutex_lock(idle.clk);
	t0 = nvxnu_nanotime();
	if (pl_switch(idle.idle_pl) || nvkm_clk_read(device->clk, nv_clk_src_gpc) != (int)best) {
		idle_fail("test drop", idle.idle_pl);
		nvxnu_mutex_unlock(idle.clk);
		return;
	}
	down = nvxnu_nanotime() - t0;
	at_idle = gpc_counted_khz();
	t0 = nvxnu_nanotime();
	if (pl_switch(idle.run_pl) || nvkm_clk_read(device->clk, nv_clk_src_gpc) != run) {
		idle_fail("test raise", idle.run_pl);
		nvxnu_mutex_unlock(idle.clk);
		return;
	}
	up = nvxnu_nanotime() - t0;
	at_run = gpc_counted_khz();
	idle.log_start = nvxnu_nanotime();
	__atomic_store_n(&idle.last_busy, idle.log_start, __ATOMIC_SEQ_CST);
	WRITE_ONCE(idle.on, 1);
	nvxnu_mutex_unlock(idle.clk);
	nvxnu_log("tegra-gm20b: idle clock: %d kHz while busy, %u kHz after %llu ms with no "
		  "submission and no "
		  "swap (GPCPLL M %u N %u: PL %u <-> %u, VCO %u kHz locked throughout, VDD_GPU "
		  "unchanged); "
		  "test drop %llu ns (GPC counter %u kHz), raise %llu ns (GPC counter %u kHz)\n",
		  run, best, IDLE_DROP_NS / 1000000, m, n, pl, idle.idle_pl, vco, down, at_idle, up,
		  at_run);
}

/* nouveau_fence_wait_uevent_handler(): a fence's NON_STALL_INTERRUPT arrived; wake the fence waiters. */
static void present_fenced(struct tgm_chan *chan);
static void scrub_fenced(struct tgm_chan *chan);

static int chan_nonstall(struct nvif_event *event, void *repv, u32 repc)
{
	struct tgm_chan *chan = container_of(event, struct tgm_chan, nonstall);
	(void)repv;
	(void)repc;
	idle_fenced(chan);
	if (chan->kernel == CHAN_SCRUB) {
		scrub_fenced(chan);
		/* nobody sleeps on its fence but system sleep's drain and the wake's test fence */
		if (tgm_gate_is_shut(&sc7_gate))
			nvxnu_wake();
		return NVIF_EVENT_KEEP;
	}
	if (stats_on())
		stats_fences(chan);
	if (chan->kernel)
		present_fenced(chan);
	nvxnu_wake();
	return NVIF_EVENT_KEEP;
}

static void chan_free(struct tgm_chan *chan)
{
	int i;
	nvif_event_dtor(&chan->nonstall);
	nvif_event_dtor(&chan->kill);
	for (i = chan->nobj - 1; i >= 0; i--)
		nvif_object_dtor(&chan->obj[i]);
	nvif_object_unmap(&chan->user);
	nvif_object_dtor(&chan->user);
	/* the channel is gone: unmap its ring (TLB invalidated) before the ring's pages go back to the pool */
	nvif_vmm_put(&chan->vm->vmm, &chan->ring_vma);
	if (chan->ring_mem) {
		struct tgm_mem *m = chan->ring_mem;
		nvif_mem_dtor(&m->mem);
		kfree(m);
	}
	if (chan->ring)
		dma_free_attrs(&gpu_pdev.dev, CHAN_BYTES, chan->ring, chan->ring_pa, 0);
	kfree(chan->ring_pas);
	kfree(chan);
}

/* Compositor priority (tegra-gpu-all-clients=1): nvkm's runlist has no priorities ("TODO: prio, interleaving",
 * fifo/nv50.c nv50_runl_update) and gives every channel RAMFC's default timeslice, 128 << 3 (fifo/gk104.c
 * gk104_chan_ramfc_write, RAMFC 0xf8 = PFIFO 0x2310: enable 28, scale 15:12, timeout 7:0), as nvgpu's default
 * (gk20a/fifo_gk20a.c gk20a_fifo_default_timeslice_us). nvgpu gives high-priority work 4x its low timeslice
 * (os/linux/driver_common.c nvgpu_init_timeslice: 5200 vs 1300 us); critical channels get scale 5, 4x, written before
 * their first submission (the host reads RAMFC when it first loads the channel). */
#define RAMFC_TIMESLICE 0xf8
#define RAMFC_TIMESLICE_HIGH 0x10005080

static int
compositor_timeslice_on(void) /* tegra-gpu-compositor-timeslice=0 keeps nvkm's default for all */
{
	static int on = -1;
	long long v = 1;
	if (on < 0) {
		nvxnu_bootarg_int("tegra-gpu-compositor-timeslice", &v);
		on = v != 0;
	}
	return on;
}

static void chan_timeslice(u32 chid, u32 value)
{
	struct nvkm_runl *runl;
	struct nvkm_chan *nchan;
	unsigned long flags;

	nvkm_runl_foreach(runl, gpu.device->fifo)
	{
		nchan = nvkm_runl_chan_get_chid(runl, chid, &flags);
		if (nchan) {
			nvkm_kmap(nchan->inst);
			nvkm_wo32(nchan->inst, RAMFC_TIMESLICE, value);
			nvkm_done(nchan->inst);
			nvkm_chan_put(&nchan, flags);
			return;
		}
	}
}

static int chan_new(struct tgm_vm *vm, volatile uint32_t *fence_cpu, uint64_t fence_gpu_va,
		    const char *owner, int critical, struct tgm_chan **pchan)
{
	static const u32 classes[5] = {MAXWELL_B, MAXWELL_COMPUTE_B, KEPLER_INLINE_TO_MEMORY_B,
				       FERMI_TWOD_A, MAXWELL_DMA_COPY_A};
	union {
		struct nvif_chan_v0 chan;
		u8 raw[sizeof(struct nvif_chan_v0) + 32];
	} args = {};
	union {
		struct nvif_event_v0 ev;
		u8 raw[sizeof(struct nvif_event_v0) + sizeof(struct nvif_chan_event_v0)];
	} kargs = {};
	struct nvif_chan_event_v0 *host = (struct nvif_chan_event_v0 *)kargs.ev.data;
	struct gf100_vmm_map_v0 margs = {.version = 0, .vol = 1};
	struct nvif_push *push;
	struct tgm_chan *chan;
	u64 runm;
	u32 i, npages = CHAN_BYTES >> PAGE_SHIFT;
	int ret;

	if (gpu.stage < 2)
		return -ENODEV;
	if (!(chan = kzalloc(sizeof(*chan), GFP_KERNEL)))
		return -ENOMEM;
	chan->vm = vm;
	chan->fence_cpu = fence_cpu;
	chan->fence_gpu = fence_gpu_va;
	chan->critical = critical;
	strscpy(chan->owner, owner, sizeof(chan->owner));
	/* a fence word only moves forward: a channel made in a slot continues its predecessor's sequence, so a semaphore
	 * acquire on the old seqno (the GPU present) is never left waiting for a word that went back to 0 */
	chan->submitted = READ_ONCE(*fence_cpu);
	chan->stat_done = chan->submitted;
	chan->ring = dma_alloc_attrs(&gpu_pdev.dev, CHAN_BYTES, &chan->ring_pa, GFP_KERNEL,
				     DMA_ATTR_WRITE_COMBINE);
	chan->ring_pas = kcalloc(npages, sizeof(u64), GFP_KERNEL);
	chan->ring_mem = kzalloc(sizeof(*chan->ring_mem), GFP_KERNEL);
	if (!chan->ring || !chan->ring_pas || !chan->ring_mem) {
		chan_free(chan);
		return -ENOMEM;
	}
	for (i = 0; i < npages; i++)
		chan->ring_pas[i] = chan->ring_pa + ((u64)i << PAGE_SHIFT);

	mutex_lock(&gpu.lock);
	{
		struct nvif_mem_ram_v0 rargs = {.version = 0, .dma = (dma_addr_t *)chan->ring_pas};
		chan->ring_mem->bytes = CHAN_BYTES;
		ret = nvif_mem_ctor_type(&gpu.mmu, "chanRing", NVIF_CLASS_MEM_GF100, gpu.mem_type,
					 PAGE_SHIFT, CHAN_BYTES, &rargs, sizeof(rargs),
					 &chan->ring_mem->mem);
		if (ret) {
			kfree(chan->ring_mem);
			chan->ring_mem = NULL;
		}
	}
	if (!ret)
		ret = nvif_vmm_get(&vm->vmm, ADDR, false, PAGE_SHIFT, 0, CHAN_BYTES,
				   &chan->ring_vma);
	if (!ret) {
		ret = nvif_vmm_map(&vm->vmm, chan->ring_vma.addr, CHAN_BYTES, &margs, sizeof(margs),
				   &chan->ring_mem->mem, 0);
		chan->ring_va = chan->ring_vma.addr;
	}
	if (ret)
		goto fail;

	/* nouveau_channel_ctor(): GPFIFO channel on the GR runlist, in this VM, GPFIFO after the pushbuf */
	runm = nvif_fifo_runlist(&gpu.dev, NV_DEVICE_HOST_RUNLIST_ENGINES_GR);
	args.chan.version = 0;
	args.chan.namelen = 32;
	args.chan.runlist = runm ? __ffs64(runm) : 0;
	args.chan.runq = 0;
	args.chan.priv = 0;
	args.chan.devm = BIT(0);
	args.chan.vmm = nvif_handle(&vm->vmm.object);
	args.chan.ctxdma = 0;
	args.chan.offset = chan->ring_va + CHAN_PUSH_BYTES;
	args.chan.length = CHAN_GPFIFO_BYTES;
	args.chan.huserd = 0;
	args.chan.ouserd = 0;
	snprintf((char *)args.chan.name, 32, "%s", chan->owner); /* nvkm names it in its reports */
	ret = nvif_object_ctor(&gpu.dev.object, "chan", 0, MAXWELL_CHANNEL_GPFIFO_A, &args,
			       sizeof(args), &chan->user);
	chan->chid = args.chan.chid;
	if (!ret && critical && tgm_all_clients() && compositor_timeslice_on())
		chan_timeslice(chan->chid, RAMFC_TIMESLICE_HIGH);
	if (!ret)
		ret = nvif_object_map(&chan->user, NULL, 0); /* USERD (BAR1) for GP_PUT */
	if (ret)
		goto fail;

	/* engine objects: these allocate the GR context (golden image on first use) and bind the CE */
	for (i = 0; i < 5; i++) {
		ret = nvif_object_ctor(&chan->user, "obj", 0x00010000 + i, classes[i], NULL, 0,
				       &chan->obj[chan->nobj]);
		if (ret)
			pr_err("class %04x on channel: %d\n", classes[i], ret);
		else
			chan->nobj++;
	}
	host->version = 0;
	host->type = NVIF_CHAN_EVENT_V0_KILLED;
	ret = nvif_event_ctor(&chan->user, "chanKilled", args.chan.chid, chan_killed, false,
			      &kargs.ev, sizeof(kargs), &chan->kill);
	if (!ret)
		nvif_event_allow(&chan->kill);
	/* nouveau_fence_context_new(): the fence's nonstall interrupt (uchan.c NVIF_CHAN_EVENT_V0_NON_STALL_INTR); its
	 * allow sets PFIFO_INTR_EN_0 bit 31 (gf100_fifo_nonstall_allow). Without it, fences are still seen by polling. */
	host->type = NVIF_CHAN_EVENT_V0_NON_STALL_INTR;
	if (!nvif_event_ctor(&chan->user, "fenceNonStallIntr", args.chan.chid, chan_nonstall, false,
			     &kargs.ev, sizeof(kargs), &chan->nonstall))
		nvif_event_allow(&chan->nonstall);
	ret = nvif_chan906f_ctor(&chan->chan, chan->user.map.ptr,
				 (u8 *)chan->ring + CHAN_PUSH_BYTES, CHAN_GPFIFO_BYTES, chan->ring,
				 chan->ring_va, CHAN_PUSH_BYTES,
				 (u8 *)chan->ring + CHAN_PUSH_BYTES + CHAN_GPFIFO_BYTES,
				 chan->ring_va + CHAN_PUSH_BYTES + CHAN_GPFIFO_BYTES);
	mutex_unlock(&gpu.lock);
	if (ret) {
		mutex_lock(&gpu.lock);
		goto fail;
	}

	/* bind subchannels 0..4 (NV906F SET_OBJECT = method 0, data = class) */
	push = &chan->chan.push;
	ret = PUSH_WAIT(push, 10);
	if (ret) {
		mutex_lock(&gpu.lock);
		goto fail;
	}
	for (i = 0; i < 5; i++) {
		PUSH_DATA(push, (1u << 29) | (1u << 16) | (i << 13) | (0x0000 >> 2));
		PUSH_DATA(push, classes[i]);
	}
	PUSH_KICK(push);
	mutex_lock(&live_lock);
	chan->id = ++live_ids;
	list_add_tail(&chan->live, &live_chans);
	mutex_unlock(&live_lock);
	*pchan = chan;
	return 0;

fail:
	mutex_unlock(&gpu.lock);
	chan_free(chan);
	return ret;
}

/* The gate: its subchannel binds are kicked after gpu.lock is dropped. */
int tgm_chan_new(struct tgm_vm *vm, volatile uint32_t *fence_cpu, uint64_t fence_gpu_va,
		 const char *owner, int critical, struct tgm_chan **pchan)
{
	int ret;

	gate_enter();
	ret = chan_new(vm, fence_cpu, fence_gpu_va, owner, critical, pchan);
	gate_leave();
	return ret;
}

static void order_forget(struct tgm_chan *chan);

void tgm_chan_del(struct tgm_chan **pchan)
{
	struct tgm_chan *chan = *pchan;
	u64 end;
	if (!chan)
		return;
	/* bounded wait for idle (CONTRACT.md: <= 1 s), then nvkm tears the channel down (preempt + runlist) */
	end = nvxnu_nanotime() + NSEC_PER_SEC;
	for (;;) {
		u32 seq = nvxnu_wake_seq();
		if (chan->error || (s32)(tgm_chan_completed(chan) - chan->submitted) >= 0 ||
		    nvxnu_nanotime() >= end)
			break;
		if (!nvxnu_idle(seq, end) && (s32)(tgm_chan_completed(chan) - chan->submitted) >= 0)
			nvxnu_wake_due(seq, "fence", chan->submitted);
	}
	/* tearing down a channel that still runs would free pages the GPU is using */
	if (gpu.stage >= 3 && !chan->error && (s32)(tgm_chan_completed(chan) - chan->submitted) < 0)
		tgm_chan_hung(chan, "channel destroy", chan->submitted, NSEC_PER_SEC);
	/* the idle clock's count: what it finished, and (a dead channel) what it never will */
	idle_fenced(chan);
	__atomic_fetch_add(&idle.retired, chan->submitted - chan->gov_done, __ATOMIC_SEQ_CST);
	chan->gov_done = chan->submitted;
	mutex_lock(&live_lock);
	list_del(&chan->live);
	order_forget(chan);
	mutex_unlock(&live_lock);
	mutex_lock(&gpu.lock);
	chan_free(chan);
	mutex_unlock(&gpu.lock);
	*pchan = NULL;
}

/* fence pages:
 * Every connection's fence page (its channels' seqno words) is one page of a single pool range, so the GPU present's
 * address space maps all of them once and waits on any user channel with a semaphore acquire. Pages are handed out
 * round-robin and zeroed, so a page freed with an acquire still pending on it is not reused at once. */
#define TGM_FENCE_PAGES 64 /* one per connection (TegraGM20BUserClient.cpp kMaxConnections) */
static struct {
	void *va;
	u64 pa;
	u64 used; /* bitmap of pages in use */
	u32 next;
} fences;

int tgm_fence_page_get(volatile uint32_t **va, uint64_t *pa)
{
	u32 i, n;
	int ret = -ENOMEM;

	mutex_lock(&live_lock);
	if (!fences.va)
		fences.va = tgm_pool_alloc(TGM_FENCE_PAGES * PAGE_SIZE, &fences.pa);
	for (n = 0; fences.va && n < TGM_FENCE_PAGES; n++) {
		i = (fences.next + n) % TGM_FENCE_PAGES;
		if (fences.used & BIT_ULL(i))
			continue;
		fences.used |= BIT_ULL(i);
		fences.next = i + 1;
		memset((u8 *)fences.va + i * PAGE_SIZE, 0, PAGE_SIZE);
		*va = (volatile uint32_t *)((u8 *)fences.va + i * PAGE_SIZE);
		*pa = fences.pa + i * PAGE_SIZE;
		ret = 0;
		break;
	}
	mutex_unlock(&live_lock);
	return ret;
}

void tgm_fence_page_put(volatile uint32_t *va)
{
	uintptr_t off = (uintptr_t)va - (uintptr_t)fences.va;
	if (!va)
		return;
	mutex_lock(&live_lock);
	fences.used &= ~BIT_ULL(off / PAGE_SIZE);
	mutex_unlock(&live_lock);
}

/* order between the channels of a task:
 * Two drivers in one process (the render server's raw GM20B path and NVK) each have their own connection, address space and
 * channels, and Metal's order between their command buffers is the order the process submits them in. tgm_chan_order
 * (TegraGM20BABI.h method 14) keeps it on the GPU, with no CPU wait: it queues semaphore ACQUIRE_GEQs (acquire switch on, as
 * the present's) on the fence words of the task's other channels, which the waiting channel's next tgm_chan_exec kicks as an
 * entry of their own ahead of its pushes. Every address space maps the whole fence pool read-only for them (fences_map, from
 * tgm_vm_new), as the present's does. Each acquire names a seqno already submitted when it was queued (AfterTask: the other
 * channels' last; BeforeTask: one up to the channel's last), and a channel's entries run in order, so no two submissions can
 * wait for each other. A source channel's teardown waits for it to go idle and drops what was queued on it (order_forget),
 * so no acquire is left on a fence page handed out again; one already kicked has passed by then. A waiter stuck on one is a
 * hang like any other (tgm_chan_check_progress), and its line names the acquire (tgm_chan_hung). */
static DEFINE_MUTEX(fences_mem_lock);
static struct tgm_mem *fences_mem; /* the whole pool as GPU memory, made once and kept */
static u64 fences_pas[TGM_FENCE_PAGES];

static int fences_map(struct tgm_vm *vm)
{
	u32 i;
	int ret = 0;

	mutex_lock(&fences_mem_lock);
	if (!fences_mem) {
		/* the pool exists from the first connection on; make sure it does now */
		mutex_lock(&live_lock);
		if (!fences.va)
			fences.va = tgm_pool_alloc(TGM_FENCE_PAGES * PAGE_SIZE, &fences.pa);
		mutex_unlock(&live_lock);
		if (!fences.va)
			ret = -ENOMEM;
		for (i = 0; !ret && i < TGM_FENCE_PAGES; i++)
			fences_pas[i] = fences.pa + i * PAGE_SIZE;
		if (!ret)
			ret = tgm_mem_new(vm, fences_pas, TGM_FENCE_PAGES, &fences_mem);
	}
	mutex_unlock(&fences_mem_lock);
	if (!ret)
		ret = vm_kmap(vm, fences_mem, TGM_FENCE_PAGES * PAGE_SIZE, 1, &vm->fences_va);
	return ret;
}

void tgm_chan_set_group(struct tgm_chan *chan, const void *group)
{
	mutex_lock(&live_lock);
	chan->group = group;
	mutex_unlock(&live_lock);
}

/* live_lock held: `waiter`'s next submission waits for `source` to complete `seqno` (one entry per source, the later seqno) */
static int order_add(struct tgm_chan *waiter, struct tgm_chan *source, u32 seqno)
{
	uintptr_t off = (uintptr_t)source->fence_cpu - (uintptr_t)fences.va;
	u32 i;

	if (!waiter->vm->fences_va || !fences.va || off >= TGM_FENCE_PAGES * PAGE_SIZE)
		return -ENODEV;
	for (i = 0; i < waiter->norder; i++) {
		if (waiter->order[i].id != source->id)
			continue;
		if ((s32)(seqno - waiter->order[i].seqno) > 0)
			waiter->order[i].seqno = seqno;
		return 0;
	}
	if (waiter->norder == TGM_ORDER_MAX)
		return -ENOSPC;
	waiter->order[waiter->norder].va = waiter->vm->fences_va + off;
	waiter->order[waiter->norder].cpu = source->fence_cpu;
	waiter->order[waiter->norder].id = source->id;
	waiter->order[waiter->norder].seqno = seqno;
	waiter->norder++;
	return 0;
}

int tgm_chan_order(struct tgm_chan *chan, uint32_t flags, uint32_t seqno, uint32_t *queued)
{
	struct tgm_chan *other;
	u32 n = 0, last;
	int ret = 0;

	*queued = 0;
	if (chan->kernel || !flags || (flags & ~(TGM_ORDER_AFTER | TGM_ORDER_BEFORE)))
		return -EINVAL;
	if (flags & TGM_ORDER_BEFORE) {
		if (!seqno)
			seqno = chan->submitted;
		if ((s32)(seqno - chan->submitted) > 0)
			return -EINVAL; /* only submitted work: an acquire never waits for a later submission */
		if ((s32)(tgm_chan_completed(chan) - seqno) >= 0)
			flags &= ~TGM_ORDER_BEFORE; /* done already: nothing to wait for */
	}
	mutex_lock(&live_lock);
	list_for_each_entry(other, &live_chans, live)
	{
		if (other == chan || other->kernel || !chan->group || other->group != chan->group ||
		    READ_ONCE(other->error))
			continue;
		last = READ_ONCE(other->submitted);
		if ((flags & TGM_ORDER_AFTER) && (s32)(tgm_chan_completed(other) - last) < 0) {
			ret = order_add(chan, other, last);
			if (ret)
				break;
			n++;
		}
		if (flags & TGM_ORDER_BEFORE) {
			ret = order_add(other, chan, seqno);
			if (ret)
				break;
			n++;
		}
	}
	mutex_unlock(&live_lock);
	*queued = n;
	return ret;
}

/* live_lock held, `chan` leaving: what other channels had queued on its fence word goes (it is idle: nothing to wait for) */
static void order_forget(struct tgm_chan *chan)
{
	struct tgm_chan *other;
	u32 i;

	list_for_each_entry(other, &live_chans, live)
	{
		for (i = 0; i < other->norder;) {
			if (other->order[i].id == chan->id)
				other->order[i] = other->order[--other->norder];
			else
				i++;
		}
	}
}

/* From tgm_chan_exec, before a submission's own entries: the acquires tgm_chan_order queued for it, as one entry. One whose
 * source passed its seqno already is left out. */
static void order_waits(struct tgm_chan *chan)
{
	struct nvif_push *push = &chan->chan.push;
	struct tgm_order wait[TGM_ORDER_MAX];
	u32 i, n, kicked = 0;

	mutex_lock(&live_lock);
	n = chan->norder;
	memcpy(wait, chan->order, n * sizeof(wait[0]));
	chan->norder = 0;
	mutex_unlock(&live_lock);
	for (i = 0; i < n; i++) {
		if ((s32)(READ_ONCE(*wait[i].cpu) - wait[i].seqno) >= 0)
			continue;
		PUSH_MTHD(push, NV906F, SEMAPHOREA,
			  NVVAL(NV906F, SEMAPHOREA, OFFSET_UPPER, upper_32_bits(wait[i].va)),
			  SEMAPHOREB, lower_32_bits(wait[i].va), SEMAPHOREC, wait[i].seqno,
			  SEMAPHORED,
			  NVDEF(NV906F, SEMAPHORED, OPERATION, ACQ_GEQ) |
				  NVDEF(NV906F, SEMAPHORED, ACQUIRE_SWITCH, ENABLED));
		chan->waited = wait[i];
		kicked++;
	}
	if (kicked)
		PUSH_KICK(push);
}

/* CPU readers of GPU-written memory:
 * The GPU is not IO-coherent; its sysmem PTEs are volatile (no GPU L2 lines), so a submission's writes are in DRAM once
 * its fence has passed, but the CPU may still hold stale lines of those pages (e.g. from copying the same IOSurface
 * three frames ago). For a reader outside the GPU client, NativeLegacyDisplay's copy of a swapped IOSurface: wait until
 * every submission made so far on every live channel has completed (or its channel died), then clean+invalidate
 * [va, va + bytes) to the point of coherency. With no user channel it returns at once and touches no cache, so the CPU
 * renderer's path pays nothing. Returns 0, or -ETIMEDOUT after timeout_ns (the range is invalidated either way). */
#define TGM_SYNC_MAX 512 /* 64 connections x 8 channels */

static int sync_done(u64 id, u32 seqno)
{
	struct tgm_chan *chan;
	int done = 1; /* a channel that is gone was drained or stopped by tgm_chan_del */
	mutex_lock(&live_lock);
	list_for_each_entry(chan, &live_chans, live)
	{
		if (chan->id == id) {
			done = chan->error || (s32)(tgm_chan_completed(chan) - seqno) >= 0;
			break;
		}
	}
	mutex_unlock(&live_lock);
	return done;
}

int tegra_gm20b_sync_for_cpu(const void *va, uint64_t bytes, uint64_t timeout_ns)
{
	static struct {
		u64 id;
		u32 seqno;
	} want[TGM_SYNC_MAX]; /* 8 KiB: not on the kernel stack; sync_lock */
	static DEFINE_MUTEX(sync_lock);
	struct tgm_chan *chan;
	unsigned int n = 0, i;
	int ret = 0;
	u64 end;

	mutex_lock(&sync_lock);
	mutex_lock(&live_lock);
	list_for_each_entry(chan, &live_chans, live)
	{
		if (n < TGM_SYNC_MAX) {
			want[n].id = chan->id;
			want[n].seqno = READ_ONCE(chan->submitted);
			n++;
		}
	}
	mutex_unlock(&live_lock);
	if (!n) {
		mutex_unlock(&sync_lock);
		return 0;
	}
	end = nvxnu_nanotime() + timeout_ns;
	for (i = 0; i < n && !ret; i++) {
		for (;;) {
			u32 seq = nvxnu_wake_seq();
			if (sync_done(want[i].id, want[i].seqno))
				break;
			if (nvxnu_nanotime() >= end) {
				ret = -ETIMEDOUT;
				break;
			}
			if (!nvxnu_idle(seq, end) && sync_done(want[i].id, want[i].seqno))
				nvxnu_wake_due(seq, "fence", want[i].seqno);
		}
		/* the caller would copy a surface the GPU may still be writing: panic naming the channel it waited for */
		if (ret && gpu.stage >= 3) {
			mutex_lock(&live_lock);
			list_for_each_entry(chan, &live_chans, live)
			{
				if (chan->id == want[i].id)
					tgm_chan_hung(chan, "sync_for_cpu", want[i].seqno,
						      timeout_ns);
			}
			mutex_unlock(&live_lock);
		}
	}
	mutex_unlock(&sync_lock);
	nvxnu_dcache_clean_inval(va, bytes);
	return ret;
}

void tgm_cpu_clean_pages(const uint64_t *pas, uint32_t npages)
{
	nvxnu_dcache_clean_inval_pages(pas, npages);
}

/* Host methods of MAXWELL_CHANNEL_GPFIFO_A not in the trimmed cl906f.h (open-gpu-doc classes/host/clb06f.h) */
#define NVB06F_FB_FLUSH 0x0024
#define NVB06F_MEM_OP_C 0x0030
#define NVB06F_MEM_OP_D 0x0034
#define NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY (0x10u << 27)
#define NVB06F_WFI 0x0078

/* One 16-byte semaphore release (writes payload and the GPU's PTIMER ns), after wait-for-idle */
static void push_stamp(struct nvif_push *push, u64 va, u32 payload)
{
	PUSH_MTHD(push, NV906F, SEMAPHOREA,
		  NVVAL(NV906F, SEMAPHOREA, OFFSET_UPPER, upper_32_bits(va)), SEMAPHOREB,
		  lower_32_bits(va), SEMAPHOREC, payload, SEMAPHORED,
		  NVDEF(NV906F, SEMAPHORED, OPERATION, RELEASE) |
			  NVDEF(NV906F, SEMAPHORED, RELEASE_WFI, EN) |
			  NVDEF(NV906F, SEMAPHORED, RELEASE_SIZE, 16BYTE));
}

/* Push dwords chan_fence() may write, at most */
#define CHAN_FENCE_DWORDS 32

/* Fence push: semaphore release of the next 32-bit seqno after wait-for-idle, then a nonstall interrupt (nouveau
 * nvc0_fence_emit32), then kick. With tegra-gpu-l2=1 (and on the present channel) the L2's dirty lines go to DRAM first; with
 * tegra-gpu-stats=1 the end of the submission is stamped. Space for it was reserved by the caller
 * (nvif_chan_gpfifo_wait / PUSH_WAIT): CHAN_FENCE_DWORDS. */
static u32 chan_fence(struct tgm_chan *chan)
{
	struct nvif_push *push = &chan->chan.push;
	u32 next = chan->submitted + 1;
	idle_wake(); /* backstop: tgm_chan_exec and tgm_present_copy raise it before their first kick */
	if (l2_on() || chan->kernel) {
		/* engines idle, their writes committed to the L2 (FB flush: nvgpu gk20a_mm_fb_flush), the L2's dirty lines
		 * written to DRAM, and those writes committed before the release below. Always on the present channel: its
		 * completion promises the display every byte in DRAM (TegraScanoutCopy.h). */
		PUSH_NVSQ(push, NV906F, NVB06F_WFI, 0);
		PUSH_NVSQ(push, NV906F, NVB06F_FB_FLUSH, 0);
		PUSH_NVSQ(push, NV906F, NVB06F_MEM_OP_C, 0, NVB06F_MEM_OP_D,
			  NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY);
		PUSH_NVSQ(push, NV906F, NVB06F_FB_FLUSH, 0);
	}
	if (stats_on())
		push_stamp(push, chan_stamps_va(chan, next) + offsetof(struct tgm_stamps, end),
			   next);
	PUSH_MTHD(push, NV906F, SEMAPHOREA,
		  NVVAL(NV906F, SEMAPHOREA, OFFSET_UPPER, upper_32_bits(chan->fence_gpu)),
		  SEMAPHOREB, lower_32_bits(chan->fence_gpu), SEMAPHOREC, next, SEMAPHORED,
		  NVDEF(NV906F, SEMAPHORED, OPERATION, RELEASE) |
			  NVDEF(NV906F, SEMAPHORED, RELEASE_WFI, EN) |
			  NVDEF(NV906F, SEMAPHORED, RELEASE_SIZE, 4BYTE),
		  NON_STALL_INTERRUPT, 0);
	__asm__ volatile(
		"dsb sy" ::
			: "memory"); /* client WC writes and our push reach DRAM before GP_PUT */
	chan->submit_ns = nvxnu_nanotime();
	if (stats_on())
		WRITE_ONCE(chan->stat_sub_ns[next % CHAN_NSTAMPS], chan->submit_ns);
	PUSH_KICK(push);
	chan->submitted = next;
	idle_kicked();
	idle_stats();
	return next;
}

/* Present order (tegra-gpu-present-order): held by a user submission from its present wait to its kick, and by the present
 * from its snapshot of the user channels to its kick, so every user submission is either in a present's snapshot (the
 * copy waits for it) or kicked after that present (it waits for the copy). */
static DEFINE_MUTEX(order_lock);
static void present_order(struct tgm_chan *chan);

static int chan_exec(struct tgm_chan *chan, const struct tgm_push *pushes, uint32_t count,
		     uint32_t *seqno, const struct tgm_prof *prof)
{
	int ordered = present_order_on() && !chan->kernel && chan->vm->pfence_va;
	u32 i, stamps = 0;
	int ret;

	if (chan->error)
		return -EIO;
	if (!count) {
		*seqno = chan->submitted;
		return 0;
	}
	idle_wake(); /* the run rate before any of this submission is kicked */
	/* a push profile (see "push profile"): only with the stats' stamps, one in flight per channel; one whose completion
	 * the stats' interrupt pass no longer looks at (more than CHAN_NSTAMPS behind) is given up */
	{
		u32 ps = READ_ONCE(chan->prof_seqno);
		if (ps && (s32)(tgm_chan_completed(chan) - ps) > CHAN_NSTAMPS)
			WRITE_ONCE(chan->prof_seqno, 0);
	}
	if (prof &&
	    (!stats_on() || chan->kernel || count > TGM_PROF_MAX || READ_ONCE(chan->prof_seqno)))
		prof = NULL;
	for (i = 0; prof && i < count; i++)
		stamps += prof->cls[i] != TGM_PC_SKIP;
	/* nouveau_exec_job_run(): room for every user entry plus our fence push and, each kicked as its own entry before
	 * them, a present wait, the task order's acquires and (tegra-gpu-stats) a start stamp, one more nvif GET semaphore
	 * with each; nvif polls GET for up to ~1 s. A profile's stamps: an entry each, a stamp and a GET semaphore. */
	ret = nvif_chan_gpfifo_wait(&chan->chan, count + 4 + stamps,
				    CHAN_FENCE_DWORDS + 24 + 5 * TGM_ORDER_MAX + 16 * stamps);
	if (ret == -ETIMEDOUT && gpu.stage >= 3)
		tgm_chan_hung(chan, "exec: GPFIFO still full after 1 s", chan->submitted,
			      NSEC_PER_SEC);
	if (ret)
		return ret == -ETIMEDOUT ? -EBUSY : ret;
	if (l2_on())
		l2_invalidate(); /* before the host can fetch anything of this submission */
	if (ordered) {
		mutex_lock(&order_lock);
		present_order(chan); /* first, so the start stamp below times the work itself */
	}
	if (READ_ONCE(chan->norder))
		order_waits(chan); /* the task's other channels' work it follows (tgm_chan_order) */
	if (stats_on()) {
		u32 next = chan->submitted + 1;
		WRITE_ONCE(chan->stat_sub_ptimer[next % CHAN_NSTAMPS],
			   nvkm_timer_read(gpu.device->timer));
		push_stamp(&chan->chan.push,
			   chan_stamps_va(chan, next) + offsetof(struct tgm_stamps, start), next);
		PUSH_KICK(&chan->chan.push);
	}
	if (prof) {
		memcpy(chan->prof_cls, prof->cls, count);
		memcpy(chan->prof_draws, prof->draws, count * sizeof(prof->draws[0]));
		memcpy(chan->prof_sdraws, prof->sampleDraws, count * sizeof(prof->sampleDraws[0]));
		WRITE_ONCE(chan->prof_count, count);
		WRITE_ONCE(chan->prof_seqno,
			   chan->submitted + 1); /* before the fence that completes it */
	}
	for (i = 0; i < count; i++) {
		nvif_chan_gpfifo_push(&chan->chan, pushes[i].va, pushes[i].dwords << 2,
				      pushes[i].no_prefetch);
		if (prof && prof->cls[i] != TGM_PC_SKIP) {
			push_stamp(&chan->chan.push,
				   chan->ring_va + CHAN_PROF + i * sizeof(struct tgm_stamp),
				   chan->submitted + 1);
			PUSH_KICK(&chan->chan.push);
		}
	}

	*seqno = chan_fence(chan);
	if (ordered)
		mutex_unlock(&order_lock);
	if (stats_on())
		stats_tick();
	return 0;
}

int tgm_chan_exec(struct tgm_chan *chan, const struct tgm_push *pushes, uint32_t count,
		  uint32_t *seqno, const struct tgm_prof *prof)
{
	int ret;

	gate_enter();
	ret = chan_exec(chan, pushes, count, seqno, prof);
	gate_leave();
	return ret;
}

/* GPU present (TegraDisplayFlip/TegraScanoutCopy.h):
 * The display's swap path (boot-arg ioswitch-gpu-copy=1) hands a frame (an IOSurface, pitch-linear BGRA) to the GPU instead
 * of waiting for the GPU and swizzling it with the CPU (TegraGM20BUserClient.cpp tegra_gm20b_scanout_copy). One kernel
 * channel in its own address space, which maps the scanout surfaces, the swapped surfaces and every connection's fence
 * page (tgm_fence_page_get), does per frame:
 *  - a semaphore ACQUIRE_GEQ (acquire switch on: the channel yields while it waits) on the last submitted seqno of every
 *    user channel whose address space imports the surface (its first page, tgm_vm_import_note; every user channel if
 *    none does, or with tegra-gpu-present-writer=0), so the copy runs after the compositor's rendering in GPU order, no
 *    CPU waits for it, and an app's GPU work does not hold up the frame;
 *  - one MAXWELL_DMA_COPY_A LAUNCH_DMA, pitch source (the surface's row bytes) to a BLOCKLINEAR destination: GOBs of
 *    64 bytes x 8 lines (GOB_HEIGHT_FERMI_8), blocks one GOB wide and 16 GOBs high, width 5120 bytes: the layout window A
 *    column-scans (NativeScanout.h native_legacy_block_copy);
 *  - the kernel fence (chan_fence: WFI, with tegra-gpu-l2 the L2 flush, semaphore release, NON_STALL_INTERRUPT).
 * The fence's nonstall interrupt completes it: the caller's callback (native_legacy_gpu_complete, which any context may
 * call) runs from the interrupt pass. A present that has not completed 1 s after it was queued panics, naming what it
 * waits for. Every mapping is volatile: the source was written by work whose fence flushed the L2 (or bypassed it), the
 * destination is read by the display controller only. Methods from open-gpu-doc classes/dma-copy/clb0b5.h.
 * Present order (tegra-gpu-present-order, on unless =0): the display retires a swap as soon as its copy is queued, so the
 * compositor may draw its next frame into the surface the copy still reads, which tears. Every user submission
 * kicked after a present that has not completed, from an address space that imports its surface (any, if the present
 * found none), starts with a semaphore ACQUIRE_GEQ on the present channel's fence (mapped read-only in each address
 * space, pfence), with the acquire switch on, so it waits in GPU order and the host runs other channels meanwhile. No
 * cycle: a present waits only for submissions kicked before it (order_lock). */
#define NVB0B5_LAUNCH_DMA 0x0300
#define NVB0B5_LAUNCH_DMA_NON_PIPELINED (2u << 0)
#define NVB0B5_LAUNCH_DMA_FLUSH_ENABLE (1u << 2)
#define NVB0B5_LAUNCH_DMA_SRC_PITCH (1u << 7)       /* SRC_MEMORY_LAYOUT 7:7, PITCH 1 */
#define NVB0B5_LAUNCH_DMA_DST_BLOCKLINEAR (0u << 8) /* DST_MEMORY_LAYOUT 8:8, BLOCKLINEAR 0 */
#define NVB0B5_LAUNCH_DMA_MULTI_LINE (1u << 9)
#define NVB0B5_OFFSET_IN_UPPER 0x0400
#define NVB0B5_OFFSET_IN_LOWER 0x0404
#define NVB0B5_OFFSET_OUT_UPPER 0x0408
#define NVB0B5_OFFSET_OUT_LOWER 0x040c
#define NVB0B5_PITCH_IN 0x0410
#define NVB0B5_PITCH_OUT 0x0414
#define NVB0B5_LINE_LENGTH_IN 0x0418
#define NVB0B5_LINE_COUNT 0x041c
#define NVB0B5_SET_DST_BLOCK_SIZE 0x070c
#define NVB0B5_BLOCK_SIZE_FERMI_8(log2h) \
	(((log2h) << 4) | (1u << 12)) /* WIDTH 1 GOB, HEIGHT 2^log2h, DEPTH 1, FERMI_8 */
#define NVB0B5_SET_DST_WIDTH 0x0710
#define NVB0B5_SET_DST_HEIGHT 0x0714
#define NVB0B5_SET_DST_DEPTH 0x0718
#define NVB0B5_SET_DST_LAYER 0x071c
#define NVB0B5_SET_DST_ORIGIN 0x0720
#define TGM_PRESENT_NS \
	(1000 *        \
	 NSEC_PER_MSEC) /* as the display's own limit: compositor jobs up to ~90 ms were seen */

static struct {
	struct mutex lock;
	struct tgm_vm *vm;
	struct tgm_chan *chan;
	struct tgm_mem *fence_mem, *fences_mem;
	void *fence; /* the channel's own fence page (pool) */
	u64 fence_pa, fence_gpu, fences_gpu;
	void *deadline_work;
	u32 pending; /* seqno of the present in flight, 0: none (atomic: the interrupt pass completes it) */
	u64 queued;
	void (*done)(void *);
	void *arg;
	u32 waits; /* the acquires of the present in flight, for the deadline's line */
	struct {
		u64 id;
		u32 seqno;
	} wait[TGM_SYNC_MAX];
	volatile struct tgm_stamp *wait_end
		[TGM_SYNC_MAX]; /* tegra-gpu-stats: their end stamps (pool memory, kept mapped) */
	u32 stamped; /* tegra-gpu-stats: seqno of the present in flight, whose copy is stamped */
	u32 last;    /* seqno of the last present kicked, 0: none (order_lock) */
	u64 last_src; /* its surface's first page, and whether address spaces importing it were found */
	int last_matched;
	int last_idle,
		last_late; /* tegra-gpu-stats: it found no user work pending; a user submit then waited for it */
	u32 presents; /* tegra-gpu-stats */
	u64 lat_sum, lat_max;
} present = {.lock = {NULL}};

/* From the present channel's nonstall interrupt (the interrupt pass, nvkm's event lock held): a present whose fence passed
 * is done, and its callback runs here (the display's only signals its work loop). */
static void present_fenced(struct tgm_chan *chan)
{
	u32 pending = __atomic_load_n(&present.pending, __ATOMIC_ACQUIRE);
	void (*done)(void *);
	void *arg;

	if (!pending || (s32)(tgm_chan_completed(chan) - pending) < 0)
		return;
	done = present.done;
	arg = present.arg;
	if (!__atomic_compare_exchange_n(&present.pending, &pending, 0, false, __ATOMIC_ACQ_REL,
					 __ATOMIC_RELAXED))
		return;
	nvxnu_work_cancel(present.deadline_work);
	if (stats_on()) {
		__atomic_fetch_add(&present.presents, 1, __ATOMIC_RELAXED);
		stats_add(&present.lat_sum, &present.lat_max, nvxnu_nanotime() - present.queued);
	}
	done(arg);
}

/* TGM_PRESENT_NS after a present was queued, if it has not completed: name the first user channel it still waits for. */
static void present_deadline(void *arg)
{
	struct tgm_chan *chan, *waited = NULL;
	char state[320], what[320];
	u64 age;
	u32 i, seqno = 0;

	(void)arg;
	mutex_lock(&present.lock);
	if (!__atomic_load_n(&present.pending, __ATOMIC_ACQUIRE)) {
		mutex_unlock(&present.lock);
		return;
	}
	/* a deadline of an earlier present that was already running when that one completed: this one has its own time */
	age = nvxnu_nanotime() - present.queued;
	if (age < TGM_PRESENT_NS) {
		nvxnu_work_schedule_after(present.deadline_work, TGM_PRESENT_NS - age);
		mutex_unlock(&present.lock);
		return;
	}
	if ((s32)(tgm_chan_completed(present.chan) - present.pending) >= 0)
		nvxnu_panic("tegra-gm20b: present fence %u complete after %llu ms but its nonstall "
			    "interrupt never came",
			    present.pending, (nvxnu_nanotime() - present.queued) / 1000000);
	mutex_lock(&live_lock);
	for (i = 0; i < present.waits && !waited; i++) {
		list_for_each_entry(chan, &live_chans, live)
		{
			if (chan->id == present.wait[i].id &&
			    (s32)(tgm_chan_completed(chan) - present.wait[i].seqno) < 0) {
				waited = chan;
				seqno = present.wait[i].seqno;
				break;
			}
		}
	}
	if (waited)
		chan_describe(waited, what, sizeof(what));
	else
		scnprintf(what, sizeof(what), "none of its %u user fences (the copy itself)",
			  present.waits);
	chan_describe(present.chan, state, sizeof(state));
	nvxnu_panic("tegra-gm20b: present not done %llu ms after it was queued, waiting for "
		    "%s%s%u: present %s",
		    (nvxnu_nanotime() - present.queued) / 1000000, what, waited ? " to reach " : "",
		    waited ? seqno : 0, state);
}

/* From tgm_chan_exec, order_lock held, before a user submission's own entries: if the last present kicked has not
 * completed, the submission waits for it on the GPU. One completed already (the CPU sees its fence) needs no wait. */
static void present_order(struct tgm_chan *chan)
{
	struct tgm_chan *pchan = READ_ONCE(present.chan);
	struct nvif_push *push = &chan->chan.push;
	u32 last = present.last;
	u64 va = chan->vm->pfence_va;
	int writer = 1;

	if (!last || !pchan || (s32)(tgm_chan_completed(pchan) - last) >= 0)
		return;
	if (present.last_matched) { /* only an address space that imports the surface can draw into it */
		mutex_lock(&live_lock);
		writer = vm_imports(chan->vm, present.last_src);
		mutex_unlock(&live_lock);
	}
	if (!writer)
		return;
	PUSH_MTHD(push, NV906F, SEMAPHOREA,
		  NVVAL(NV906F, SEMAPHOREA, OFFSET_UPPER, upper_32_bits(va)), SEMAPHOREB,
		  lower_32_bits(va), SEMAPHOREC, last, SEMAPHORED,
		  NVDEF(NV906F, SEMAPHORED, OPERATION, ACQ_GEQ) |
			  NVDEF(NV906F, SEMAPHORED, ACQUIRE_SWITCH, ENABLED));
	PUSH_KICK(push);
	if (stats_on()) {
		__atomic_fetch_add(&stats.ordered, 1, __ATOMIC_RELAXED);
		if (present.last_idle && !present.last_late) {
			present.last_late = 1;
			__atomic_fetch_add(&stats.late, 1, __ATOMIC_RELAXED);
		}
	}
}

/* tegra-gpu-stats, from stats_fences (interrupt pass) before present_fenced completes the present: PTIMER from the end of
 * the last user submission present `seqno` waited for to the copy's start (its start stamp, after the acquires), ~0 when
 * it waited for none or a stamp is missing. The wait list is that present's: the next is queued only after
 * present_fenced. A user channel's stamps are in its ring, pool memory that stays mapped even after the channel is
 * gone; a reused page fails the payload check. */
static u64 present_gap(u32 seqno, u64 start)
{
	u64 last = 0;
	u32 i;

	if (present.stamped != seqno || !present.waits)
		return ~0ULL;
	for (i = 0; i < present.waits; i++) {
		volatile struct tgm_stamp *end = present.wait_end[i];
		if (!end || end->payload != present.wait[i].seqno)
			return ~0ULL;
		if (end->ns > last)
			last = end->ns;
	}
	return start >= last ? start - last : 0;
}

static void present_stats(u32 *presents, u64 *sum, u64 *max)
{
	*presents = __atomic_exchange_n(&present.presents, 0, __ATOMIC_RELAXED);
	*sum = __atomic_exchange_n(&present.lat_sum, 0, __ATOMIC_RELAXED);
	*max = __atomic_exchange_n(&present.lat_max, 0, __ATOMIC_RELAXED);
}

int tgm_present_open(struct tgm_vm **pvm)
{
	u64 pas[TGM_FENCE_PAGES];
	volatile u32 *probe;
	u64 probe_pa;
	u32 i;
	int ret = 0;

	if (gpu.stage < 3)
		return -ENODEV;
	mutex_lock(&present.lock);
	if (present.chan)
		goto out;
	/* the fence pages exist from the first connection on; make sure they do now */
	ret = tgm_fence_page_get(&probe, &probe_pa);
	if (ret)
		goto out;
	tgm_fence_page_put(probe);
	for (i = 0; i < TGM_FENCE_PAGES; i++)
		pas[i] = fences.pa + i * PAGE_SIZE;
	present.deadline_work = nvxnu_work_alloc(present_deadline, NULL);
	ret = present.deadline_work ? tgm_vm_new(&present.vm) : -ENOMEM;
	if (!ret)
		ret = pfence_get(
			present.vm); /* the channel's own fence: the page every address space may wait on */
	if (!ret) {
		present.fence = pfence.va;
		present.fence_pa = pfence.pa;
		present.fence_mem = pfence.mem;
		ret = tgm_vm_kmap(present.vm, present.fence_mem, PAGE_SIZE, &present.fence_gpu);
	}
	if (!ret)
		ret = tgm_mem_new(present.vm, pas, TGM_FENCE_PAGES, &present.fences_mem);
	if (!ret)
		ret = tgm_vm_kmap(present.vm, present.fences_mem, TGM_FENCE_PAGES * PAGE_SIZE,
				  &present.fences_gpu);
	if (!ret)
		ret = tgm_chan_new(present.vm, present.fence, present.fence_gpu, "present", 1,
				   &present.chan);
	if (!ret) {
		present.chan->kernel = CHAN_PRESENT;
	} else {
		/* stays unusable: every offer is refused and the display copies with the CPU */
		nvxnu_log("tegra-gm20b: GPU present unavailable: %d\n", ret);
	}
out:
	*pvm = present.chan ? present.vm : NULL;
	mutex_unlock(&present.lock);
	return present.chan ? 0 : ret ? ret : -ENODEV;
}

/* Present mappings: volatile, at a fixed address of the present's user range. */
int tgm_present_map(uint64_t va, struct tgm_mem *mem, uint64_t bytes, uint8_t kind)
{
	return present.vm ? vm_map(present.vm, va, bytes, mem, 0, kind, 0, 1) : -ENODEV;
}

int tgm_present_unmap(uint64_t va, uint64_t bytes)
{
	return present.vm ? tgm_vm_unmap(present.vm, va, bytes) : -ENODEV;
}

/* tegra-gpu-present-writer (on unless =0): a present waits only for the address spaces that import its surface, and only
 * their later submissions wait for it (present order); =0, or a surface no address space imports by its first page, is
 * the all-channel wait. */
static int present_writer_on(void)
{
	static int on = -1;
	long long v = 1;
	if (on < 0) {
		nvxnu_bootarg_int("tegra-gpu-present-writer", &v);
		on = v != 0;
	}
	return on;
}

static int present_copy(uint64_t src_va, uint64_t src_pa, uint64_t stride, uint32_t width,
			uint32_t height, uint64_t dst_va, uint32_t dst_pitch,
			uint32_t block_height_log2, void (*done)(void *), void *arg)
{
	struct tgm_chan *chan = present.chan, *user;
	struct nvif_push *push;
	u64 va;
	u32 n = 0, room = 0;
	int ret, matched = 0;

	if (!chan || chan->error)
		return -ENODEV;
	idle_wake(); /* the run rate before the copy is kicked */
	/* dst_va 0: no copy, only the acquires and the fence (direct scanout: the display scans the surface itself once the
	 * compositor's rendering of it is done) */
	if (dst_va && (block_height_log2 > 5 || dst_pitch < width * 4))
		return -EINVAL;
	mutex_lock(&present.lock);
	if (__atomic_load_n(&present.pending, __ATOMIC_ACQUIRE)) {
		mutex_unlock(&present.lock);
		return -EBUSY;
	}
	/* push room for an acquire per user channel there is now (one made meanwhile has nothing for this frame) */
	mutex_lock(&live_lock);
	list_for_each_entry(user, &live_chans, live) room += room < TGM_SYNC_MAX && !user->kernel;
	mutex_unlock(&live_lock);
	push = &chan->chan.push;
	ret = PUSH_WAIT(
		push, room * 5 + 40 +
			      CHAN_FENCE_DWORDS); /* acquires, a start stamp, the copy, the fence */
	if (ret == -ETIMEDOUT)
		tgm_chan_hung(chan, "present: push buffer still full after 1 s", chan->submitted,
			      NSEC_PER_SEC);
	if (ret) {
		mutex_unlock(&present.lock);
		return ret;
	}
	/* what the surface's writers (or, with none found, every user channel) have submitted so far and not completed yet;
	 * with present order, no user submission is kicked from here to this present's kick */
	if (present_order_on())
		mutex_lock(&order_lock);
	mutex_lock(&live_lock);
	list_for_each_entry(user, &live_chans, live) matched |=
		present_writer_on() && !user->kernel && vm_imports(user->vm, src_pa);
	list_for_each_entry(user, &live_chans, live)
	{
		uintptr_t off = (uintptr_t)user->fence_cpu - (uintptr_t)fences.va;
		if (user->kernel || READ_ONCE(user->error) || off >= TGM_FENCE_PAGES * PAGE_SIZE ||
		    n == room || (matched && !vm_imports(user->vm, src_pa)) ||
		    (s32)(tgm_chan_completed(user) - user->submitted) >= 0)
			continue;
		present.wait[n].id = user->id;
		present.wait[n].seqno = READ_ONCE(user->submitted);
		present.wait_end[n] =
			stats_on() ? &chan_stamps(user, present.wait[n].seqno)->end : NULL;
		va = present.fences_gpu + off;
		PUSH_MTHD(push, NV906F, SEMAPHOREA,
			  NVVAL(NV906F, SEMAPHOREA, OFFSET_UPPER, upper_32_bits(va)), SEMAPHOREB,
			  lower_32_bits(va), SEMAPHOREC, present.wait[n].seqno, SEMAPHORED,
			  NVDEF(NV906F, SEMAPHORED, OPERATION, ACQ_GEQ) |
				  NVDEF(NV906F, SEMAPHORED, ACQUIRE_SWITCH, ENABLED));
		n++;
	}
	mutex_unlock(&live_lock);
	present.waits = n;
	if (stats_on()) { /* the copy's start, after the acquires: stats_fences times the copy and the gap before it */
		present.stamped = chan->submitted + 1;
		push_stamp(push,
			   chan_stamps_va(chan, present.stamped) +
				   offsetof(struct tgm_stamps, start),
			   present.stamped);
	}
	if (dst_va) {
		PUSH_NVSQ(push, NVA0B5, NVB0B5_OFFSET_IN_UPPER, upper_32_bits(src_va),
			  NVB0B5_OFFSET_IN_LOWER, lower_32_bits(src_va), NVB0B5_OFFSET_OUT_UPPER,
			  upper_32_bits(dst_va), NVB0B5_OFFSET_OUT_LOWER, lower_32_bits(dst_va),
			  NVB0B5_PITCH_IN, (u32)stride, NVB0B5_PITCH_OUT, 0, NVB0B5_LINE_LENGTH_IN,
			  width * 4, NVB0B5_LINE_COUNT, height);
		PUSH_NVSQ(push, NVA0B5, NVB0B5_SET_DST_BLOCK_SIZE,
			  NVB0B5_BLOCK_SIZE_FERMI_8(block_height_log2), NVB0B5_SET_DST_WIDTH,
			  dst_pitch, NVB0B5_SET_DST_HEIGHT, height, NVB0B5_SET_DST_DEPTH, 1,
			  NVB0B5_SET_DST_LAYER, 0, NVB0B5_SET_DST_ORIGIN, 0);
		PUSH_NVSQ(push, NVA0B5, NVB0B5_LAUNCH_DMA,
			  NVB0B5_LAUNCH_DMA_NON_PIPELINED | NVB0B5_LAUNCH_DMA_FLUSH_ENABLE |
				  NVB0B5_LAUNCH_DMA_SRC_PITCH | NVB0B5_LAUNCH_DMA_DST_BLOCKLINEAR |
				  NVB0B5_LAUNCH_DMA_MULTI_LINE);
	}
	/* set before the kick: the nonstall interrupt can come before chan_fence returns */
	present.done = done;
	present.arg = arg;
	present.queued = nvxnu_nanotime();
	__atomic_store_n(&present.pending, chan->submitted + 1, __ATOMIC_RELEASE);
	nvxnu_work_schedule_after(
		present.deadline_work,
		TGM_PRESENT_NS); /* before the kick, so the completion cancels it */
	present.last = chan_fence(chan);
	present.last_src = src_pa;
	present.last_matched = matched;
	if (matched && stats_on())
		__atomic_fetch_add(&stats.writer, 1, __ATOMIC_RELAXED);
	if (present_order_on()) {
		present.last_idle = !n;
		present.last_late = 0;
		mutex_unlock(&order_lock);
	}
	if (!n && stats_on())
		__atomic_fetch_add(&stats.idle, 1, __ATOMIC_RELAXED);
	mutex_unlock(&present.lock);
	return 0;
}

/* Refused while the gate is shut: the display copies that frame on the CPU. */
int tgm_present_copy(uint64_t src_va, uint64_t src_pa, uint64_t stride, uint32_t width,
		     uint32_t height, uint64_t dst_va, uint32_t dst_pitch,
		     uint32_t block_height_log2, void (*done)(void *), void *arg)
{
	int ret;

	if (!gate_try())
		return -EBUSY;
	ret = present_copy(src_va, src_pa, stride, width, height, dst_va, dst_pitch,
			   block_height_log2, done, arg);
	gate_leave();
	return ret;
}

/* pool scrub: the copy engine zeroes pool pages:
 * The CPU would spend 0.3 s of every boot zeroing the pool at M1 (256 MiB through the write-combined aperture), and a
 * thread call's memset 4.5 ms per 16 MiB of freed pool BO. So its pages start dirty (shim nvxnu_pool_init) and the
 * GPU zeroes them: a kernel channel (CHAN_SCRUB) in an address space of its own, which maps
 * the whole pool once (volatile, at scrub.pool_va), runs MAXWELL_DMA_COPY_A fills of remapped constant 0 (as Mesa's
 * nvk_cmd_fill_memory_ce and M3), each range followed by the kernel fence (chan_fence: WFI, FB flush, L2 flush of
 * dirty lines, then the semaphore release and the nonstall interrupt), so the zeros are in DRAM before the range goes
 * back clean (nvxnu_pool_free_zeroed) from that interrupt's pass. Nothing waits for it on the CPU: until then a range is
 * the scrubber's (allocated), and an allocation takes other pages, zeroing on the CPU only those a free left dirty.
 *  - Boot: once M3 passed, the scrubber's work item claims the free dirty pages (shim nvxnu_pool_claim_dirty) a run of at
 *    most SCRUB_CHUNK at a time, with at most SCRUB_INFLIGHT bytes queued, and claims more as ranges complete, so it
 *    never holds more than that of the pool from an allocation.
 *  - A freed pool BO (TegraGM20BUserClient.cpp poolZeroLater): tgm_pool_scrub queues its range.
 * The CPU has no cacheable view of the pool, so the zeros need no cache maintenance. A range not back SCRUB_NS after it
 * was queued panics with one line, as a present does: its nonstall interrupt never came, or the copy never ran. */
#define SCRUB_JOBS 64
#define SCRUB_CHUNK (16UL << 20)
#define SCRUB_INFLIGHT (48UL << 20)
#define SCRUB_NS (1000 * NSEC_PER_MSEC)
#define SCRUB_LINE \
	(128U << 10) /* bytes per line of a fill: 32768 four-byte elements, Mesa's max_dim */

static struct {
	struct mutex lock; /* submissions: the ring's head, the channel's push buffer */
	struct tgm_vm *vm;
	struct tgm_chan *chan;
	struct tgm_mem *pool_mem, *fence_mem;
	u64 *pool_pas;
	u64 pool_va, fence_gpu, fence_pa;
	volatile u32 *fence;
	void *work, *deadline;
	struct {
		u64 pa, bytes, queued;
		u32 seqno;
	} job[SCRUB_JOBS];
	u32 head,
		tail; /* job[tail % SCRUB_JOBS .. head) queued; head moves under lock, tail in scrub_fenced only */
	u64 inflight;  /* bytes queued (atomic) */
	int boot_done; /* the boot scrub found nothing left to claim */
	u64 boot_start, boot_bytes;
	/* tegra-gpu-stats: this window's ranges zeroed, their bytes and queue-to-done time (ns) */
	u64 n, bytes, ns, max_ns;
} scrub = {.lock = {NULL}};

/* One range: copy-engine fills, then the fence. Under scrub.lock, with a free job slot. 0 or a push error. */
static int scrub_submit(u64 pa, u64 bytes)
{
	struct tgm_chan *chan = scrub.chan;
	struct nvif_push *push = &chan->chan.push;
	u64 va = scrub.pool_va + (pa - nvxnu_pool_base_pa()), lines = bytes / SCRUB_LINE,
	    rest = bytes % SCRUB_LINE;
	u32 slot, seqno;
	int ret = nvif_chan_gpfifo_wait(&chan->chan, 4, 32 + CHAN_FENCE_DWORDS);

	if (ret)
		return ret;
	PUSH_MTHD(push, NVA0B5, SET_REMAP_CONST_A, 0);
	PUSH_MTHD(push, NVA0B5, SET_REMAP_COMPONENTS,
		  NVDEF(NVA0B5, SET_REMAP_COMPONENTS, DST_X, CONST_A) |
			  NVDEF(NVA0B5, SET_REMAP_COMPONENTS, COMPONENT_SIZE, FOUR) |
			  NVDEF(NVA0B5, SET_REMAP_COMPONENTS, NUM_SRC_COMPONENTS, ONE) |
			  NVDEF(NVA0B5, SET_REMAP_COMPONENTS, NUM_DST_COMPONENTS, ONE));
	PUSH_MTHD(push, NVA0B5, PITCH_IN, SCRUB_LINE, PITCH_OUT, SCRUB_LINE);
	if (lines) { /* whole 128 KiB lines, one multi-line fill */
		PUSH_MTHD(push, NVA0B5, OFFSET_OUT_UPPER, upper_32_bits(va), OFFSET_OUT_LOWER,
			  lower_32_bits(va));
		PUSH_MTHD(push, NVA0B5, LINE_LENGTH_IN, SCRUB_LINE / 4, LINE_COUNT, (u32)lines);
		PUSH_MTHD(push, NVA0B5, LAUNCH_DMA,
			  NVDEF(NVA0B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, NON_PIPELINED) |
				  NVDEF(NVA0B5, LAUNCH_DMA, FLUSH_ENABLE, TRUE) |
				  NVDEF(NVA0B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, PITCH) |
				  NVDEF(NVA0B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH) |
				  NVDEF(NVA0B5, LAUNCH_DMA, MULTI_LINE_ENABLE, TRUE) |
				  NVDEF(NVA0B5, LAUNCH_DMA, REMAP_ENABLE, TRUE));
	}
	if (rest) { /* the last pages, one line (a multiple of 4 KiB) */
		va += lines * SCRUB_LINE;
		PUSH_MTHD(push, NVA0B5, OFFSET_OUT_UPPER, upper_32_bits(va), OFFSET_OUT_LOWER,
			  lower_32_bits(va));
		PUSH_MTHD(push, NVA0B5, LINE_LENGTH_IN, (u32)(rest / 4), LINE_COUNT, 1);
		PUSH_MTHD(push, NVA0B5, LAUNCH_DMA,
			  NVDEF(NVA0B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, NON_PIPELINED) |
				  NVDEF(NVA0B5, LAUNCH_DMA, FLUSH_ENABLE, TRUE) |
				  NVDEF(NVA0B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, PITCH) |
				  NVDEF(NVA0B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH) |
				  NVDEF(NVA0B5, LAUNCH_DMA, REMAP_ENABLE, TRUE));
	}
	slot = scrub.head % SCRUB_JOBS;
	scrub.job[slot].pa = pa;
	scrub.job[slot].bytes = bytes;
	scrub.job[slot].queued = nvxnu_nanotime();
	scrub.job[slot].seqno = seqno = chan->submitted + 1;
	__atomic_fetch_add(&scrub.inflight, bytes, __ATOMIC_RELAXED);
	/* the job is visible to scrub_fenced before its fence can complete */
	__atomic_store_n(&scrub.head, scrub.head + 1, __ATOMIC_RELEASE);
	if (chan_fence(chan) != seqno)
		nvxnu_panic("tegra-gm20b: pool scrub fence %u is not the job's %u", chan->submitted,
			    seqno);
	nvxnu_work_schedule_after(scrub.deadline, SCRUB_NS);
	return 0;
}

/* Room for one more range: a free job slot (the boot's claims also keep to SCRUB_INFLIGHT). Under scrub.lock. */
static int scrub_room(void)
{
	return scrub.head - __atomic_load_n(&scrub.tail, __ATOMIC_ACQUIRE) < SCRUB_JOBS;
}

/* The work item: claim and queue the free pages not known to be zero, as long as there is room (see above). */
static void scrub_work(void *arg)
{
	u64 pa;
	unsigned long bytes;
	(void)arg;
	if (!gate_try())
		return; /* the free dirty pages stay as they are: the wake kicks this again */
	mutex_lock(&scrub.lock);
	while (scrub.chan && !READ_ONCE(scrub.chan->error) && scrub_room() &&
	       __atomic_load_n(&scrub.inflight, __ATOMIC_RELAXED) + SCRUB_CHUNK <= SCRUB_INFLIGHT) {
		bytes = nvxnu_pool_claim_dirty(SCRUB_CHUNK, (unsigned long long *)&pa);
		if (!bytes) {
			if (!scrub.boot_done) {
				scrub.boot_done = 1;
				nvxnu_log("tegra-gm20b: pool scrub: %llu MiB queued to the copy "
					  "engine %llu ms after M3 (the CPU cleared it at M1 "
					  "before)\n",
					  scrub.boot_bytes >> 20,
					  (nvxnu_nanotime() - scrub.boot_start) / NSEC_PER_MSEC);
			}
			break;
		}
		if (scrub_submit(pa, bytes)) {
			/* back as it was, dirty (the CPU zeroes it if taken), without kicking this work again */
			dma_free_attrs(&gpu_pdev.dev, bytes, NULL, pa, 0);
			break;
		}
		if (!scrub.boot_done)
			scrub.boot_bytes += bytes;
	}
	mutex_unlock(&scrub.lock);
	gate_leave();
}

/* Pages went back dirty: the work item claims and queues them (any context; nothing before the scrubber is up). */
static void scrub_kick(void)
{
	if (READ_ONCE(scrub.chan))
		nvxnu_work_schedule(scrub.work);
}

/* From the scrub channel's nonstall interrupt: every range whose fence passed goes back clean; then more claims. */
static void scrub_fenced(struct tgm_chan *chan)
{
	u32 done = tgm_chan_completed(chan), tail = scrub.tail,
	    head = __atomic_load_n(&scrub.head, __ATOMIC_ACQUIRE);
	u64 now = nvxnu_nanotime(), took;
	int freed = 0;

	while (tail != head && (s32)(done - scrub.job[tail % SCRUB_JOBS].seqno) >= 0) {
		u32 slot = tail % SCRUB_JOBS;
		nvxnu_pool_free_zeroed(scrub.job[slot].pa, scrub.job[slot].bytes);
		__atomic_fetch_sub(&scrub.inflight, scrub.job[slot].bytes, __ATOMIC_RELAXED);
		took = now - scrub.job[slot].queued;
		__atomic_fetch_add(&scrub.n, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&scrub.bytes, scrub.job[slot].bytes, __ATOMIC_RELAXED);
		stats_add(&scrub.ns, &scrub.max_ns, took);
		__atomic_store_n(&scrub.tail, ++tail, __ATOMIC_RELEASE);
		freed = 1;
	}
	if (tail == head)
		nvxnu_work_cancel(scrub.deadline);
	if (freed && nvxnu_pool_dirty_free_pages())
		nvxnu_work_schedule(scrub.work);
}

/* A range not back SCRUB_NS after it was queued: one line and a panic. */
static void scrub_deadline(void *arg)
{
	u32 tail = __atomic_load_n(&scrub.tail, __ATOMIC_ACQUIRE),
	    head = __atomic_load_n(&scrub.head, __ATOMIC_ACQUIRE), slot = tail % SCRUB_JOBS;
	u64 age;
	char state[320];

	(void)arg;
	if (tail == head)
		return;
	age = nvxnu_nanotime() - scrub.job[slot].queued;
	if (age < SCRUB_NS) {
		nvxnu_work_schedule_after(scrub.deadline, SCRUB_NS - age);
		return;
	}
	if ((s32)(tgm_chan_completed(scrub.chan) - scrub.job[slot].seqno) >= 0)
		nvxnu_panic(
			"tegra-gm20b: pool scrub fence %u complete after %llu ms but its nonstall "
			"interrupt never came",
			scrub.job[slot].seqno, age / 1000000);
	chan_describe(scrub.chan, state, sizeof(state));
	nvxnu_panic("tegra-gm20b: pool scrub of %llu KiB at %#llx not done %llu ms after it was "
		    "queued: scrub %s",
		    scrub.job[slot].bytes >> 10, scrub.job[slot].pa, age / 1000000, state);
}

/* After M3: the scrubber's address space (the whole pool mapped once), fence page and channel; then the boot scrub. One
 * line and no scrubber if any of it fails: pool pages are then zeroed by the CPU as they are taken. */
static void scrub_open(void)
{
	u64 npages = nvxnu_pool_size() >> PAGE_SHIFT, i;
	struct tgm_chan *chan = NULL;
	void *fence_va;
	int ret;

	mutex_init(&scrub.lock);
	scrub.boot_start = nvxnu_nanotime();
	scrub.work = nvxnu_work_alloc(scrub_work, NULL);
	scrub.deadline = nvxnu_work_alloc(scrub_deadline, NULL);
	scrub.pool_pas =
		kcalloc(npages, sizeof(u64), GFP_KERNEL); /* nvkm keeps the array (mem.c) */
	fence_va = tgm_pool_alloc(PAGE_SIZE, &scrub.fence_pa);
	ret = scrub.work && scrub.deadline && scrub.pool_pas && fence_va ? tgm_vm_new(&scrub.vm)
									 : -ENOMEM;
	for (i = 0; !ret && i < npages; i++)
		scrub.pool_pas[i] = nvxnu_pool_base_pa() + (i << PAGE_SHIFT);
	if (!ret)
		ret = tgm_mem_new(scrub.vm, scrub.pool_pas, (u32)npages, &scrub.pool_mem);
	if (!ret)
		ret = tgm_vm_kmap(scrub.vm, scrub.pool_mem, npages << PAGE_SHIFT, &scrub.pool_va);
	if (!ret)
		ret = tgm_mem_new(scrub.vm, &scrub.fence_pa, 1, &scrub.fence_mem);
	if (!ret)
		ret = tgm_vm_kmap(scrub.vm, scrub.fence_mem, PAGE_SIZE, &scrub.fence_gpu);
	scrub.fence = fence_va;
	if (!ret)
		ret = tgm_chan_new(scrub.vm, scrub.fence, scrub.fence_gpu, "scrub", 1, &chan);
	if (ret) {
		/* no teardown: what was made stays unused (a few pages); allocations zero on the CPU */
		nvxnu_log("tegra-gm20b: pool scrub unavailable (%d): pool pages are zeroed by the "
			  "CPU "
			  "as they are taken\n",
			  ret);
		return;
	}
	chan->kernel =
		CHAN_SCRUB; /* before anyone can submit on it: its nonstall goes to scrub_fenced */
	__atomic_store_n(&scrub.chan, chan, __ATOMIC_RELEASE);
	nvxnu_work_schedule(scrub.work);
}

int tgm_pool_scrub(uint64_t pa, uint64_t bytes)
{
	int ret;
	if (!__atomic_load_n(&scrub.chan, __ATOMIC_ACQUIRE))
		return -ENODEV;
	if ((pa | bytes) & (PAGE_SIZE - 1) || !bytes || pa < nvxnu_pool_base_pa() ||
	    pa + bytes > nvxnu_pool_base_pa() + nvxnu_pool_size())
		return -EINVAL;
	if (!gate_try())
		return -EBUSY; /* the caller zeroes it on the CPU */
	mutex_lock(&scrub.lock);
	if (!READ_ONCE(scrub.chan->error) && scrub_room())
		ret = scrub_submit(pa, bytes);
	else
		ret = -EBUSY;
	mutex_unlock(&scrub.lock);
	gate_leave();
	return ret;
}

/* tegra-gpu-stats: this window's scrubbed ranges for the stats2 line */
static void scrub_stats(char *buf, unsigned int len, u64 window_ms)
{
	u64 n = __atomic_exchange_n(&scrub.n, 0, __ATOMIC_RELAXED),
	    bytes = __atomic_exchange_n(&scrub.bytes, 0, __ATOMIC_RELAXED),
	    ns = __atomic_exchange_n(&scrub.ns, 0, __ATOMIC_RELAXED),
	    max = __atomic_exchange_n(&scrub.max_ns, 0, __ATOMIC_RELAXED);
	(void)window_ms;
	scnprintf(buf, len,
		  "scrub %llu (%llu MiB, queued to done mean %llu us max %llu us), %lu KiB dirty",
		  n, bytes >> 20, n ? ns / n / 1000 : 0, max / 1000,
		  nvxnu_pool_dirty_free_pages() << (PAGE_SHIFT - 10));
}

static int m3(char *line, unsigned int len)
{
	struct tgm_vm *vm = NULL;
	struct tgm_chan *chan = NULL;
	struct tgm_mem *mem = NULL;
	u64 pa, pas[2], va = 0, fill = 0, end;
	u32 *page, seqno = 0, bad = 0, i;
	struct nvif_push *push;
	int ret, user = 0;

	/* page 0: fence word at +0, page 1: fill target. With tegra-gpu-l2=1 the copy engine fills page 1 through a user
	 * (L2-cacheable) mapping, so the check below also proves the fence's L2 flush. */
	page = tgm_pool_alloc(2 * PAGE_SIZE, &pa);
	if (!page)
		return -ENOMEM;
	pas[0] = pa;
	pas[1] = pa + PAGE_SIZE;
	ret = tgm_vm_new(&vm);
	if (!ret)
		ret = tgm_mem_new(vm, pas, 2, &mem);
	if (!ret)
		ret = tgm_vm_kmap(vm, mem, 2 * PAGE_SIZE, &va);
	fill = va + PAGE_SIZE;
	if (!ret && l2_on()) {
		fill = 0x100000;
		ret = tgm_vm_map(vm, fill, PAGE_SIZE, mem, PAGE_SIZE, 0, 0);
		user = !ret;
	}
	if (!ret)
		ret = tgm_chan_new(vm, (volatile u32 *)page, va, "self-test", 1, &chan);
	if (ret)
		goto done;

	/* MAXWELL_DMA_COPY_A on subchannel 4: remap const A (0xc0ffee42) over 1024 dwords of page 1 */
	push = &chan->chan.push;
	ret = PUSH_WAIT(push, 12 + CHAN_FENCE_DWORDS);
	if (ret)
		goto done;
	PUSH_MTHD(push, NVA0B5, SET_REMAP_CONST_A, 0xc0ffee42);
	PUSH_MTHD(push, NVA0B5, SET_REMAP_COMPONENTS,
		  NVDEF(NVA0B5, SET_REMAP_COMPONENTS, DST_X, CONST_A) |
			  NVDEF(NVA0B5, SET_REMAP_COMPONENTS, COMPONENT_SIZE, FOUR) |
			  NVDEF(NVA0B5, SET_REMAP_COMPONENTS, NUM_SRC_COMPONENTS, ONE) |
			  NVDEF(NVA0B5, SET_REMAP_COMPONENTS, NUM_DST_COMPONENTS, ONE));
	PUSH_MTHD(push, NVA0B5, OFFSET_OUT_UPPER, upper_32_bits(fill), OFFSET_OUT_LOWER,
		  lower_32_bits(fill));
	PUSH_MTHD(push, NVA0B5, LINE_LENGTH_IN, PAGE_SIZE / 4, LINE_COUNT, 1);
	PUSH_MTHD(push, NVA0B5, LAUNCH_DMA,
		  NVDEF(NVA0B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, NON_PIPELINED) |
			  NVDEF(NVA0B5, LAUNCH_DMA, FLUSH_ENABLE, TRUE) |
			  NVDEF(NVA0B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, PITCH) |
			  NVDEF(NVA0B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH) |
			  NVDEF(NVA0B5, LAUNCH_DMA, REMAP_ENABLE, TRUE));
	seqno = chan_fence(chan);
	end = nvxnu_nanotime() + NSEC_PER_SEC;
	for (;;) {
		u32 seq = nvxnu_wake_seq();
		if (READ_ONCE(page[0]) == seqno || chan->error || nvxnu_nanotime() >= end)
			break;
		if (!nvxnu_idle(seq, end) && READ_ONCE(page[0]) == seqno)
			nvxnu_wake_due(seq, "fence", seqno);
	}
	for (i = 0; i < PAGE_SIZE / 4; i++)
		bad += page[PAGE_SIZE / 4 + i] != 0xc0ffee42;
	ret = READ_ONCE(page[0]) != seqno ? -ETIMEDOUT : bad ? -EIO : 0;
done:
	scnprintf(line, len, "M3 %s vm=%s chan=%s fence=%u/%u ce_fill_bad=%u/1024%s err=%u ret=%d",
		  ret ? "FAIL" : "ok", vm ? "ok" : "no", chan ? "ok" : "no", page ? page[0] : 0,
		  seqno, bad, l2_on() ? " (L2-cached target)" : "", chan ? chan->error : 0, ret);
	tgm_chan_del(&chan);
	if (user)
		tgm_vm_unmap(vm, fill, PAGE_SIZE);
	tgm_mem_del(&mem);
	tgm_vm_del(&vm);
	tgm_pool_free(page, 2 * PAGE_SIZE, pa);
	return ret;
}

/* fault test (tegra-gpu-fault-test=1):
 * With tegra-gpu-all-clients=1, after M3: proves client fault isolation on this GPU before apps rely on it. A test
 * channel (not critical) writes through GR's inline-to-memory unit (KEPLER_INLINE_TO_MEMORY_B on subchannel 2; methods
 * from Mesa's cla140.h) to an address its VM does not map: an MMU fault on GR, which nvkm recovers by killing the
 * channel and resetting GR (gf100_gr_reset: FECS/GPCCS booted again through ACR). Then a
 * second channel writes a word to a mapped page the same way and fences it: GR came back. One line with the outcome.
 * A recovery that does not finish panics as for any client (iso_deadline). */
#define PUSH906F_SUBC_NVA140 2
#define NVA140_LINE_LENGTH_IN 0x0180
#define NVA140_LINE_COUNT 0x0184
#define NVA140_OFFSET_OUT_UPPER 0x0188
#define NVA140_OFFSET_OUT 0x018c
#define NVA140_LAUNCH_DMA 0x01b0
#define NVA140_LAUNCH_DMA_DST_PITCH 0x00000001
#define NVA140_LOAD_INLINE_DATA 0x01b4
#define FAULT_TEST_VA 0x200000000ULL /* never bound in the test VM */

static void push_i2m(struct tgm_chan *chan, u64 va, u32 data)
{
	struct nvif_push *push = &chan->chan.push;

	PUSH_NVSQ(push, NVA140, NVA140_LINE_LENGTH_IN, 4, NVA140_LINE_COUNT, 1,
		  NVA140_OFFSET_OUT_UPPER, upper_32_bits(va), NVA140_OFFSET_OUT, lower_32_bits(va));
	PUSH_NVSQ(push, NVA140, NVA140_LAUNCH_DMA, NVA140_LAUNCH_DMA_DST_PITCH);
	PUSH_NVSQ(push, NVA140, NVA140_LOAD_INLINE_DATA, data);
}

/* Sleeps until done(chan) or `ms` passed; returns the ms waited. */
static u64 fault_test_wait(struct tgm_chan *chan, int (*done)(struct tgm_chan *), u32 ms)
{
	u64 start = nvxnu_nanotime(), end = start + (u64)ms * NSEC_PER_MSEC;
	for (;;) {
		u32 seq = nvxnu_wake_seq();
		if (done(chan) || nvxnu_nanotime() >= end)
			break;
		nvxnu_idle(seq, end);
	}
	return (nvxnu_nanotime() - start) / NSEC_PER_MSEC;
}

static int chan_dead(struct tgm_chan *chan)
{
	return READ_ONCE(chan->error) != 0;
}
static int chan_idle(struct tgm_chan *chan)
{
	return (s32)(tgm_chan_completed(chan) - chan->submitted) >= 0;
}

static void fault_test(void)
{
	struct tgm_vm *vm = NULL;
	struct tgm_mem *mem = NULL;
	struct tgm_chan *bad = NULL, *good = NULL;
	u64 pa, pas[2], va = 0, killed_ms = 0, ok_ms = 0;
	u32 *page, word = 0;
	long long v = 0;
	int ret;

	if (!nvxnu_bootarg_int("tegra-gpu-fault-test", &v) || !v)
		return;
	if (!tgm_all_clients() || !iso.deadline) {
		nvxnu_log("tegra-gm20b: fault test skipped: needs tegra-gpu-all-clients=1\n");
		return;
	}
	page = tgm_pool_alloc(2 * PAGE_SIZE, &pa);
	if (!page)
		return;
	pas[0] = pa;
	pas[1] = pa + PAGE_SIZE;
	ret = tgm_vm_new(&vm);
	if (!ret)
		ret = tgm_mem_new(vm, pas, 2, &mem);
	if (!ret)
		ret = tgm_vm_kmap(vm, mem, 2 * PAGE_SIZE, &va);
	if (!ret)
		ret = tgm_chan_new(vm, (volatile u32 *)page, va, "fault-test", 0, &bad);
	if (!ret)
		ret = PUSH_WAIT(&bad->chan.push, 16 + CHAN_FENCE_DWORDS);
	if (!ret) {
		push_i2m(bad, FAULT_TEST_VA, 0xdeadbeef);
		chan_fence(bad);
		killed_ms = fault_test_wait(bad, chan_dead, 1000);
		ret = chan_dead(bad) ? 0 : -ETIMEDOUT;
	}
	if (!ret)
		ret = tgm_chan_new(vm, (volatile u32 *)page + 4, va + 16, "fault-test", 0, &good);
	if (!ret)
		ret = PUSH_WAIT(&good->chan.push, 16 + CHAN_FENCE_DWORDS);
	if (!ret) {
		push_i2m(good, va + PAGE_SIZE, 0xc0ffee43);
		chan_fence(good);
		ok_ms = fault_test_wait(good, chan_idle, 2000);
		word = READ_ONCE(page[PAGE_SIZE / 4]);
	}
	nvxnu_log("tegra-gm20b: fault test %s: MMU fault on a client channel %s (%llu ms, error "
		  "%u); GR afterwards: I2M word "
		  "%08x (want c0ffee43), fence %u/%u in %llu ms, ret %d\n",
		  !ret && word == 0xc0ffee43 && good && chan_idle(good) && !good->error ? "ok"
											: "FAIL",
		  bad && chan_dead(bad) ? "killed it" : "did NOT kill it", killed_ms,
		  bad ? bad->error : 0, word, good ? tgm_chan_completed(good) : 0,
		  good ? good->submitted : 0, ok_ms, ret);
	tgm_chan_del(&good);
	tgm_chan_del(&bad);
	tgm_mem_del(&mem);
	tgm_vm_del(&vm);
	tgm_pool_free(page, 2 * PAGE_SIZE, pa);
}

/* system sleep (TegraGM20BSleep.h):
 * SC7 powers the SoC off with DRAM in self-refresh: the GPU's registers, falcons, L2 and PTIMER are lost; its memory is
 * kept (pool and nvkm instmem pages: instance blocks, RAMFC, page tables, runlists, GR context buffers, ACR's blob, rings,
 * fence pages). Every nvif object is in the one client m2() made (the user client's connections, the present and the
 * scrubber are its vmm, mem and channel objects), so nouveau's own suspend and resume (nouveau_do_suspend/_resume: the
 * channels idle, nvif_client_suspend, nvif_client_resume) take all of them. Around them:
 *  sleep  the gate shut (submissions and new channels wait for the wake, the present and the scrubber are refused), every
 *         channel's last fence (interrupt-driven, TGM_HANG_NS, else tgm_chan_hung), gpu.lock taken and held to the wake
 *         (every other nvif call waits), the idle clock off, nvif_client_suspend (each GR context unbound after a
 *         preempt that saves it, each channel blocked, preempted and off its runlist, events blocked), the L2's dirty
 *         lines written back (the GPU writes RAMFC and contexts through it: nvkm's gk20a instmem flushes it before every
 *         CPU look, instmem/gk20a.c), nvkm_device_fini(NVKM_SUSPEND) (what nvkm_udevice_fini would run if m2() held no
 *         device reference), interrupts off, the MC client drained, then nvkm_device_tegra_power_down()'s steps.
 *  wake   nvkm_device_tegra_power_up()'s steps, interrupts on, then the boot's order: M1's PRI ring, timer and GPCPLL,
 *         clk_raise() (the relock under M1's quiet conditions, before any falcon runs), m2_device_init() (ACR's HS load
 *         again, which checks the GPU still reports the WPR it was built for; the PMU; every engine with a user, GR's
 *         FECS/GPCCS through the PMU), nvif_client_resume (channels bound and back on their runlists, GR contexts
 *         rebound, events allowed), one fence on a kernel channel, the idle clock, gpu.lock, the gate. */
static struct {
	enum tgm_sleep_plan plan; /* the last sleep's, until its wake */
	int khz; /* the GPC clock before sleep: its run rate, also when the idle clock had it low */
	int idle_on;
} sc7;

static u64 us_since(u64 start)
{
	return (nvxnu_nanotime() - start) / 1000;
}

static void sleep_gate(void)
{
	u64 start = nvxnu_nanotime(), end = start + TGM_HANG_NS;
	u32 inside;

	mutex_lock(&gate_lock);
	tgm_gate_close(&sc7_gate);
	inside = __atomic_load_n(&sc7_gate.inside, __ATOMIC_SEQ_CST);
	while (!tgm_gate_drained(&sc7_gate)) {
		if (!nvxnu_mutex_sleep(gate_lock.lck, &sc7_gate.inside, end) &&
		    !tgm_gate_drained(&sc7_gate))
			nvxnu_panic("tegra-gm20b: sleep: %u callers still in the GPU %llu ms after "
				    "its gate shut",
				    __atomic_load_n(&sc7_gate.inside, __ATOMIC_SEQ_CST),
				    (unsigned long long)(TGM_HANG_NS / 1000000));
	}
	mutex_unlock(&gate_lock);
	tgm_journal("tegra-gm20b: sleep: submissions held (%u were in the driver): %llu us", inside,
		    us_since(start));
}

static void sleep_fences(void)
{
	struct tgm_chan *chan;
	u64 start = nvxnu_nanotime(), end = start + TGM_HANG_NS, id = 0;
	u32 seqno = 0, chans, waits = 0;
	int busy;

	for (;;) {
		u32 seq = nvxnu_wake_seq();

		busy = 0;
		chans = 0;
		mutex_lock(&live_lock);
		list_for_each_entry(chan, &live_chans, live)
		{
			chans++;
			if (busy || READ_ONCE(chan->error) ||
			    (s32)(tgm_chan_completed(chan) - chan->submitted) >= 0)
				continue;
			if (nvxnu_nanotime() >= end)
				tgm_chan_hung(chan, "system sleep", chan->submitted,
					      nvxnu_nanotime() - start);
			busy = 1;
			id = chan->id;
			seqno = chan->submitted;
		}
		mutex_unlock(&live_lock);
		if (!busy)
			break;
		waits++;
		if (!nvxnu_idle(seq, end) && sync_done(id, seqno))
			nvxnu_wake_due(seq, "fence", seqno);
	}
	tgm_journal("tegra-gm20b: sleep: %u channels idle (%u fence waits): %llu us", chans, waits,
		    us_since(start));
}

static void sleep_idle_off(void)
{
	sc7.khz = nvkm_clk_read(gpu.device->clk, nv_clk_src_gpc);
	sc7.idle_on = 0;
	if (!idle.clk)
		return;
	nvxnu_mutex_lock(idle.clk);
	sc7.idle_on = READ_ONCE(idle.on);
	WRITE_ONCE(idle.on, 0);
	if (READ_ONCE(idle.low)) {
		idle.low_ns += nvxnu_nanotime() - idle.low_since;
		WRITE_ONCE(idle.low, 0);
		sc7.khz = (int)idle.run_khz;
	}
	nvxnu_mutex_unlock(idle.clk);
}

static void sleep_nvkm(void)
{
	struct nvkm_device *device = gpu.device;
	u64 start = nvxnu_nanotime();
	int ret = nvif_client_suspend(&gpu.client, false);

	if (ret)
		nvxnu_panic("tegra-gm20b: sleep: nvkm object suspend failed: %d", ret);
	tgm_journal("tegra-gm20b: sleep: nvkm objects suspended (GR contexts saved, channels off "
		    "the runlist): %llu us",
		    us_since(start));
	start = nvxnu_nanotime();
	nvkm_ltc_flush(device->ltc);
	if (nvkm_rd32(device, 0x070010) & 0x3) /* FLUSH_L2_FLUSH_DIRTY: 0 pending, 1 outstanding */
		nvxnu_panic("tegra-gm20b: sleep: L2 flush still busy after 2 s (%08x)",
			    nvkm_rd32(device, 0x070010));
	ret = nvkm_device_fini(device, NVKM_SUSPEND);
	if (ret)
		nvxnu_panic("tegra-gm20b: sleep: nvkm device suspend failed: %d", ret);
	tgm_journal("tegra-gm20b: sleep: L2 written back, nvkm device suspended at %d kHz: %llu us",
		    sc7.khz, us_since(start));
}

static void sleep_power_down(void)
{
	struct nvkm_device_tegra *tdev = gpu.device->func->tegra(gpu.device);
	u64 start = nvxnu_nanotime();
	int drained, ret;

	nvxnu_irq_suspend();
	drained = tegra_gm20b_mc_flush(1); /* nvgpu's railgate order: the client drained first */
	clk_disable_unprepare(tdev->clk_pwr);
	clk_disable_unprepare(tdev->clk_ref);
	clk_disable_unprepare(tdev->clk);
	udelay(10);
	ret = tdev->vdd ? regulator_disable(tdev->vdd) : 0;
	if (drained)
		nvxnu_log("tegra-gm20b: sleep: the MC's GPU client did not drain (%d)\n", drained);
	if (ret)
		nvxnu_log("tegra-gm20b: sleep: VDD_GPU may still be on (MAX77812 %d)\n", ret);
	tgm_journal("tegra-gm20b: sleep: interrupts off, MC client %s, clocks off, VDD_GPU %s: "
		    "%llu us",
		    drained ? "NOT drained" : "drained", ret ? "MAYBE STILL ON" : "off",
		    us_since(start));
}

/* nvkm_device_tegra_power_up() (engine/device/tegra.c, static there): what M1's nvkm_device_tegra_new() ran. */
static int power_up(struct nvkm_device_tegra *tdev)
{
	int ret = tdev->vdd ? regulator_enable(tdev->vdd) : 0;

	if (!ret)
		ret = clk_prepare_enable(tdev->clk);
	if (!ret)
		ret = clk_prepare_enable(tdev->clk_ref);
	if (!ret)
		ret = clk_prepare_enable(tdev->clk_pwr);
	if (ret)
		return ret;
	clk_set_rate(tdev->clk_pwr, 204000000);
	udelay(10);
	if (!tdev->pdev->dev.pm_domain) {
		reset_control_assert(tdev->rst);
		udelay(10);
		ret = tegra_pmc_powergate_remove_clamping(tdev->pmc, TEGRA_POWERGATE_3D);
		if (ret)
			return ret;
		udelay(10);
		reset_control_deassert(tdev->rst);
		udelay(10);
	}
	return 0;
}

static void wake_power_up(void)
{
	u64 start = nvxnu_nanotime();
	int ret = power_up(gpu.device->func->tegra(gpu.device)), uv = 0;

	if (ret)
		nvxnu_panic("tegra-gm20b: wake: GPU power-up failed: %d (VDD_GPU, clocks, clamp)",
			    ret);
	nvxnu_irq_resume();
	tegra_gm20b_rail_get_uv(&uv);
	tgm_journal("tegra-gm20b: wake: VDD_GPU %d uV, clocks on, out of reset and clamp: %llu us",
		    uv, us_since(start));
}

/* M1 after nvkm_device_tegra_new() */
static void wake_m1(void)
{
	struct nvkm_device *device = gpu.device;
	u64 start = nvxnu_nanotime();
	u32 boot0 = nvkm_rd32(device, 0x000000), ring;
	int ret;

	if (boot0 != gpu.boot0)
		nvxnu_panic("tegra-gm20b: wake: BOOT_0 reads %08x after the power-up, %08x before "
			    "sleep",
			    boot0, gpu.boot0);
	ret = nvkm_subdev_init(device->privring);
	if (!ret)
		ret = nvkm_subdev_init(&device->timer->subdev);
	ring = nvkm_rd32(device,
			 0x120050); /* PRI_RINGMASTER_START_RESULTS, bit 0 = connectivity pass */
	if (!ret)
		ret = nvkm_subdev_init(&device->clk->subdev);
	if (!ret)
		nvkm_mask(device, 0x137250, 0x0000003f, 0x00000000); /* M1's 1:1 bypass divider */
	if (ret || badf(ring) || !(ring & 1))
		nvxnu_panic("tegra-gm20b: wake: PRI ring %08x, timer or GPCPLL init failed: %d",
			    ring, ret);
	tgm_journal("tegra-gm20b: wake: BOOT_0 %08x, PRI ring %08x, timer, GPCPLL_CFG %08x at %d "
		    "kHz: %llu us",
		    boot0, ring, nvkm_rd32(device, 0x137000),
		    nvkm_clk_read(device->clk, nv_clk_src_gpc), us_since(start));
}

static void wake_clock(void)
{
	struct nvkm_device *device = gpu.device;
	u64 start = nvxnu_nanotime();
	char line[200] = "";
	int ret = clk_raise(line, sizeof(line)), khz = nvkm_clk_read(device->clk, nv_clk_src_gpc);

	if (ret || khz < sc7.khz)
		nvxnu_panic("tegra-gm20b: wake: GPU clock %d kHz, %d kHz before sleep (GPCPLL_CFG "
			    "%08x COEFF %08x)%s%s",
			    khz, sc7.khz, nvkm_rd32(device, 0x137000), nvkm_rd32(device, 0x137004),
			    line[0] ? ": " : "", line);
	tgm_journal("tegra-gm20b: wake: GPU clock %d kHz (GPCPLL COEFF %08x): %llu us", khz,
		    nvkm_rd32(device, 0x137004), us_since(start));
}

static void wake_device(void)
{
	struct nvkm_device *device = gpu.device;
	struct nvkm_acr *acr = device->acr;
	u64 start = nvxnu_nanotime(), lo, hi;
	const char *at = "";
	u32 mc[5];
	int ret = m2_device_init(device, &at);

	if (ret || !acr || !acr->done) {
		/* gm200_acr_wpr_check(): the WPR the GPU reports now; ACR built its image for acr->wpr_start/_end */
		nvkm_wr32(device, 0x100cd4, 2);
		lo = (u64)(nvkm_rd32(device, 0x100cd4) & 0xffffff00) << 8;
		nvkm_wr32(device, 0x100cd4, 3);
		hi = ((u64)(nvkm_rd32(device, 0x100cd4) & 0xffffff00) << 8) + 0x20000;
		tegra_gm20b_mc_gpu_wpr(mc);
		nvxnu_panic(
			"tegra-gm20b: wake: nvkm init failed at %s: %d, ACR %s; the GPU reports "
			"WPR %llx-%llx, ACR's image is for %llx-%llx; MC carveout 2 CFG0 %08x "
			"BOM %08x SIZE %u x 128 KiB CLIENT_ACCESS2 %08x CLIENT_ACCESS4 %08x",
			at, ret, acr && acr->done ? "loaded" : "NOT loaded", lo, hi,
			acr ? acr->wpr_start : 0, acr ? acr->wpr_end : 0, mc[0], mc[1], mc[2],
			mc[3], mc[4]);
	}
	tgm_journal("tegra-gm20b: wake: nvkm device up (ACR loaded, WPR %llx-%llx; PMU; GR "
		    "FECS/GPCCS): %llu us",
		    acr->wpr_start, acr->wpr_end, us_since(start));
}

static void wake_nvkm(void)
{
	struct tgm_chan *chan;
	u64 start = nvxnu_nanotime();
	u32 chans = 0, dead = 0;
	int ret = nvif_client_resume(&gpu.client);

	if (ret)
		nvxnu_panic("tegra-gm20b: wake: nvkm object resume failed: %d", ret);
	mutex_lock(&live_lock);
	list_for_each_entry(chan, &live_chans, live)
	{
		chans++;
		dead += READ_ONCE(chan->error) != 0;
	}
	mutex_unlock(&live_lock);
	tgm_journal("tegra-gm20b: wake: nvkm objects resumed (%u channels back on the runlist, %u "
		    "dead ones left off): %llu us",
		    chans - dead, dead, us_since(start));
}

/* One kernel fence through the host, PBDMA, semaphore release and nonstall interrupt before any user work. The scrubber's
 * channel when there is one: no caller waiting at the gate holds scrub.lock (present.lock is held around a channel
 * creation, which waits at the gate, while present.chan is still NULL). */
static void wake_fence(void)
{
	struct tgm_chan *chan = scrub.chan ? scrub.chan : present.chan;
	struct mutex *lock = scrub.chan ? &scrub.lock : &present.lock;
	u64 start = nvxnu_nanotime(), end = start + TGM_PRESENT_NS;
	u32 seqno;
	int ret;

	if (!chan || READ_ONCE(chan->error)) {
		tgm_journal("tegra-gm20b: wake: no kernel channel to test the GPU on");
		return;
	}
	mutex_lock(lock);
	ret = PUSH_WAIT(&chan->chan.push, CHAN_FENCE_DWORDS);
	if (ret)
		nvxnu_panic("tegra-gm20b: wake: %s channel push buffer unavailable: %d",
			    chan->owner, ret);
	seqno = chan_fence(chan);
	mutex_unlock(lock);
	for (;;) {
		u32 seq = nvxnu_wake_seq();
		if ((s32)(tgm_chan_completed(chan) - seqno) >= 0)
			break;
		if (nvxnu_nanotime() >= end)
			tgm_chan_hung(chan, "wake: kernel fence", seqno, nvxnu_nanotime() - start);
		if (!nvxnu_idle(seq, end) && (s32)(tgm_chan_completed(chan) - seqno) >= 0)
			nvxnu_wake_due(seq, "fence", seqno);
	}
	tgm_journal("tegra-gm20b: wake: %s channel fence %u done: %llu us", chan->owner, seqno,
		    us_since(start));
}

static void wake_open(void)
{
	u64 start = nvxnu_nanotime();
	const char *clock = "not in use";

	if (idle.clk && sc7.idle_on) {
		nvxnu_mutex_lock(idle.clk);
		if (((nvkm_rd32(gpu.device, GPCPLL_COEFF_R) >> 16) & 0x3f) == idle.run_pl) {
			idle.log_start = nvxnu_nanotime();
			__atomic_store_n(&idle.last_busy, idle.log_start, __ATOMIC_SEQ_CST);
			WRITE_ONCE(idle.on, 1);
			clock = "on";
		} else {
			clock = "OFF for this boot";
			nvxnu_log("tegra-gm20b: wake: GPCPLL COEFF %08x is not the idle clock's PL "
				  "%u: idle clock off, GPU at %d kHz\n",
				  nvkm_rd32(gpu.device, GPCPLL_COEFF_R), idle.run_pl, sc7.khz);
		}
		nvxnu_mutex_unlock(idle.clk);
	}
	mutex_unlock(&gpu.lock);
	mutex_lock(&gate_lock);
	tgm_gate_open(&sc7_gate);
	nvxnu_wakeup(&sc7_gate.shut);
	mutex_unlock(&gate_lock);
	scrub_kick();
	tgm_journal("tegra-gm20b: wake: submissions open, idle clock %s: %llu us", clock,
		    us_since(start));
}

void tgm_sleep(void)
{
	sc7.plan = tgm_sleep_plan(gpu.stage, gpu.device != NULL);
	switch (sc7.plan) {
	case TGM_SLEEP_NOTHING:
		return;
	case TGM_SLEEP_POWER:
		sleep_power_down();
		tgm_journal("tegra-gm20b: sleep: no nvkm client (bring-up stopped after stage %d): "
			    "powered down",
			    gpu.stage);
		return;
	case TGM_SLEEP_FULL:
		break;
	}
	sleep_gate();
	sleep_fences();
	mutex_lock(&gpu.lock);
	sleep_idle_off();
	sleep_nvkm();
	sleep_power_down();
}

void tgm_wake(void)
{
	enum tgm_sleep_plan plan = sc7.plan;

	sc7.plan = TGM_SLEEP_NOTHING;
	switch (plan) {
	case TGM_SLEEP_NOTHING:
		return;
	case TGM_SLEEP_POWER:
		tgm_journal("tegra-gm20b: wake: GPU left off (its bring-up stopped after stage %d)",
			    gpu.stage);
		return;
	case TGM_SLEEP_FULL:
		break;
	}
	wake_power_up();
	wake_m1();
	wake_clock();
	wake_device();
	wake_nvkm();
	wake_fence();
	wake_open();
}

int tgm_bringup(int stage, char *line, unsigned int len)
{
	int ret = 0;
	line[0] = 0;
	if (gpu.stage < 1 && stage >= 1) {
		ret = m1(line, len);
		if (ret)
			return ret;
		gpu.stage = 1;
	}
	if (gpu.stage < 2 && stage >= 2) {
		ret = clk_raise(
			line,
			len); /* tegra-gpu-mhz only: GPCPLL above 76.8 MHz before ACR/PMU/GR */
		if (!ret)
			ret = m2(line, len);
		if (ret)
			return ret;
		gpu.stage = 2;
	}
	if (gpu.stage < 3 && stage >= 3) {
		ret = m3(line, len);
		if (ret)
			return ret;
		gpu.stage = 3;
		if (tgm_all_clients())
			iso.deadline = nvxnu_work_alloc(iso_deadline, NULL);
		nvxnu_errors_fatal(iso.deadline ? errors_contained : NULL);
		fault_test();
		/* Register the host-import coherency backend the kernel's IOSurface lock/unlock hook calls (XNU
		 * is_io_connect_method, tegra_coherency_register). The GPU does the imports, so it owns the registry. */
		tegra_coherency_register(tgm_coherency_read_lock, tgm_coherency_write_unlock);
		idle_start(); /* 768 MHz while busy, 76.8 MHz once truly idle */
		scrub_open(); /* the copy engine zeroes the pool from here on (see "pool scrub") */
	}
	return ret;
}

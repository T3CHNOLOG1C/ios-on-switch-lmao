// SPDX-License-Identifier: MIT
// Where a coherent BO's memory comes from (AGXDeviceUserClient::BoAlloc), as pure functions: the kernel calls them with
// the pool's live numbers, tests/gm20b_pool_policy_harness.cpp with a model of the pool (r8a: Maps' first buffers).
//
// The GPU pool (256 MiB of write-combined memory outside the VM) serves coherent BOs (NVK's HOST_COHERENT memory: its
// event heap and upload chunks, Metal's shared storage through m2v, the raw path's write-combined chunks), and nvkm's own
// page tables and channels, unpoliced. Its tiers keep room for the compositor and nvkm. A BO the tiers keep out of the pool,
// or one the pool has no room for, is not refused (r8a: Maps' first 4 MiB buffer, VectorKit aborting on a nil newBuffer,
// with 33 MiB free and 32 of them the compositor's reserve): it is served from ordinary pages, wired and zeroed, mapped
// write-combined into the task as a pool BO is (CONTRACT §3 (b): its CPU writes are in memory once the DSB before GP_PUT
// drains them). Only a failure of those pages too (the machine out of memory) refuses it.
#pragma once
#include <stdint.h>

namespace tgm_pool
{
struct Limits {
	uint64_t
		smallBytes;  // a BO up to this size only keeps floorBytes free (what a device needs to exist)
	uint64_t
		floorBytes;  // the pool's last bytes: the compositor's next m2v block and nvkm's tables for new clients
	uint64_t reserveBytes;  // a larger BO also leaves this free (tegra-gpu-pool-reserve-mb)
	uint64_t
		clientBytes;  // and keeps its client within this much pool (tegra-gpu-client-pool-mb): a fairness share
};

// Why a BO of `bytes` stays out of the pool while its client holds `held` and the pool has `avail` free, or nullptr (the
// tiers). The compositor keeps only the floor (nvkm's page tables and channels, which have no other memory): with its BOs
// served from system memory past that, it is never starved, and nvkm never is either (r8a: the compositor's twelve 16 MiB
// BOs refused at 244/256 MiB used).
inline const char *refusal(uint64_t bytes, uint64_t held, uint64_t avail, const Limits &l,
			   bool compositor = false)
{
	if (avail < bytes + l.floorBytes)
		return compositor ? "the pool's last 16 MiB stay for nvkm"
				  : "the pool's last 16 MiB stay for the compositor and nvkm";
	if (compositor)
		return nullptr;
	if (bytes <= l.smallBytes)
		return nullptr;
	if (avail < bytes + l.reserveBytes)
		return "the rest stays for the compositor";
	if (held + bytes > l.clientBytes)
		return "past the client's share";
	return nullptr;
}

enum Source : uint8_t {
	FromPool,    // the GPU pool
	FromSystem,  // ordinary pages, mapped write-combined
};

// A coherent BO's source: the pool when the tiers let the client have it (the compositor always) and the pool has it,
// else ordinary pages. `poolHasIt`: the pool allocation (after zeroing the freed ranges not zeroed yet) succeeded.
inline Source coherentSource(const char *refused, bool poolHasIt)
{
	return !refused && poolHasIt ? FromPool : FromSystem;
}
}  // namespace tgm_pool

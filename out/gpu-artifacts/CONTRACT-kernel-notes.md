# Kernel notes on CONTRACT.md v1 (from the GPU kernel agent)

**Bring-up log (2026-10-29, plugin round).** Each stage now logs its own line: `tegra-gm20b: M1 ok ...`, `M2 ok ...`,
`M3 ok ...`, then `M4 ok published AGXAccelerator (MetalPluginName TegraGM20BMetal)`, or the failing stage's `Mn FAIL ...`
line and nothing after it.

Implemented in `drivers/TegraGM20B/TegraGM20BUserClient.cpp` (AGXDeviceUserClient) and `TegraGM20BAccelerator.cpp`
(AGXAccelerator : IOAcceleratorES). `TegraGM20BABI.h` was copied byte for byte. Everything is syntax-checked and links;
none of it has run on hardware.

## Implemented as written
- **Methods.** All 11, in the direct and tunnel forms; the tunnel probe returns success.
- **Open type.** Only open type 0x4e56 is accepted; any other type is refused with kIOReturnBadArgument.
- **Record sizes.** In/out sizes must match exactly. VmBind and Exec also accept the shorter `count` form.
- **Reserved fields.** VmBind, ChannelCreate, Channel and GetStatus refuse non-zero reserved fields.
- **Memory tokens.**
  - Coherent BOs map write-combined; other BOs map with kIOMapDefaultCache.
  - The fence page uses token `0xfe000001` and maps read-only, write-combined. A channel's seqno is at `fence_offset = slot*16`.
- **Freeing.**
  - BoFree unbinds the BO's VAs at once, then snapshots every channel's submitted seqno.
  - The pages are released lazily, on later calls, once every channel has completed its snapshot or is dead.
  - clientClose destroys the channels first, waiting at most 1 s for each, and then frees everything.
- **Exec.** One GPFIFO entry per push, then a kernel fence push: an NV906F semaphore release of the 32-bit seqno with
  RELEASE_WFI=EN and 4-byte size. Then a DSB, then GP_PUT through nvif/chan906f.c. `count = 0` returns the last seqno and
  reports errors.
- **Memory model.** Sysmem PTEs are **volatile**, so the GPU L2 keeps no sysmem lines (§3(b), option one).

## Deviations and limits (please reflect in nvkmd_darwin)
1. **Binds are tracked whole.** A Map or Unmap that overlaps an earlier bind removes that entire earlier bind first, even
   where the new range only covers part of it. NVK binds and unbinds whole VA ranges, so this should not bite. Partial
   unbinds of one bind are not supported.
2. **No sparse.** MapSparse returns kIOReturnUnsupported and `InfoSparse` is not set. `InfoZcull` is not set either.
3. **Coherent BOs come from one contiguous GPU pool.**
   - The pool is 128 MiB by default (boot-arg `tegra-gpu-pool-mb`). A large Coherent BO can fail with NoMemory.
   - Non-coherent BOs are IOBufferMemoryDescriptors below 4 GiB, so keep big buffers non-coherent.
   - `max_bo_bytes` = 256 MiB.
4. **Capacity.** At most 8 channels per connection (ChannelCreate returns kIOReturnNoResources after that) and 16
   connections.
5. **Plugin registry keys and service class (changed 2026-10-29, plugin round).**
   - `MetalPluginName=TegraGM20BMetal`, `MetalPluginClassName=TegraGM20BMetalDevice` (the plugin's principal class), on
     `AGXAccelerator`, attached to the platform expert and registered at stage 4.
   - `AGXAccelerator` subclasses the original `IOAcceleratorES` of IOGPUFamily (now prelinked by the coordinator) as a late
     object: TegraGM20B.cpp loads IOGPUFamily, runs the exported static initializer inside preModLoad/postModLoad, then
     publishes (sheng's publishMetal). Its three IOAcceleratorES imports are bound by `drivers/IOSwitchAccelStub/
     original-bindings.S` in the same link; the GM20B archive has no bindings of its own.
   - The IOAcceleratorES stub (no MetalPluginName) can coexist: Metal skips it, and the plugin sets
     `NVK_DARWIN_SERVICE=AGXAccelerator` so nvkmd_darwin's `IOServiceMatching` opens ours, never the stub.
   - `NativeSandboxGrantProcess=backboardd` only. Our XNU's `native_sandbox_grant()` (IOUserClient.cpp:4334) compares the
     whole property with the process name, so the old comma list matched nobody. Other processes open under their own
     sandbox profile (and its user-client filter), as on a real iPad. Sheng's later XNU patch takes a list
     (`UniXNU_UniXNU/patches/xnu/zzw-native-adreno-sandbox-grant.patch`) if more clients are wanted.
   - The first successful open logs one line: `tegra-gm20b: AGXDeviceUserClient opened by <process>`.
6. **GetInfo values.**
   - `device_id` is 0.
   - `gpc_count`/`tpc_count` come from nvkm GR (`nvkm_gr_units`), so they are only valid once the GR stage is up.
     Before that, every method returns kIOReturnNotReady.
7. **Channel errors.**
   - `error = 3` comes from nvkm's channel-killed event; it is delivered while WaitFence/GetStatus poll the GPU interrupt
     handler.
   - `error = 4` comes from a 10 s WaitFence with `UINT64_MAX`.
   - MMU-fault (1) and GR-exception (2) classification and `fault_va` are not separated yet (TODO).
8. **Timestamps.** GetTimestamp is `nvif_device_time()` (PTIMER, ns).

# GM20B kernel <-> userland contract (v1)

(Was out/gpu-artifacts/CONTRACT.md, the scratchpad/gpu/CONTRACT.md that TegraGM20BABI.h and the NVK darwin backend name.)

**Owner and status.** The userland agent owns this file; the kernel agent implements it. Kernel feedback goes in
`./gm20b-contract-kernel-notes.md`.

**Wire format.** The exact records are in `./userland/abi/TegraGM20BABI.h`. Copy it verbatim into `drivers/TegraGM20B/`.
The C and C++ static asserts both pass.

**Client.** Mesa NVK with a new backend, `src/nouveau/vulkan/nvkmd/darwin/`, a sibling of `nvkmd/nouveau/`.

**Design.** The contract takes the slice of nouveau's VM_BIND/EXEC uAPI that `nvkmd_nouveau` really issues (§6) and simplifies it
in three ways:
- **No DRM syncobjs in the kernel.** Each channel has one monotonically increasing seqno, and sync objects live in userland. This
  is sheng's proven model: `vk_darwin_sync` + `vk_sync_timeline`, in `R/UniXNU_UniXNU/patches/mesa/turnip-darwin-kmd.patch:926-1077`.
- **No NVIF object tree.** Channel creation does the whole job, including binding the subchannels.
- **Every record is fixed-size and ≤ 4096 bytes,** so IOKit never hands the kext an out-of-line descriptor.

The transport copies `NativeAdrenoABI.h`/`NativeAdrenoPublish.cpp`, which already work under iPadOS 17 sandboxing.

## 1. Service, open, transport

### Registry and open
- **Accelerator.** `AGXAccelerator : IOAcceleratorES`, the same trick as `T/drivers/NativeAdreno/NativeAdrenoAccelerator.cpp`. It carries
  `MetalPluginName`/`MetalPluginClassName` for the plugin bundle, plus `NativeSandboxGrantProcess` (our XNU already honours that
  property: `kernel/xnu-10063.141.1/iokit/Kernel/IOUserClient.cpp`).
- **User client.** Class name `AGXDeviceUserClient`, so the original sandbox profiles admit the open (as sheng did).
- **Open type.** `TegraGM20BOpenType` = 0x4e56. Refuse every other type: the original IOGPU library opens with type 1. A
  diagnostic service, e.g. `TegraGM20BPublished` opened with type 0, may accept the same methods directly.
- **One connection = one GPU address space (VM).** A connection's channels live in its VM and its BOs belong to it; BOs are
  never shared across connections in v1. At least 4 concurrent connections: backboardd plus a few apps.

### Calls
- **Direct form.** `IOConnectCallStructMethod(conn, selector=method, in, in_size, out, &out_size)`.
- **Tunnel form.** The kext must accept this on every selector. The input starts with `{uint32 TegraGM20BTunnelMagic,
  uint32 method}`, followed by the method's record. The tunnelled method `TegraGM20BTunnelProbe` returns success and does
  nothing; the client uses it to find a selector the sandbox admits (sheng's `dw_probe_selector`).
- **Strict input checking.** `in_size`/`out_size` must equal the record sizes exactly. For `VmBind`/`Exec`, a shorter input
  holding just `count` entries is also accepted.
  - In tunnel form every input is 8 bytes longer, for the `{magic, method}` header; check the record size after stripping it.
  - The largest call is a tunnelled Exec with 255 pushes: 8 + 4088 = 4096 bytes. That is exactly IOKit's inline struct limit,
    so the kext accepts inputs up to 4096 bytes inline and never needs an out-of-line descriptor.
- **Return codes.**
  - `kIOReturnSuccess`.
  - `kIOReturnBadArgument`: bad record, unknown handle, or a range outside the client VA window.
  - `kIOReturnNoMemory`.
  - `kIOReturnBusy`: GPFIFO full after a bounded wait; the client waits on its oldest fence and retries.
  - `kIOReturnTimeout`.
  - `kIOReturnNotResponding`: the channel is dead. The client turns this into `VK_ERROR_DEVICE_LOST`.

### Memory maps
`IOConnectMapMemory64(conn, type=token, mach_task_self(), &addr, &len, kIOMapAnywhere)`, served by `clientMemoryForType(token)`:
- **BO tokens.** A BO allocated with `TegraGM20BBoCoherent` maps write-combined (`kIOMapWriteCombineCache`); any other BO maps
  `kIOMapDefaultCache`.
- **Fence-page tokens.** The fence page maps read-only.
- **Unmap.** The client calls `IOConnectUnmapMemory64` before `BoFree`. The kext also drops any client views on BoFree and
  clientClose.

## 2. Methods

| # | Method | In → Out | Kernel semantics |
|---|---|---|---|
| 1 | GetInfo | – → `TegraGM20BAbiInfo` | Static facts (details below). |
| 2 | BoAlloc | `BoAlloc` → `BoAllocReply` | Zeroed pages, physically below 4 GiB. Contiguous only if `TegraGM20BBoContiguous` is set; otherwise page lists are fine because the GMMU maps them. Returns `{handle, map_token}`. Allocating does **not** map it in the GPU VM. |
| 3 | BoImport | `BoImport` → `BoAllocReply` (map_token 0) | Wires the caller's pages with `IOMemoryDescriptor::withAddressRange(...task)` + `prepare()`. They keep the owner's CPU mapping and cache mode. Used for IOSurface/host-pointer import. |
| 4 | BoFree | `BoFree` → – | Unbinds any VA still pointing at the BO. Releases or unwires the pages **only after** every channel of the connection has completed the seqno it had submitted at the time of the call (§4). |
| 5 | VmBind | `VmBind` → – | Ops run in order and synchronously; the GMMU TLB is invalidated before return. `Map` with `kind` into `[va_start, va_end)`. `Unmap` sets PTEs invalid. `MapSparse` sets sparse PTEs (reads return 0, writes are dropped) only if `InfoSparse` is set; otherwise it returns kIOReturnUnsupported. Overlapping maps replace what was there, like nouveau's VM_BIND. |
| 6 | ChannelCreate | `ChannelCreate` → `ChannelCreateReply` | Channel setup (details below). |
| 7 | ChannelDestroy | `Channel` → – | Bounded wait for idle (≤ 1 s), then preempt or kill and free. |
| 8 | Exec | `Exec` → `ExecReply` | Submission (details below). |
| 9 | WaitFence | `WaitFence` → – | Polls the fence page; no IRQs, because ArmCpu masks SPIs. Back off from a few spins to `IOSleep(1)`. Returns Success, Timeout, or NotResponding. `timeout_ns = 0` checks once. `UINT64_MAX` means 10 s, after which the channel is declared dead. |
| 10 | GetTimestamp | – → `Timestamp` | PTIMER in ns. It must be the clock the GPU writes for semaphore-release timestamps; NVK calibrates host/device timestamps from it. |
| 11 | GetStatus | `Channel` → `Status` | `{completed, submitted, error, fault_va}`. `error != 0` is sticky: the channel is dead. |

**GetInfo values:**
- **Chip:** `chipset` 0x12b; `gpc_count`/`tpc_count` from the GR unit config (1/2 on T210); `chip_name` "GM20B".
- **Classes:** copy 0xb0b5, 2D 0x902d, 3D 0xb197, M2MF 0xa140, compute 0xb1c0, gpfifo 0xb06f.
- **VA window and pages:** `va_start` 0x1000; `va_end` 1<<39 (nouveau's kernel reservation start,
  `M/src/nouveau/winsys/nouveau_device.h:18`); `page_size` 4096.
- **Optional:** `zcull[]` only if the kext fills it (`InfoZcull`); NVK works without zcull.

**ChannelCreate:**
- Creates a GPFIFO channel of class `cls_gpfifo` on the GR runlist, bound to the connection's VM, with its GR context.
- The kext builds the golden context once per boot.
- Before the first user push, the kext's own init push binds subchannels 0..4 with `SET_OBJECT`: 0 = eng3d, 1 = compute,
  2 = m2mf, 3 = eng2d, 4 = copy.
  - Why the kext must do this: NVK itself binds only subchannels 0 and 1 (`M/src/nouveau/vulkan/nvk_cmd_draw.c:150`,
    `nvk_cmd_dispatch.c:37`). It still emits copy methods on subchannel 4 (`nvk_cmd_copy.c:974`, `nv_push.h:94`).
  - nouveau GL does that binding in userspace on GR channels, MAXWELL_DMA_COPY_A included
    (`M/src/gallium/drivers/nouveau/nvc0/nvc0_screen.c:886-920`).
  - nvgpu confirms GM20B's copy class is MAXWELL_DMA_COPY_A (`gpu-research/gm20b_hal_gm20b.c:175-176`).
- `engines` is advisory. Any mask, Copy-only included, may get a full GR channel.
- Returns the channel id and the fence-page token and offset.

**Exec:**
- For each push, in order, one GPFIFO entry: `{va, dwords, NO_PREFETCH}`. The entries of one Exec are contiguous, because NVK
  splits a method across two pushes ("incomplete", `nvkmd.h:300-304`).
- Then the kext's fence push: WFI, the L2/FB flush needed for CPU visibility (§3), and a semaphore release of `seqno`
  (32-bit) to the fence page.
- Then DSB and GP_PUT.
- `count = 0` submits nothing and returns the last submitted seqno. It is still the health check: it returns NotResponding on
  a dead channel.
- If the GPFIFO is full, poll up to 1 s for space, then return Busy.

## 3. Memory model (Tegra: unified DRAM, GPU not IO-coherent with the CPU caches)

**Visibility guarantees the kext must give:**
- **(a) GPU → CPU.** When a channel's fence page shows `seqno ≥ N`, all GPU writes of submissions ≤ N are in DRAM. A
  write-combined or uncached CPU read sees them. A cacheable CPU mapping sees them after the client invalidates its own lines.
  - NVK does that invalidate via `nvkmd_mem_sync_map_from_gpu`, which uses `util_flush_inval_range` (DC CIVAC from EL0).
- **(b) CPU → GPU.** Data the CPU wrote before Exec is visible to that submission. That holds for write-combined BOs (the kext's
  DSB before GP_PUT drains them), and for cacheable BOs once the client has cleaned its lines (`nvkmd_mem_sync_map_to_gpu`).
  - The GPU's L2 must not serve stale sysmem lines across submissions. Either map sysmem PTEs volatile (no L2 allocate), or
  invalidate L2 for sysmem at the start of each Exec. That choice is the kernel's.

**How NVK uses the two CPU mapping types.**
- NVK on SOC exposes two memory types, HOST_CACHED and HOST_COHERENT (`M/src/nouveau/vulkan/nvk_physical_device.c:1680-1700`).
  `NVKMD_MEM_COHERENT` becomes `TegraGM20BBoCoherent`.
- Its internal push and upload memory is allocated coherent (`nvk_device.c:137,344`); everything else it flushes explicitly.

**Kinds.** `kind` is NVK's `pte_kind` from nil, i.e. Maxwell GMMU kinds. Compression kinds need comptags, which v1 does not
provide, so the kext maps compressible kinds as their uncompressed equivalent. The client reports `has_compression = false`,
so NVK should not ask for them anyway.

## 4. Lifetime and safety rules (no SMMU: a stale GMMU PTE is kernel memory corruption)

1. **No page leaves a BO while the GPU might touch it.**
   - BoFree, clientClose and process death snapshot every channel's `submitted` seqno.
   - The kext unmaps the BO's VAs immediately and invalidates the TLB.
   - The `IOBufferMemoryDescriptor`, or the wired user range for imports, is released only when every snapshotted seqno has
     completed or its channel is dead and stopped.
   - Reap lazily on later calls plus a timer.
2. **Dead channels.** A dead channel is stopped (PBDMA/engine preempted, runlist entry removed) before its BOs can be
   released.
3. **The kext's own VA range.** `[va_end, 2^40)` belongs to the kext: fence page, init push, ctx buffers. Binds outside
   `[va_start, va_end)` are refused.
4. **Imports.** BoImport refuses ranges the caller doesn't own or can't wire. The wired pages stay wired until rule 1
   releases them.

## 5. Userland side (nvkmd_darwin, for reference; the kernel doesn't need to read this)

| nvkmd op (`M/src/nouveau/vulkan/nvkmd/nvkmd.h`) | darwin implementation |
|---|---|
| pdev create / `kmd_info` | Open + probe the tunnel + GetInfo, then fill `nv_device_info`. `type` = SOC. `sm` = 53. The rest from the winsys tables (`nouveau_device.c:53-355`). `kmd_info` = {all false except `has_host_import`}. `bind_align_B` = 4096. |
| `get_vram_used` | 0 (SOC). |
| `get_drm_primary_fd`, `get_drm_fd` | NULL (callers handle -1). |
| `get_gpu_timestamp` | GetTimestamp. |
| `alloc_mem` / `alloc_tiled_mem` | BoAlloc (COHERENT → Coherent), then alloc_va from a userland `util_vma_heap` (as `nvkmd_nouveau_va.c`), then VmBind Map with the BO's `pte_kind`. `tile_mode` is ignored. |
| `import_dma_buf`, `export_dma_buf` | VK_ERROR_INVALID_EXTERNAL_HANDLE. |
| `import_host_mem` (new op, for VK_EXT_external_memory_host / IOSurface) | BoImport; `map` = the host pointer, non-coherent. |
| `mem map`/`unmap` | IOConnectMapMemory64/UnmapMemory64 on `map_token`. MAP_FIXED maps at the given address without kIOMapAnywhere. `overmap` = unmap + `mmap(PROT_NONE, MAP_FIXED\|MAP_ANON)`. |
| `alloc_va` / `va free` / `bind_mem` / `unbind` | Userland heap; VmBind Map / Unmap (MapSparse for sparse VAs). |
| ctx `create` | ChannelCreate. The bind ctx (`NVKMD_ENGINE_BIND`) is userland only, with synchronous VmBind batches. |
| ctx `exec`/`flush` | Queue pushes, then one Exec of ≤255 pushes; record the seqno on the ctx. |
| ctx `wait` | CPU-wait each `vk_sync` (fence page, then WaitFence) before the next flush, as sheng does. |
| ctx `signal` | Flush, then mark each sync "pending on (channel, seqno)". |
| ctx `sync` | Flush, then WaitFence on the last seqno, then GetStatus. |
| `sync_types` | `{darwin_sync (binary), vk_sync_timeline over it, NULL}`. |

## 6. What NVK issues on Linux, and where each piece went

All in `M/src/nouveau/winsys` and `M/src/nouveau/vulkan/nvkmd/nouveau`:

| Linux / nouveau (where) | Here |
|---|---|
| `DRM_NOUVEAU_VM_INIT` (`nouveau_device.c:509-514`) | Implicit per connection; window in GetInfo. |
| `NVIF NEW NV_DEVICE` + `NV_DEVICE_V0_INFO` (`nouveau_device.c:370-454`) | GetInfo. |
| `GETPARAM` PCI_DEVICE, GRAPH_UNITS, EXEC_PUSH_MAX, PTIMER_TIME, HAS_VMA_TILEMODE, VRAM_USED (`nouveau_device.c:519-680`) | GetInfo / GetTimestamp / constants. |
| `GET_ZCULL_INFO` (`nouveau_device.c:456-472`) | `GetInfo.zcull` (optional). |
| `CHANNEL_ALLOC` + NVIF SCLASS + NVIF NEW per class (`nouveau_context.c:36-209`) | ChannelCreate (classes in GetInfo, subchannels bound by the kext). |
| `CHANNEL_FREE` | ChannelDestroy. |
| `GEM_NEW` / `GEM_INFO` / mmap(`map_handle`) (`nouveau_bo.c`, `nvkmd_nouveau_mem.c:164-262`) | BoAlloc + map_token. |
| `GEM_CLOSE` | BoFree. |
| `VM_BIND` MAP/UNMAP/SPARSE, sync and async (`nvkmd_nouveau_va.c:97-249`, `nvkmd_nouveau_ctx.c:294-460`) | VmBind (synchronous only). |
| `EXEC` with `drm_nouveau_exec_push` + `drm_nouveau_sync` wait/signal (`nvkmd_nouveau_ctx.c:136-283`) | Exec → seqno; waits and signals in userland. |
| `SYNCOBJ_CREATE`/`WAIT`/... (`vk_drm_syncobj`, `nvkmd_nouveau_ctx.c:76,264`) | Userland `darwin_sync` + WaitFence. |
| `GEM_PUSHBUF` with nr_push = 0 (`nouveau_context.c:303-317`), EXEC with no pushes | Exec count 0 / GetStatus. |
| PRIME/dma-buf | Not supported; BoImport covers IOSurfaces. |

**Changes in v1.** None yet; kernel notes (`./gm20b-contract-kernel-notes.md`) did not exist when this was written.

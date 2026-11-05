# Direct scanout and video planes on DC A (design, r14-pacing)

Two follow-ups to the triple-buffered scanout (TegraScanoutRing.h): scan Core Animation's framebuffers directly (no GPU
copy), then give video its own DC window (no composite). **[V]** = read on the device, in the 21H461 binaries or in
this tree; **[I]** = inferred, to check; **[T]** = to be proven by a boot self-test before it is used.

## Where things stand (r7q, main eae4371)

- CA renders 720x1280 portrait at 1:1 (`IOSWITCH_PORTRAIT_NATIVE=1`, backboardd `nativeRotation:0`) [V]. The GPU copy
  (`tegra_gm20b_scanout_copy`) does not rotate: it converts CA's pitch-linear BGRA (rows of 2944 bytes) to the block-linear
  layout window A row-scans. It costs 0.46 ms on the copy engine (`stats2 copy mean 460 us`); offer to done is otherwise
  CA's own GPU tail (the copy starts 3 us after it) [V]. Direct scanout saves that copy, ~7.5 MB of DRAM traffic a frame
  and one GPU job a frame; it is worth ~0.5 ms of swap-to-scan, not a frame. The frame was the two-buffer flip-wait.
- The blocker is memory, not orientation: CA's framebuffer IOSurfaces are 3.75 MiB in 240-959 physical pieces (largest
  8-36 KiB, `IOSwitch display: client surface N` lines) [V], and DC A reads physical addresses: its swgroups are in
  bypass. Window A can scan pitch (hekate's own window is pitch, 2880-byte rows) and BGRA [V].
- SMMU users today [V]: TegraVIC owns ASID 127 on the `vic` swgroup (MC 0x284, clients vicsrd/vicswr in
  TRANSLATION_ENABLE_3 bits 12/13), its own page directory, and drives the global `PTB_ASID`/`PTB_DATA`/`TLB_FLUSH`/
  `PTC_FLUSH` registers directly (TegraVIC.cpp smmuAttach/smmuFlush). NVDEC (0xab4), NVENC (0x264) and host1x (0x250)
  refuse to start if their swgroup is translated; they use physical arenas. The GPU translates with its own MMU.
  switchroot's bl31 leaves every client's translation bit set and every swgroup in bypass; the global enable is
  secure-only (VIC's V3 proves translation instead of reading it).

## Phase 1: CA framebuffers scanned through the SMMU

1. **TegraSMMU (shared).** Extract VIC's domain code into `drivers/TegraSMMU`: one lock around every `PTB_ASID` +
   `PTB_DATA` pair and every flush (two drivers selecting ASIDs at once would program each other's directory); an ASID
   table (VIC 127, display 126); a domain = page directory + tables in one contiguous buffer, 4 KiB PTEs
   (`PFN | R 31 | W 30 | NS 29`, PDE `NEXT 28`), `identity(range)`, `map(iova, pages)`, `unmap`,
   `attach(asidReg, translationBits)`, `detach`, `reattach` after SC7 (the MC loses it). VIC moves onto it with its
   stages and lines unchanged. Host test:
   the PTE/PDE images and flush order against a register model.
2. **Display domain, ASID 126, swgroup `dc`** (MC 0x240 [I: memory/tegra/tegra210.c], clients display0a/0b/0c and
   displayhc in TRANSLATION_ENABLE_0 [I]). Identity-mapped before the switch: everything DC A may scan then: the boot
   framebuffer (0xf5a00000), scan surfaces 1 and 2, the console tape. The panic hook keeps working (it writes the boot
   framebuffer's PA, identity-mapped). CA surfaces get IOVA slots below DRAM (e.g. 0x10000000 + i * 4 MiB, never a DRAM
   PA, so they cannot alias the identity maps), four slots LRU by IOMemoryDescriptor like the GPU's present surfaces.
3. **Gate (D0-D3, one line each, any failure = one line, swgroup back to bypass, today's copy path for the boot):**
   D0 domain built; D1 attached with identity maps while the console/boot frame scans (no visible change); D2 translation
   proven with the DC CRC unit (`DC_COM_CRC_CONTROL`/`CHECKSUM`): CRC of a pattern buffer scanned by PA, then through an
   IOVA mapped to it (must match), then that IOVA remapped to a second pattern (must change, and match its own CRC) [T],
   on window C at a few lines during the boot logo; D3 a CA surface scanned through its slot for one frame with the CRC
   equal to the copy path's CRC of the same surface [T]. Only then do swaps go direct.
4. **Per swap:** BGRA, full frame, single layer (what validateSwap accepts today), stride a multiple of 64 [I: DC pitch
   alignment] → prepare and map the surface into its slot (cached per surface), ask the GPU for a **fence-only present**
   (`tgm_present_copy` without the copy: the acquires on the writers' channels, the kernel fence with its L2 flush, the
   nonstall interrupt; TegraScanoutCopy.h v2), and at its completion flip window A to the slot with SURFACE_KIND pitch,
   LINE_STRIDE = the surface's stride, H offset 0 (latched with START_ADDR on the same ACT_REQ; a copy-path frame flips
   back to block-linear the same way). The ring (TegraScanoutRing.h) gains slot entries: a slot is busy while scanned or
   asked for, exactly like a scan surface.
5. **Surface lifetime:** the DC reads a direct surface until the next flip is seen taken, so CA must not draw into it until
   then. CA reuses framebuffers on swap completion, not on use counts: QuartzCore logs "swap %u completed, recycling
   surface 0x%x from swap %u" [V, dyld cache string], i.e. a completed swap frees the surface of an earlier one, which is
   IOMFB's own meaning of completion (on screen). So a direct swap retires when its flip is seen taken, not at the offer
   (the copy path's early retire stays: its surface is read by the copy, which the GPU orders before CA's next render);
   a frame replaced before it latched retires with the one that replaced it. CA has three framebuffers (client surfaces
   0-2 in r7q [V]): one scanned, one asked for, one to draw, so 60 Hz with one frame of latency holds [I: measure, as the
   early-retire change of r3 was kept for speed]. The kernel keeps the surface's descriptor retained and prepared (wired)
   for as long as it is mapped in a slot.
6. **Probe first (done, r14):** DisplaySmmuProbe.h runs D1-D2 once at boot and undoes it (CRC by physical address, through
   the identity, through an IOVA slot below DRAM, with a pixel-change liveness check); its line on the next boot is what
   steps 3-5 wait for. Window A is client display0a (id 1, TRANSLATION_ENABLE_0 bit 1) in swgroup dc (MC 0x240); the
   T210 device tree gives DC A the dc and dc12 (0xa88) swgroups, dc12 being window D's clients [V: tegra210-soc-base.dtsi,
   hekate mc_t210.h], which stays in bypass (window D is never enabled here).
7. **Expected:** swap-to-scan ~0.5 ms shorter (frames that finish within that of a boundary make it), 450 MB/s less DRAM
   traffic at 60 fps, the present channel idle for UI frames. Next-boot lines: the D0-D3 lines, the pacing line's copy
   stage dropping to the GPU tail alone, no WIN_A underflow frames.

## Phase 2: video on its own DC window

**Done in r18-planes** (11-05); the rules, their sources and tags are in `NativePlanes.h`, the driver side in
`NativePlanesDisplay.inc`. What changed from the plan below: Core Animation never retries a refused swap (it sends it
again every frame), so the display publishes the limits it can show (IOMFBScalingLimits 4/4, IOMFBMaxSrcPixels 1920
wide and 2048x2048 buffers, IOMFBNumLayers 2, plane bases 64) and accepts every swap within them; on this internal
display CA copies any rotated, out-of-limit or converted layer through the IOSurfaceAccelerator (TegraVIC) first, so
window B only scans display-ready layers; window B's SMMU proof runs at the display's start (the plane probe), before
backboardd, and backboardd drops CA_DISABLE_DETACHED only once it passed and TegraVIC proved quarter turns
(`native_legacy_detach_ready`). The plan as written before:


1. **What CA wants [V names, I semantics]:** QuartzCore decides per layer with `kCADetachReasonCode*` (DisplayRejected,
   NoDisplayCompositing, WindowServerRejected, Rotation, NonOpaque, NonAffine, RectClipped, UnderBackdrop, ...), reads
   `IOMFBScalingLimits`, `IOMFBMaxSrcPixels` and `DisplayPipePlaneBaseAlignment` from the framebuffer, and disables detach
   when a bandwidth budget is exceeded ("Display %d gpBandwidth Exceeded. Detach disabled"). Env switches: `CA_PRINT_DETACHING`
   (logs each decision), `CA_COLOR_DETACHED`, `CA_DISABLE_DETACHED`, `CA_DETACH_ANY_PRIMARIES`. r7q shows `rejected 0`:
   CA never sends a multi-layer swap today. Step one is a diagnostic boot with `CA_PRINT_DETACHING=1` in backboardd's
   environment (kern_exec, as CA_FORCE_TIMER; no boot-arg) during YouTube, then the RE of the display-side checks it names.
2. **Swap side:** accept a second layer in validateSwap (surfaces at +2624 [4], rects and per-layer state as the single
   layer's [V offsets for layer 0 only]), YCbCr 4:2:0 ('420v'/'420f', two planes) for it; publish the properties CA reads
   with window B's real limits.
3. **DC side:** window B (YUV 4:2:0 semi-planar, DDA scaling with filters, blend order under/over window A [I: T210 dc_reg.h
   modes]) through the display domain, both planes in a slot; flipped with window A on one GENERAL_ACT_REQ.
4. **Decoder side:** NVDEC's output IOSurface scanned in place through the SMMU (no CE conversion copy), held until it
   leaves the scan as in Phase 1.
5. **Expected:** during playback CA composites only the UI when it changes; video frames cost no GPU composite and present
   at their own cadence.

## Order and risk

Phase 1 needs the shared SMMU first (VIC regression risk: its V-stages must still pass), then the display gate. Every
step is behind a boot self-test with a one-line fallback to today's path; a lost DC interrupt or GPU completion still
panics. Nothing here needs a boot-arg.

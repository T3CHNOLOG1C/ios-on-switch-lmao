# Native RAM (V21)

**V23 is installed, verified and safely ejected.** All five changed files passed readback; existing Data files were retained without recopy or content hashing. [Installation](../out/switch/native-xnu-v23-install.json), [package](../out/native-vmregions-v23-delta/manifest.json).

V21 removes the native profile's 768 MiB managed-memory cap. That was an early bring-up restriction, not a Switch hardware limit. The combined replacement also corrects V20's nonsecure MC access guard; no DMA throughput was established by the failed V20 run.

The boot memory aperture is now 4 GiB, from physical `0x80000000` through `0x180000000`. The measured OLED banks are `0x80000000..0xffd00000` and `0x100000000..0x180000000`. The intervening 3 MiB secure range is unavailable. [Measured platform](native-memory-result.md).

The loader records the actual firmware device-tree reservation table and reserved-memory nodes in a bounded handoff ledger. XNU reserves those pages, the inherited framebuffer, the retained adapter/storage-reference mailbox, its own kernel and bootstrap data, and later VM metadata. The old U-Boot stack, heap and staged copy buffers can be reclaimed after the guarded, quiesced, no-return handoff. Free memory therefore depends on the runtime reservations and kernel allocations; reporting a 4 GiB address span does not mean all of it is free.

The initial kernel load address stays unchanged. Its 768 MiB contiguous placement envelope protects the early copy and temporary mappings only. The kernel's boot arguments independently describe the full RAM aperture. XNU's temporary bootstrap tables retain the bounded mapping until its final mappings are installed, avoiding normal-memory aliases over the display and secure ranges. The framebuffer and retained SD reference remain device mappings.

XNU's physical-page metadata keeps one slot for every 4 KiB page in the aperture. Reserved slots never reach backing initialization, debug fill or the free queues; preserving their positions keeps the physical page numbers above each hole correct. Early metadata allocations are tracked separately, so reclaiming low RAM cannot free them twice. Normal memory accounting includes real wired kernel and metadata pages and excludes permanent reservations.

The existing native 4 KiB policy applies at every memory size. The separate nonnative heuristic that selects 16 KiB pages above 1 GiB remains outside this path. The native vnode budget also scales with the larger RAM profile instead of retaining the previous 768 MiB value.

Focused checks use the actual changed C helpers, page-array indexing macros and native ARM64 compilation. They do not run a VM, boot the Switch or establish sustained allocation from the upper bank. Actual RAM ownership, high-bank allocation, DMA behavior, memory pressure and the iPadOS UI still require the next hardware boot.

[Source and helper proof](../out/research-native/full-ram-v21/README.md), [VM startup and native object proof](../out/research-native/native-full-memory-review/REPORT.md), [actual new adapter review](../out/research-native/full-ram-v21/v21-artifact-review.json), [combined build](../out/native-a57/root-v1/passes/native-root-memory-v1/build-receipt.json). The focused fixtures reproduced the old dense-array assertion and bootstrap framebuffer collision before their fixes. Normal header dependency rebuilding refreshed the inlined bank predicates; the combined native build and prelink completed with the same 33 original components. No VM, broad suite or retained-image hash scan was run.

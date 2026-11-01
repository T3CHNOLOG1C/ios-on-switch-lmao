# Early native XNU entry diagnostics (V22)

**The V22 diagnostic correction is retained in installed V23.** [Installation receipt](../out/switch/native-xnu-v22-install.json), [package](../out/native-entry-v22-delta/manifest.json).

V21's owner-reported hardware run stayed on **COPY COMPLETE**. The owner then powered it off and reconnected USB storage. There is no photograph or evidence establishing whether XNU entered, panicked or stalled. [Hardware record](../out/switch/native-xnu-v21-hardware-result.json).

The actual V21 entry gate still required the old 768 MiB physical-memory pair and graphics video tuple `display=1, depth=32`. The stock text profile has used `display=0, depth=0x320`; V21 also changed the memory pair to the full RAM aperture. Consequently the gate left all early stage and fault rendering disabled. The previous mapping fixture checked the mapping macros without executing this gate and missed that dependency.

V22 accepts only the exact reviewed old or full-RAM PA/VA/size tuple, the measured framebuffer geometry, and the old graphics or current stock-text tuple. It also requires incoming MMU and caches off. The framebuffer receives device mappings in the temporary tables. The existing stage marks and early exception handler can then operate before the stock console exists. Thirty-two checks execute the actual preprocessed ARM64 gate; its native object and linked kernel were independently checked. [Gate proof](../out/research-native/v21-entry-regression/result.json).

An ordinary early C panic previously returned from the diagnostic hook because normal console initialization had not completed. V22 adds a bounded branch to that existing hook while early diagnostics are armed. It draws a failure header, last stage and caller before formatting the panic reason into the existing bounded buffer. Recursive faults stop without overwriting that header. Eight focused extracted-branch/renderer checks and actual ARM64 compilation passed. [Panic proof](../out/research-native/native-full-memory-review/early-panic/REPORT.md), [final linked artifact](../out/research-native/native-full-memory-review/early-panic/final-v22-artifact-review.json).

On the next boot:

- Small assembly-stage marks distinguish initial XNU entry from the loader screen.
- Numbered C stages identify progress through early CPU and VM setup.
- **EARLY XNU EXCEPTION** reports the last stage, ESR, FAR and ELR.
- **EARLY XNU PANIC** reports the last stage, caller and bounded panic reason.

Photograph the whole final screen. These diagnostics retire before the temporary framebuffer mapping is removed; the normal stock console takes over later. The custom HUD and process log spam remain disabled. The early diagnostic window does not cover every later startup failure.

This corrects visibility and does not establish the cause of the V21 stop. V22 retains V21's full RAM aperture, 4 KiB pages and required DMA. Multicore startup remains unimplemented in the native GICv2 profile; the current boot still exposes only CPU0. Physical boot, DMA operation, upper-bank allocation and the UI remain unqualified.

V22 subsequently reached every stage through 21 on hardware. Its photographed stop is addressed by the [V23 virtual reservation correction](native-virtual-regions.md); this is not evidence of a successful complete boot.

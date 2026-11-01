# Native read-only block devices and APFS data mounts

**V9 native APFS access succeeded by owner report.** The owner reported `apfs files verified - read only`; the installed path requires both read-only mounts and complete 574-byte and 49,152-byte files with matching hashes. No photograph or timing was supplied. [Physical V9 result](../out/switch/native-xnu-v9-hardware-result.json). Root remains the own diagnostic RAM disk; Apple launchd, writable Data and the iPadOS UI are not yet established.

**V9 is installed and verified; its physical APFS success is recorded above.** The [installation receipt](../out/switch/native-xnu-v9-install.json) records all ten boot files verified, 154 tracked existing files preserved and all 9.38 GB of retained media checked by SHA-256 before writes. The same menu entry now points to V9. The final [qualification](../out/native-a57/sd-block-v1/qualification.json) passes 56,479 checks across five model outcomes and binds the reviewed native kernel and package. The [installation gate](../out/research-native/native-xnu-v9-install-review.json) binds this exact package to those reviews; physical success is recorded by owner report above.

V8's physical FAT32/split-file sample test succeeded by [owner report](../out/switch/native-xnu-v8-hardware-result.json): `samples verified` and `fat files successfully verified`. No photograph or timing was supplied. That remains the physical storage baseline; V9's subsequent owner-reported result establishes its native block-device and APFS file-read path.

## Intended result

Keep the signed own PID1 process and `rd=md0`. Publish the two complete OS and SystemOS disk images through genuine IOBlockStorageDevice children. Their existing GPT/APFS structures are discovered by the original IOStorageFamily and APFS modules. The test obtains each volume's observed BSD name by its expected APFS UUID and its ancestry under the corresponding IOSwitch child, then requests ordinary read-only data mounts.

The test reads the complete OS `/System/Library/CoreServices/SystemVersion.plist` (574 bytes) and SystemOS `/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.02` (49,152 bytes). Each read loop must reach the exact length, observe EOF, and close successfully. The kernel then copies the buffers from the signed process and compares full SHA-256 digests to the exact original files extracted read-only on the host. Only that complete path displays `APFS FILES VERIFIED - READ ONLY`.

This is filesystem access from the own test process. It does not execute Apple's launchd, change root authentication, authenticate an iPadOS root volume, or boot the iPadOS UI.

## Lifetime and request bounds

The native SD host, reader callbacks and trusted part descriptors persist for the whole boot. A shared sleepable IOKit lock serializes both disks. Each FAT chain is validated once while constructing an immutable cluster index; combined index allocation is capped at 16 MiB. Indexed data reads do not walk FAT chains or allocate memory.

Each block request is sector-aligned, nonzero, and at most 64 KiB. The owner reads into a private 64 KiB buffer before copying any bytes to the caller. A transport failure discards that buffer and marks both disks unavailable. Completion runs exactly once after unlocking, so a completion may submit another read without deadlocking. Write, format, unmap, eject and write-cache changes are refused.

Index construction has a 131,072-sector and 120-second budget. Ordinary reads receive a fresh sector budget equal to their requested size and a five-second budget. Time is checked between sectors and after the final read; an in-flight SD command can exceed that budget by up to its three one-second phase limits, but the request then fails. There is no automatic controller reset, retry, reinitialization or media write. The original finite qualification's lifetime limit no longer expires an otherwise healthy persistent service.

## Discovery and signed fixture

Both children prepare successfully before either is published. The boot retains their owner, and each child retains it until its final release. Stop marks the shared service offline; cleanup waits for the owner's final release.

The fixture queries the registry through fixed, signature-bound getpid checkpoints. Successful discovery requires the expected UUID, an observed BSD name and device numbers, a live IOMedia service, and ancestry to the correct child. Duplicate matching UUIDs refuse. No guessed `diskNsM` path is used. The bounded polling loop makes at most 200 registry queries and yields with Mach `thread_switch(MACH_PORT_NULL, SWITCH_OPTION_NONE, 0)` so IOKit matching can run without depending on timer wakeups. A failed or unavailable volume reaches a diagnostic failure result.

The native test requires `ioswitch-block-test=1`, the existing signed PID1 qualification, and successful SD/GPT/FAT/sample qualification. Original APFS loads with `offline-apfs-load=1`. The fixture makes `MNT_RDONLY`, mode-1 data mounts under devfs; root stays `md0`. The trust cache contains only the freshly signed own fixture. Data-file reads do not require adding the cache file to executable trust.

## Final local evidence

The [independent model review](../out/native-a57/sd-block-v1/model-block-independent-review.json) binds saved guest memory, signed-process results, original executable bytes and every debugger-served sector batch to the local source disks. Original IOStorageFamily and APFS code execute in a Cortex-A57/GIC model with a separately compiled fake SD register/cache adapter. The native build contains no model bridge symbols; its other reviewed objects are identical to the model build. The 14 original modules' executable ranges remain unchanged.

| Final model run | Observed outcome |
| --- | --- |
| [APFS success](../out/native-a57/sd-block-v1/runs/apfs-success-v9d/result.json) | Two devices published; both UUIDs discovered; ordinary read-only APFS mounts and the signed PID1's complete 574-byte and 49,152-byte reads succeeded. Both full hashes matched; block stage 5, error 0. The driver completed 329 reads totaling 1,364,992 bytes without failure. |
| [Transport refusal](../out/native-a57/sd-block-v1/runs/transport-refusal-v9d/result.json) | The first block-read transport fault marked the service unavailable. No further bridge batches were requested. After 200 discovery queries and 199 successful Mach yields, the fixture reached a failure result: stage 2, error 306 (`132` on the hexadecimal display), with no file proof. |
| [File-hash refusal](../out/native-a57/sd-block-v1/runs/file-hash-refusal-v9d/result.json) | One OS file byte changed while transport CRCs remained valid. Both full reads completed, but the OS SHA-256 failed: stage 4, error 307 (`133` on screen). The SystemOS hash still matched. |
| [Native build with storage disabled](../out/native-a57/sd-block-v1/runs/native-disabled-v9b/result.json) | The exact native kernel reached the signed EL0 checkpoint in the CPU model, with zero published block devices and zero storage reads. This is a native-build control, not a physical SD result. |
| [Signed-code refusal](../out/native-a57/sd-block-v1/runs/amfi-padding-refusal-v9b/result.json) | Changing one signed padding byte caused AMFI rejection before the own PID1 syscall and storage probe. No devices or reads were published. |

The final native kernel is `df05c4b8ffb1fc656286dfba39f45913be6e66724a0cb0accc5367904f0fbc2b`; the [package manifest](../out/native-sd-block-v1b-fat32/manifest.json) is `0fac3b220da08bba8c874322d63371538baf2a198a25ef43eed20be08d61d635`. Its ten boot files total 69,098,050 bytes. The existing four large media files are retained and must pass the installer's complete readback before publication.

- The portable backend's host tests execute the real FAT/split implementation and cover allocation cleanup, cross-part reads, offsets above 4 GiB, timeout/error handling, immutable-index use and fresh per-request budgets.
- The IOKit host harness compiles the actual driver with a mock framework and checks completion reentry, two-thread serialization, refusal paths, startup rollback and UUID ancestry. It is not a native IOKit execution result.
- The [storage ABI review](../out/native-a57/sd-block-v1/passes/native-v9b/native-block-bindings-review.json) passes 2,867 checks, including all 190 virtual slots and ten imports against the exact final kernel. The [C++ object review](../out/native-a57/sd-block-v1/native-block-driver-object-review.json) independently rebuilds the driver byte-for-byte, binds all 1,769 instruction words and 709 instruction/data relocations, and checks its ARMv8-A instruction inventory. The [nine C objects](../out/native-a57/sd-block-v1/native-objects-independent-review.json) also rebuild identically and bind to the final image.
- The [fixture review](../out/native-a57/sd-block-v1/native-block-fixture-review.json) checks the exact signed Mach-yield sequence, trust cache, mount arguments and result validation. Its SHA-256 is `d924fb8bfd12e6eb4559b9028522aaed88d444b1857beb73503f984925f84708`.
- The [package review](../out/research-native/native-xnu-v9-artifact-review.json) verifies all payload segments, zero fill, ADT/boot arguments, sidecars, 21 signature pages and a byte-identical loader rebuild. All [83 inert boot-script scenarios](../out/research-native/native-xnu-v9-script-review.json) and [35 mocked installer cases](../out/research-native/native-xnu-v9-updater-independent-review.json) pass. These checks do not operate the console.

## Physical result to capture

Close Hekate USB storage and choose **More Configs → IOSwitch Native XNU**. The V9 block stages follow the existing signed EL0 and FAT/split-file qualification. The actual banners are `BUILDING READ ONLY DISK INDEXES`, `READ ONLY BLOCK DEVICES PUBLISHED`, then `APFS VOLUMES FOUND - READING FILES`. Each is partial progress.

Success requires **`APFS FILES VERIFIED - READ ONLY`**, with `BLOCK TEST STAGE=5`, `BLOCK TEST ERROR=0`, `PUBLISH STATUS=0`, `VOLUME DISCOVERY STATUS=0`, `USER STAGE/ERROR=4`, `OS FILE BYTES=23E`, `SYSTEMOS FILE BYTES=C000` and `MATCHING FULL FILE HASHES=2`. All numbers on these rows are hexadecimal. A failure instead says `NATIVE APFS TEST REFUSED OR FAILED`; record the whole screen, including the stage, error, discovery attempts and elapsed-counter rows. An earlier FAT success banner alone does not establish V9 APFS success.

There is no measured V9 hardware duration or single wall-clock deadline for the entire boot. The prior FAT qualification and the subsequent index construction each have a 120-second transport budget, checked between sector reads; an in-flight command can add up to three one-second phase limits. Discovery makes at most 200 queries and checks a 120-second elapsed limit measured from the start of block setup, so that limit includes index construction rather than adding another guaranteed two-minute wait. Each later block read has its own five-second budget. APFS can issue many reads, and there is no separate watchdog around a mount or the whole sequence. These bounds therefore cannot promise a final screen within two minutes. The successful host model took 75.206 seconds; that is not a hardware ETA. The displayed counter is a checkpoint sample, not a continuously updating countdown. If progress stops, capture the visible stage and the observed wall-clock wait. Use the established [power-button exit procedure](native-xnu-run.md).

The remaining physical proof is the complete persistent SD → block service → original GPT/APFS → signed-process read → full-hash path on the OLED, including scheduler progress and the controller's inherited state across repeated reads. Local models do not establish Tegra interrupt delivery, physical timing or complete firmware memory ownership. Even a physical V9 success would establish these two read-only data mounts and files; authenticated root mounting, Apple's userspace, display/input integration and an iPadOS UI remain separate work.

# Native userspace fixture qualification

The single-CPU Cortex-A57 model runs our signed static PID 1 process at EL0, obtains PID 1 and a 4096-byte page size through real Darwin syscalls, and rejects the same process after a one-byte signed-page integrity change. This is model evidence for the native userspace candidate. The user has since reported the exact `EL0 USERSPACE VERIFIED - PID 1` banner on the OLED ([hardware result](../out/switch/native-xnu-v3-hardware-result.json)); no native SD driver, Apple launchd, iPadOS application or UI is part of this fixture.

The definitive model records are [model-pair-review.json](../out/native-a57/userspace-v1/model-pair-review.json) and the [packaged-ADT repeat](../out/native-a57/userspace-v1/runs/packaged-native-adt/result.json). An [independent review](../out/research-native/native-xnu-v3-model-review.json) checked the saved inputs, raw destination memory, instruction extents, actual EL0 registers, user buffers, negative control and final package relationship with no blocking finding. It did not rerun the model. Hardware instructions and current installation state are maintained separately in [native-xnu-run.md](native-xnu-run.md).

## Exact inputs and source provenance

| Input | SHA-256 |
| --- | --- |
| Final prelinked A57 kernel, 51,638,272 bytes | `a1d14b224fd6d86b5c943636615e9fc4dccd9f0bea0e165b42c0bd89ec3cec4e` |
| Source-built kernel before prelink | `d649928c67f43b45579a4266f3e4b9d3977ededfa41d7e20916f46a4a08f80f5` |
| Own signed process, 33,568 bytes | `9bd3704dbcd73ac4e614b0b0c1df7271d480f73b848cd59b1462528a0b19f3f5` |
| Own one-entry trust-cache bundle, 56 bytes | `02c0e1f23e8a5a0e385d6a71c41b424f08f89721a10d40de0d30227d3412ce59` |
| Original iPad7,1 / iPadOS 17.7.11 / 21H461 decompressed kernelcache | `f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7` |
| Preserved native v2 diagnostic kernel | `1c008af1b210918676228be29e2cdcc518deaa03f8d6534cc5176e79e1d153d6` |

The isolated prepared source tree is `out/native-a57/userspace-v1/tree`. Its ancestor pins, path isolation, compiler versions, exact build options, archive hashes, source snapshots and logged commands are bound by [reproducibility.json](../out/native-a57/userspace-v1/reproducibility.json). The final [build receipt](../out/native-a57/userspace-v1/passes/prelink-base/build-receipt.json) and [prelink metadata](../out/native-a57/userspace-v1/passes/prelink-base/kernel.json) remain alongside the candidate.

The original kernelcache comes from the Apple restore image pinned in [the board configuration](../vendor/unixnu-qemu/board.toml). NativePrelink selects `com.apple.driver.AppleMobileFileIntegrity` and its 11-component dependency closure, including ApplePMGR, AppleImage4, CoreTrust and corecrypto. Original executable segments retain their recorded hashes. Pointer bindings and the documented `-0x1000000` placement slide are applied; no unconditional entitlement-success replacement is introduced. The original AMFI early initializer is selected at `0xfffffff005ab4ae0` and relocated to `0xfffffff004ab4ae0`.

The process is our assembly test, named `launchd.macho` so the RAM fixture provides `/sbin/launchd`. It has no dynamic linker or Apple userspace code. The host signs it ad hoc using 4096-byte CodeDirectory blocks and explicitly trusts its sole CDHash, `c4095050fbbed8864ae3551faa9dae465d50c4d5`. This is a development trust input, not an Apple secure-boot or attestation claim. The [independent fixture review](../out/native-a57/userspace-v1/user-observer-independent-review.json) checks all nine original signing pages and the exact trust-cache entry.

## Changes from the diagnostic kernel

- The prepared source enables prelinked crypto, NativeAES, the runtime trust-cache path and the audited PMGR service bridge. The bridge uses the original ApplePMGRNub lifecycle and exact provider ABI; unsupported power methods panic. Its two AMFI AON methods use the original constant/no-op behavior. It does not implement Tegra power management. The [350-check binary review](../out/native-a57/userspace-v1/pmgr-independent-review-prelink-base.json) validates the final imported addresses, original code hashes and all 340 provider slots.
- The fixture performs `getpid`, `sysctl(CTL_HW, HW_PAGESIZE)` and a third `getpid`. The third trap establishes that userspace resumed and stored both earlier syscall results and carry flags.
- A compile-time and `ioswitch-user-test=1` gated observer runs after the actual syscall implementation. It requires PID 1, saved EL0 mode and exact post-`svc` fixture PCs. It reads the normal return values and uses `copyin` to check the user buffers; it does not change results, credentials or authentication decisions.
- The early diagnostic physical-base check moves to `0xb207c000`, matching the prelinked image's minimum virtual address `0xfffffff00427c000`. The managed window remains 768 MiB; the conservative rounded reservation envelope remains `0xb2000000..0xe2200000`.
- The native ADT fixture profile supplies checked RAMDisk/TrustCache placeholders, an empty manifest-properties node, product labels, RAM-disk boot environment, SHA-384 image policy and `amfi-only-platform-code=1`. Its board identity remains Nintendo OLED. The empty volatile NVRAM proxy has no persistent controller.

## Model procedure and observations

The model uses QEMU `virt,virtualization=on,secure=on,gic-version=2`, one Cortex-A57 CPU, TCG and 3 GiB guest RAM. The same native loader, layout packer and kernel are used, with native platform gates disabled for the model. Its EL3 bootstrap establishes the generic model environment; it is not an execution of Hekate or Tegra firmware.

Only these two ADT resources are changed for the model:

| Resource | Native description | Model description |
| --- | --- | --- |
| `arm-io/ranges` (`<3Q`) | child base 0, parent `0x50041000`, span `0x1100` | child base 0, parent `0x08000000`, span `0x10100` |
| `arm-io/intc/reg` (`<4Q`) | distributor offset 0 / size `0x1000`; CPU offset `0x1000` / size `0x100` | distributor offset 0 / size `0x1000`; CPU offset `0x10000` / size `0x100` |

The exact command line is:

```text
rd=md0 -v serial=3 debug=0x14e kt_fixed_zones=120 cpus=1 armcpu-diagnostics=1 armcpu-self-ipi=1 ioswitch-diag=1 ioswitch-user-test=1 sheng-pmgr=2 -no_slto_panic
```

The positive `platform-policy` run stopped at the actual EL0 loop PC `0x1000003dc` in 1.545 seconds. It sampled 315,632 KiB peak host RSS. The observer contains three completed syscalls, zero error, PID 1, `getpid=1`, `sysctl=0`, clear carry flags, page size 4096 and output length 4. The actual user buffers and CPU registers agree. PMGR reached stage 5/status 0 and the original AMFI interface pointer was `0xfffffff005cfc690`. This fast run recorded zero timer and IPI interrupts; it is not evidence of their native delivery or sustained operation.

The `packaged-native-adt` repeat used the exact newly packaged native ADT, including its fresh seed, with only the two model resource substitutions above. It reproduced the same EL0 result in 1.559 seconds at 317,232 KiB sampled peak RSS. Host RSS does not establish Switch memory use or fit.

![Model display showing the own signed PID 1 process and syscall results](../out/native-a57/userspace-v1/runs/packaged-native-adt/screen.png)

## Integrity negative control

The paired negative changes file offset `0xf00` from zero to one, in unused padding in the first signed page. Executable instructions, signature bytes, CodeDirectory and trust-cache entry are unchanged. Both runs use the exact same native/model ADTs and seed, kernel, command line and placements. Their entire 67,944,448-byte staged kernel window differs at that one fixture byte.

The modified process reaches no fixture syscall. The diagnostic records the real panic text:

```text
unexpected SIGKILL of launchd  with reason -- namespace 9 code 0x8 description none
```

The pinned XNU source defines namespace 9 as `OS_REASON_EXEC` and code 8 as `EXEC_EXIT_REASON_SECURITY_POLICY`. This is an explicit execution-policy rejection paired with a validated code-page integrity change. The negative result does not rely only on absence of a success banner. The [60-assertion checker](../out/native-a57/userspace-v1/verify-model-pair.py) binds the raw records, saved CPU/user state, input preservation, single-byte payload difference and actual rejection string.

## Reproducing and preserving evidence

The current prepared tree can produce a new candidate using a fresh output name:

```sh
python3 out/native-a57/userspace-v1/build.py rebuild-new-name --pmgr
```

The PMGR archive was built in two stages: a source-only reference candidate first, then the guarded bridge against that exact reference before the final prelinked build. The final reference kernel and archive are retained and hashed. A missing reference causes the upstream bridge builder to produce a first-pass stub, which is insufficient for this test. Require the actual bridge and repeat the binary review.

The one-shot preparation scripts document the transformations; do not run them again on the already prepared tree. A complete cold build from a fresh checkout has not been repeated. Compiler/linker UUIDs and environment changes can change artifact digests; any rebuilt candidate requires fresh signature, bridge and runtime qualification.

A local model repetition with the packaged ADT uses a fresh run name:

```sh
python3 out/native-a57/userspace-v1/model.py repeat-new-name \
  --kernel out/native-a57/userspace-v1/passes/prelink-base/kernel.macho \
  --adt out/native-userspace-v1/adt-final/kernel.adt --seconds 20
python3 out/native-a57/userspace-v1/verify-model-pair.py
```

The second command verifies the preserved paired runs; it does not silently qualify a newly named run. `record-reproducibility.py` additionally checks that the prior native v2 kernel and every file named in the v2 FAT32 package manifest still match their original hashes. Those checks passed after the new model work. Physical XNU memory ownership, Tegra interrupt routing and SD transport remain separate proof requirements. EL0 is now established by the user-reported physical banner, without a photograph or electronically acquired register dump.

# First native original-root candidate (V10)

V9's OLED result is owner-reported **APFS FILES VERIFIED - READ ONLY**. V10 attempts the original OS root and Apple startup, using the existing single Hekate entry. It is now installed, verified and safely ejected: all 12 new files passed readback, all 163 tracked existing files were preserved, and the retained 9.38 GB of media passed full SHA-256 verification. [Installation receipt](../out/switch/native-xnu-v10-install.json), [limited-attempt qualification](../out/research-native/native-xnu-v10-install-review.json). V10 has not yet been run on the OLED.

The original OS and SystemOS files remain read-only. The new Data volume is disposable RAM storage; its writes do not reach the SD card or eMMC and disappear at reboot. Existing Android, Ubuntu and user data are outside this path.

## Current local evidence

The isolated kernel in `out/native-a57/root-v1/` builds with the 17 original components needed for the initial startup path. The final candidate payload is 69,910,528 bytes (about 66.7 MiB), within the unchanged 80 MiB staging limit. The allocator still receives exactly 768 MiB; no larger Mac memory profile was substituted.

[The final corrected-seed run, positive-v1](../out/native-a57/root-v1/root-runs/root-final-positive-v1/result.json), mounted the original OS and verified SystemOS cache read-only, recorded Apple's root-authentication error `EAUTH=80`, and accepted the separately signed owner development policy. RAM Data mounted read-write with real filesystem owners `root=0:0` and `mobile=501:501`. Original `/sbin/launchd` exec returned success; the exact original dyld entry bytes were observed at EL0, followed by 498 genuine PID 1 system calls. Storage delivered 3,014,366,208 authenticated bytes with zero read or integrity failures.

The observer stopped after 464.8 seconds at its 1,000,000 KiB host RSS ceiling. Original launchd's `LC_MAIN` instruction was not observed, and RAM Data performed **zero writes**. This is a reviewed checkpoint for a first physical original-root attempt; it does not establish guest out-of-memory failure, service readiness, or a working iPadOS UI.

The corrected seed passed independent filesystem, ownership and full byte-difference verification. It preserves the 206 selected startup entries from `/private/var`, including logical contents, ownership and modes. It deliberately omits `/private/var/staged_system_apps` (29 app bundles, 4,214 files, 466,987,635 logical bytes); those remain in the immutable source image but are hidden by the Data mount. Restoring their usable path is later app integration work. The sole retained host metadata difference is a bounded `com.apple.provenance` marker; compressed source files are represented by their identical decompressed contents. The native-only mount gate checks real filesystem UID/GID attributes for the Data root and `mobile` directory, rejects ignored ownership, and stops before original exec if they differ. The final kernel and package bind the corrected seed, whose packed image is 1,642,576 bytes. Its initial resident pages use 1,642,496 bytes, under the 128 MiB cap.

[Positive-v2](../out/native-a57/root-v1/root-runs/root-final-positive-v2/observer-failure.json) used a 2 GiB host observer ceiling with the same 768 MiB guest allocation. Its observer failed to read virtual bytes at the expected main-PC branch before validating the checkpoint. No qualifying main execution or guest failure was captured. This partial diagnostic adds no main-execution claim.

All four refusal controls passed independent review: corrupted policy was rejected before device publication; consumed OS corruption and a transport timeout offlined storage with zero client bytes; disabling owner acceptance left Apple's `EAUTH=80` fatal before original exec. The disk and transport controls ended at bounded observer timeouts after the recorded storage refusals, rather than panics. The [aggregate review](../out/research-native/native-root-final-model-review.json) binds these controls and positive-v1 to the exact final inputs.

This is a Mac Cortex-A57 CPU/RAM/GIC model with a synthetic, read-only card transport backed by the full original image files. It is not a Tegra SD model or evidence of physical Apple userspace execution, graphics, input, or a working iPadOS UI.

## Root and write boundaries

Before root selection, the native path qualifies the card and FAT files, loads two exact existing signed policies, verifies their signatures and tables, and makes both envelopes and their descriptors permanently read-only. Only then are the two typed read-only block devices published. Each consumed block is verified in a private snapshot before client delivery; corruption or transport failure offlines both devices.

Owner root acceptance is restricted to the expected Apple `EAUTH` result, a read-only mount, the actual mounted device matching `rootdev`, and the uniquely identified original OS UUID under the verified native provider. SystemOS cannot substitute for the root role. The screen’s `OWNER` bit records policy eligibility; the separate kernel acceptance record is checked when interpreting controls. Apple's authentication result remains recorded; this is owner-authenticated development boot, not Apple authenticated boot.

RAM Data uses only the loader-owned `NativeDataSeed` input. The encoded seed is hashed before import, while a host-computed logical digest is pinned alongside that exact encoding. The sparse logical disk is capped at 1 GiB, with at most 128 MiB of resident pages including the seed. Short input copies, allocation failures and capacity limits refuse transactional writes without partially committing them. Persistent overlays and direct-disk modes are absent from this build.

## Review records

- [Final limited-attempt model review](../out/research-native/native-root-final-model-review.json)
- [Corrected Data seed independent review](../out/research-native/native-data-seed-final-review.json)
- [Final native/model build review](../out/research-native/native-root-v10-build-independent-review.json)
- [Exact package review](../out/research-native/native-xnu-v10-artifact-review.json)
- [Excluded staged-app inventory](../out/research-native/native-data-excluded-apps-review.json)
- [Policy and block implementation review](../out/research-native/native-root-policy-implementation.json)
- [Policy-loader failure tests](../out/native-a57/root-v1/policy-loader-host-tests.json)
- [Root-hook and observer review](../out/research-native/native-root-hook-review.json)
- [Loader, layout and ADT contract review](../out/research-native/native-root-host-contract-review.json)
- [Updater source review](../out/research-native/native-xnu-v10-updater-independent-review.json)

These are separate proof layers. Source and host-test reviews do not qualify a later changed kernel or package. Corrected-seed validation, exact package review, limited integrated startup/refusal review and verified installation are complete. The manual OLED boot remains unverified; follow the [current run instructions](native-xnu-run.md).

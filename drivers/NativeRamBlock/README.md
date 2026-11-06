# NativeRamBlock: isolated Switch native-root Data candidate

This isolated adaptation publishes a **disposable writable RAM disk** through the original IOStorageFamily ABI. It reads
only the 16-byte `NativeDataSeed` device-tree property, containing a physical address and the exact encoded byte length.
Legacy `RAMDisk` and `PackedRAMDisk` properties may coexist for a control fixture; this driver does not read them. It
has no transport writes, persistence, direct-disk mode or fallback to an unrelated RAM image.

The source history was a general RAM/persistent-overlay driver used by the reference UnixNU prototype.
`PersistOverlay.h`, `DirectDisk.h`, `SeedDisk.h` and their reference tests remain here as historical sources; they are
not included by this native candidate. The active suite is `Test/run_tests.py`.

## Seed and memory contract

`build-native-ram-block.py --seed <packed.dbr> --uuid <Data-volume-UUID>` pins the encoded length/SHA-256, logical
length/SHA-256, nonzero-page count and expected Data UUID in a generated `build/native-ram-block/NativeDataSeed.h`. A
changed seed requires new pins and a fresh source build. The host computes the digest over the complete logical disk,
streaming its zero gaps. On device the driver hashes **only the exact encoded bytes**, validates the packed layout and
logical size, and imports its nonzero pages. The compiled full-logical digest is then bound to that exact authenticated
input; there is no runtime 1 GiB hash operation and no Apple-authentication claim.

The loader owns the immutable encoded input. Its address must be page aligned, its rounded span must fit physical memory
and lie below `topOfKernelData`, and its byte length must equal the compiled value. The loader must reserve that span
without overlap before XNU starts. The driver never frees or modifies the input. Its own pointer table, request scratch
and sparse resident pages use the kernel allocator and are freed after the provider's last release. Startup publishes
nothing on failure and permits only one attempt per boot.

The accepted profile is at most 1 GiB logical and 32 MiB encoded. Resident 4 KiB data pages, including imported seed
pages, are capped at **128 MiB**. The pointer table is at most 2 MiB and the request scratch is 64 KiB. The current seed
has 401 nonzero pages (1,642,496 bytes), a 2,097,152-byte pointer table, a 65,536-byte scratch buffer and a
1,646,592-byte loader reservation: 5,451,776 bytes before object/allocator overhead. At the resident limit these four
contributions total 138,027,008 bytes before that overhead. The logical free space advertised by APFS can therefore
exceed available RAM backing; a new page at the cap returns an I/O allocation failure. This is a bounded startup
experiment, not a storage-capacity guarantee for complete services.

## I/O and discovery contract

Requests must be sector aligned, nonempty and no larger than 64 KiB. A sleepable IOKit lock serializes all I/O. A write
first copies the entire descriptor into private scratch, then obtains every missing page before committing any disk
byte. A short input copy, the resident cap or allocation failure leaves the disk unchanged and completes with zero
committed bytes. Reads return a truthful prefix on a short descriptor copy. Accepted requests complete exactly once
after unlocking, so a completion can reenter the driver. Cache synchronization waits behind accepted writes and promises
only RAM visibility. Format, unmap, eject and cache-mode changes are unsupported.

`native_ram_block_volume_seed()` requires the compiled UUID, a unique live matching media object, a valid observed BSD
name/device number and retained ancestry to this live typed provider. It returns `base=0`, the full logical length and
the original full-logical seed digest. That digest identifies the initial seed; it does not claim that later mutable RAM
contents still match it. A wrong/ambiguous UUID, stopped provider or unrelated ancestry refuses.

## Current local evidence and limits

The helper compiles the actual driver with `-march=armv8-a -mgeneral-regs-only`, then checks 190 ordered virtual slots,
25 concrete overrides and ten original storage imports against the preserved storage ABI audit. Its archive and object
remain at the conventional `build/native-ram-block/` paths. Final linked-candidate binding and instruction review remain
separate requirements.

`python3 drivers/NativeRamBlock/Test/run_tests.py` uses AddressSanitizer and UndefinedBehaviorSanitizer. It checks the
actual driver with mocked IOKit APIs and the actual packed seed: property isolation and encoded authentication,
allocation-failure cleanup, all request bounds, short-copy noncommit, write/readback, full resident-cap behavior,
completion reentry, two-thread serialization, stop handling, and UUID ancestry/ambiguity. It also runs 6,000
differential sparse operations and independently compares the C parser's complete logical digest with the host builder.
Reports are `build.json`, `bindings.json`, `host-tests.json` and `seed.json` in the build directory. These are not
execution of the actual kernel IOKit framework, APFS writable-mount proof or physical Switch evidence.

The corrected seed is `out/native-a57/root-v1/data-seed/data-v10.dbr`, expected UUID
`CCD5A1A2-2FDC-4B25-B322-AE1722871BC9`, encoded SHA-256
`fb156d6164613125844512e4d68ca6c6aed966da9e730c871b825229b53fef8d`, logical SHA-256
`76b24691beba497ebe2ecc37a3847824ed499b16561ec21d6484260c1c13463d`. It preserves 206 selected original `/private/var`
entries, including 39 logical files, with their exact modes, numeric UID/GID and protection class 0. The original
29-bundle `staged_system_apps` subtree is deliberately excluded: 10,789 entries and 466,987,635 logical file bytes. This
minimal baseline targets the first original launchd checkpoint; it does not establish complete original Data or
app-service composition.

The new host-formatted image has fresh inode allocation/timestamps, decoded file contents instead of original
compression attributes, and only one permitted host attribute: the observed 11-byte `com.apple.provenance` marker,
pinned and recorded per inode. The required symlink attribute is retained. `mobile/tmp` is absent so unchanged launchd
can create and fix it. The creation receipt is `out/native-a57/root-v1/data-seed/creation.json`; it binds the complete 1
GiB ownership-only byte diff, independent 7-Zip metadata/data-fork hashes and read-only `fsck_apfs` result. Original OS
images and the earlier seed remain unchanged. Host verification is separate from the original APFS driver's
writable-mount and real launchd execution evidence.

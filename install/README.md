# Install image

`bazel run //:image` makes a folder you merge onto the root of a Switch's SD card. It holds the hekate boot entry,
`bootloader/payloads/install-ios.bin`, and an image of the whole iOS partition, split into files FAT32 can hold. In
hekate, open Payloads and run `install-ios.bin`. It writes the image onto the card's Linux or Android partition and
turns that partition into iOS. Nothing else on the card changes.

```sh
bazel run //:image                        # the one Switch card in the Mac decides the size; folder in out/sd
bazel run //:image -- --size 64G          # no card in the Mac: give the partition's size exactly
bazel run //:image -- --copy-to-card      # write it all to the card, then eject it
bazel run //:image --config=ios27         # iPadOS 27.2 instead of 17.7.11
bazel run //:image_dry_run -- --size 64G  # checks and plan only, without the large IPSW downloads
bazel run //:image_boot -- --out out/sd   # only the boot entry again, after a kernel-only change
```

The image fits one partition size, because APFS lays the container out for the space it has. Make the partition in
hekate's partition manager first, then build with the card in the Mac. `install-ios.bin` refuses an image made for
any other size.

Composing happens in three stages, and each resumes where an interrupted run stopped: compose, populate and pack.
Populate asks for your password once. Setting the owners of iOS's files on the Mac needs root, and an image without
them doesn't boot. The scratch image lives in `~/Library/Caches/switch-ios/image`. It shrinks as pack writes the
part files, so the two never take the full space at once. With `--copy-to-card` the parts go straight onto the card.

Every install writes a new container with a new Data volume, so nothing from an earlier install survives. The first
boot takes a few minutes longer while iOS finishes setting itself up behind its own progress bar under the Apple logo.

`--//install:prebuilt=release` takes this project's own parts from the GitHub release of `MODULE.bazel`'s version
instead of building them (`--repo_env=SWITCH_RELEASE_DIR=<dir>` for a local copy of the assets). The release carries
nothing of Apple's: the IPSW is downloaded and assembled only on your Mac.

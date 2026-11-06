# Install image

`bazel run //:image` makes a folder you merge onto the root of a Switch's SD card. It holds
`bootloader/payloads/install-ios.bin`, the hekate entry, and an image of the whole iOS partition for one IPSW kind,
split into files FAT32 can hold. The image and that kind's boot files go in `switchroot/ios/install/<build>/`, so a card
can hold a 21H461 image and a 24B119 image side by side. Building one kind replaces only its own folder.

In hekate, open Payloads and run `install-ios.bin`, then pick an image with the volume buttons and POWER. It writes
the image onto the card's Linux or Android partition, turns that partition into iOS and copies the image's boot files
into `switchroot/ios/`, so hekate's iOS entry starts the matching kernel. The images stay on the card, so switching
kinds later takes one more run of the installer. Nothing else on the card changes.

```sh
bazel run //:image                        # the one Switch card in the Mac decides the size; folder in out/sd
bazel run //:image -- --size 64G          # no card in the Mac: give the partition's size exactly
bazel run //:image -- --copy-to-card      # write it all to the card, then eject it
bazel run //:image --config=ios27         # iPadOS 27.2 instead of 17.7.11
bazel run //:image_dry_run -- --size 64G  # checks and plan only, without the large IPSW downloads
bazel run //:image_boot -- --out out/sd   # only the kind's boot files and manifest again, after a kernel-only change
```

The image fits one partition size, because APFS lays the container out for the space it has. Make the partition in
hekate's partition manager first, then build with the card in the Mac. `install-ios.bin` refuses an image made for
any other size. Both kinds' images have to be built for the same partition.

Composing happens in three stages, and each resumes where an interrupted run stopped: compose, populate and pack.
Populate asks for your password once. Setting the owners of iOS's files on the Mac needs root, and an image without
them doesn't boot. The scratch image lives in `~/Library/Caches/switch-ios/image` (`image-<build>` for kinds other
than 21H461). It shrinks as pack writes the part files, so the two never take the full space at once. With
`--copy-to-card` the parts go straight onto the card. Without it, `out/sd` keeps the other kind's image from an
earlier run, so one `ditto` puts both on the card.

Only `install-ios.bin` and `bazel run //deploy:sd` write the boot files hekate starts, so copying one kind's image onto
a card running the other doesn't change what boots. Deploy writes the boot files of the
kind it was built for and refuses a card whose iOS is the other kind.

Every install writes a new container with a new Data volume, so nothing from an earlier install survives. The first
boot takes a few minutes longer while iOS finishes setting itself up behind its own progress bar under the Apple logo.

An image from before the per-kind folders (`switchroot/ios/install/manifest.bin`) has no boot files of its own, so
the installer doesn't offer it. `--copy-to-card` deletes it; after a `ditto`, delete it yourself.

`--//install:prebuilt=release` takes this project's own parts from the GitHub release of `MODULE.bazel`'s version
instead of building them (`--repo_env=SWITCH_RELEASE_DIR=<dir>` for a local copy of the assets). The release carries
nothing of Apple's: the IPSW is downloaded and assembled only on your Mac.

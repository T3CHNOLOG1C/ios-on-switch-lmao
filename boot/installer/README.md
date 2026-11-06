# install-ios.bin

A hekate payload that writes the install image `bazel run //:image` put on the FAT
(`switchroot/ios/install/`) onto the partition it was made for. It runs on the BPMP with hekate's BDK. hekate is
GPL-2.0-only, and so is this payload.

```sh
bazel build //boot/installer:install-ios.bin
bazel test //boot/installer/test:installer_test
```

It offers the card's Linux (`l4t`) or Android (`userdata`, `UDA`) partition, or the iOS partition an earlier install
made. The image must be exactly that partition's size. After VOL+ it does this:

1. The GPT entry becomes Linux-typed `iOS-setup`, so a half-written partition never boots or gets offered to
   Linux or Android.
2. It reads the image 8 MiB at a time. The security engine checks each chunk's SHA-256 against the manifest
   before any of the chunk is written.
3. It writes the image's extents, with the container's checkpoint areas written whole. The partition's other blocks
   keep their old bytes, which the new container never reads.
4. The entry becomes APFS `iOS`, the name the kernel looks for, and the image's files are deleted from the FAT.

Every SD write goes through `__wrap_sdmmc_storage_write`, so FatFs's and the BDK's writes go there too. It allows
only the range that step needs and refuses everything else. The screen is drawn 1280 x 720, upright when the
Switch is held in handheld mode.

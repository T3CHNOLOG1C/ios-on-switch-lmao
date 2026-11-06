# install-ios.bin

A hekate payload that writes one of the install images `bazel run //:image` put on the FAT
(`switchroot/ios/install/<build>/`) onto the partition it was made for, then makes that image's boot files the ones
hekate's iOS entry starts. It runs on the BPMP with hekate's BDK. hekate is GPL-2.0-only, and so is this payload.

```sh
bazel build //boot/installer:install-ios.bin
bazel test //boot/installer/test:all
```

It lists every image folder it finds. If an image's manifest doesn't decode, names files outside its folder, or is
missing a part or boot file at the size it records, the list shows why and the image can't be picked. VOL+ and VOL-
move between the others and POWER picks one. The target is the card's Linux (`l4t`) or Android (`userdata`, `UDA`)
partition, or the iOS partition an earlier install made, and it must be exactly the size the image was made for. A
warning screen follows: POWER installs (it ignores buttons for the first second), VOL+ or VOL- goes back. Then:

1. It reads every boot file and checks it against the manifest's SHA-256s. Nothing has been written yet.
2. The GPT entry becomes Linux-typed `iOS-setup`, so a half-written partition never boots or gets offered to
   Linux or Android.
3. The boot files go into `switchroot/ios/`. Each 8 MiB chunk is checked before it's written and read back after.
   `bootargs.txt` is never touched.
4. It reads the image 8 MiB at a time. The security engine checks each chunk's SHA-256 against the manifest
   before any of the chunk is written.
5. It writes the image's extents, with the container's checkpoint areas written whole. The partition's other blocks
   keep their old bytes, which the new container never reads.
6. The entry becomes APFS `iOS`, the name the kernel looks for. The image stays on the FAT for the next switch.

Every SD write goes through `__wrap_sdmmc_storage_write`, so FatFs's and the BDK's writes go there too. It allows
only the range the current step needs (the FAT for step 3, the partition for step 5, the GPT's four ranges for 2
and 6) and refuses everything else. The screen is drawn 1280 x 720, upright when the Switch is held in handheld mode.

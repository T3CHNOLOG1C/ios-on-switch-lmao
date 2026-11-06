# boot

Everything that runs between hekate and XNU: the hekate entry, U-Boot's half of the handoff, and the native loader.

```
bazel build //:boot                    # debug
bazel build //:boot --config=release   # release
```

Both land in `bazel-bin/boot/ios`. `//deploy:sd` copies it to the card.

## On the card

| Path | What |
|---|---|
| `bootloader/ini/ios.ini` | The hekate entry `iOS` (`//boot:ios_ini`). |
| `switchroot/ios/bl31.bin` | switchroot's ATF, pinned. |
| `switchroot/ios/bl33.bin` | switchroot's U-Boot with everything else of ours behind it. |
| `switchroot/ios/bootargs.txt` | Yours, optional. No build ships it, and deploy never touches it. |

hekate loads all of `bl33.bin` to `0xAA000000` and runs U-Boot. U-Boot's built-in environment is edited so that it sources
the boot script from inside the file instead of looking for one on the FAT. `tools/package/loader_abi.py` keeps the map:

| Address | What |
|---|---|
| `0xAA000000` | U-Boot, unchanged apart from its default environment (`tools/package/bl33.py`) |
| `0xAA070000` | the boot script (`u-boot-script.txt`) |
| `0xAA080000` | the loader (`loader/`) |
| `0xAA0B0000` | the SD exporter (`storage/firmware_export/`) |
| `0xAA0C0000` | the board DTB |
| `0xAA100000` | the kernel window, LZ4-compressed, between a header and a trailer page (`loader/stage.h`) |

The script checks the board, U-Boot's build and the staged window, and reads `bootargs.txt`. It then prepares the FDT,
runs the exporter so the kernel can adopt the SD card U-Boot brought up, and boots the loader. The loader checks memory,
unpacks the window into place, appends `bootargs.txt` and jumps to XNU.

U-Boot stays because the kernel builds on what it does: it brings up the SD card, reads the FAT, and fixes up the FDT
in bootm. Keeping the loader, the script and the DTB inside `bl33.bin` means hekate reads them in one go at SDR104,
and U-Boot opens at most one small file.

## bootargs.txt

Plain text: words separated by spaces or new lines. A `#` at the start of a word starts a comment that runs to the end
of the line. The words go after the build's own boot-args. With no file, nothing is added.

Everything is on by default. The few args below exist only to turn something off while you hunt a problem:

| Arg | Turns off |
|---|---|
| `switch-no-bt` | Bluetooth, Joy-Con pairing over the rails, and audio (bluetoothd and audiomxd go together) |
| `switch-no-wlan` | Wi-Fi |
| `switch-no-gpu` | The GPU and Metal (a stand-in accelerator takes its place) |
| `switch-no-nvdec` | The hardware video decoder and encoder |
| `switch-no-audio` | The speakers and headphone jack (a null sink takes their place) |
| `switch-no-sleep` | Turning the panel off on lock, and with it system sleep |
| `switch-no-sd-uhs` | The SD card's 200 MHz UHS-I mode (it stays in U-Boot's 48 MHz mode) |
| `switch-no-dfll` | The CPU's on-demand clock (a fixed 1670 MHz instead) |

Both builds' own line is `-apfs_shared_datavolume`: Apple's AppleKeyStore reads it, and its other source, the ADT's
`/filesystems e-apfs`, would also switch apfs.kext into a mode that hasn't run here yet.

`-v` shows the text console instead of the Apple logo, in debug builds only. For a debug boot with the console:

```
# switchroot/ios/bootargs.txt
-v
```

If the file can't be used, the boot stops on screen:

| Message | Why |
|---|---|
| `REFUSED: BOOTARGS_TXT_TOO_LARGE` | The file is 4 KiB or more. |
| `LOADER REFUSED`, code `761` | The whole line would be longer than 1023 bytes. |
| `LOADER REFUSED`, code `762` | A word has a byte that isn't printable ASCII (a byte order mark at the start is fine). |

## Code signing

XNU's RELEASE configuration ignores `cs_integrity_skip=1`. Without it, this 4 KiB-page kernel reads and hashes a
binary's whole code range when its signature is added (Apple signs in 16 KiB pages, so the kernel derives 4 KiB
digests up front), then hashes each code page again when it faults in. Both builds now skip that page hashing with no
boot-arg. Each binary's CDHash is still checked against the trust caches when it loads. With no secure boot, anyone
who can change a file on the card can replace the kernel too, so the hashing costs boot time and protects nothing.

# hw_test_app

A SwiftUI app that tests the Switch's hardware through public iOS APIs: Metal, the hardware video decoders and
encoders, images, audio, display, sensors, input, networking, storage, CPU and crypto. It runs the whole suite once
at launch. Debug images only: the debug `//:boot` puts it in `/Applications` through the overlay, and
`bazel run //deploy:sd` leaves datamigrator's migration sentinel when it changes, so LaunchServices registers it on the
next boot the way it registers an OS update's apps. `bazel build //hw_test_app:SwitchHWTest` builds it with
rules_apple, signed ad hoc. The test clips and images are pinned in `bazel/pinned/hw_test_app/media`; every frame
carries a barcode with its frame number and a CRC, so the app checks lossy decoder output exactly.
`bazel run //hw_test_app:media_tool -- <dir>` remakes them (VideoToolbox on the Mac for H.264 and HEVC, libvpx for
VP9, libjpeg-turbo for the JPEGs) and prints the SHA256SUMS lines to pin; the Mac's 4K encodes differ from run to run,
which is why the files are pinned rather than built. Each test logs one `HWTEST <id> <PASS|FAIL|SKIP> <k=v ...>` line
to os_log under `com.brunostjohn.switch.hwtest` (`//tools/devicerun` checks them) and lands in `report.json` in the
app's Documents folder. Tapping a test in the list reruns it.

#!/bin/sh
# Host simulation of the stage-4 stack: NativeBridgeMetalFrontend.mm + the metal2vulkan bridge + NVK (macOS host build,
# nvkmd_darwin) over mock_iokit.c, a GM20B user client that completes every submission at once. It proves device creation,
# metal2vulkan's acceptance of NVK's feature set and NAK's SM53 compile of QuartzCore's shaders; nothing executes on a GPU.
# qc_frontend.mm: QuartzCore's pipelines through the frontend (sheng's 24 recipes, the framebuffer-fetch filters, the uber
# shader under 33 constant sets), each made and drawn three times in one pass: <outdir>/qc_frontend <nvk> <cache> <QC metallib>.
# present_frontend.mm: the command buffer's presentDrawable: contract (Metal's): <outdir>/present_frontend <nvk> <cache>.
# memory_frontend.mm: what a client's GPU memory costs it as iOS charges it (iokit_mapped), memoryless textures sharing a
# backing, and the idle blocks going back: <outdir>/memory_frontend <nvk> <cache> [<width>x<height> ...] (default: the widgets of
# WidgetRenderer's 23:44 memory warning).
# usage: build.sh <host libnative_metal_bridge.a> <outdir>
#   the bridge: bazel build //gpu:bridge_darwin (bazel-bin/gpu/bridge_darwin/libnative_metal_bridge.a)
#   then: <outdir>/m2v_gm20b <host libvulkan_nouveau.dylib> <cache dir> [<QuartzCore default.metallib> <pairs>]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=$HERE/../../..
mkdir -p "$2"
xcrun clang -O1 -Wall -dynamiclib -I "$TREE/drivers/TegraGM20B" "$HERE/mock_iokit.c" -framework IOKit -framework CoreFoundation \
    -install_name @rpath/libmock_iokit.dylib -o "$2/libmock_iokit.dylib"
xcrun clang++ -O1 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -I "$TREE/drivers/NativeMetal" -x objective-c++ \
    "$HERE/m2v_gm20b.mm" "$TREE/drivers/NativeMetal/NativeBridgeMetalFrontend.mm" -x none "$1" -L "$2" -lmock_iokit \
    -Wl,-rpath,"$2" -lobjc -framework Foundation -framework Metal -framework IOSurface -framework IOKit -o "$2/m2v_gm20b"
xcrun clang++ -O1 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -I "$TREE/drivers/NativeMetal" -x objective-c++ \
    "$HERE/qc_frontend.mm" "$TREE/drivers/NativeMetal/NativeBridgeMetalFrontend.mm" -x none "$1" -L "$2" -lmock_iokit \
    -Wl,-rpath,"$2" -lobjc -framework Foundation -framework Metal -framework IOSurface -framework IOKit -o "$2/qc_frontend"
xcrun clang++ -O1 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -I "$TREE/drivers/NativeMetal" -x objective-c++ \
    "$HERE/present_frontend.mm" "$TREE/drivers/NativeMetal/NativeBridgeMetalFrontend.mm" -x none "$1" -L "$2" -lmock_iokit \
    -Wl,-rpath,"$2" -lobjc -framework Foundation -framework Metal -framework IOSurface -framework IOKit -o "$2/present_frontend"
xcrun clang++ -O1 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -I "$TREE/drivers/NativeMetal" -I "$HERE" \
    -x objective-c++ "$HERE/memory_frontend.mm" "$TREE/drivers/NativeMetal/NativeBridgeMetalFrontend.mm" -x none "$1" \
    -L "$2" -lmock_iokit -Wl,-rpath,"$2" -lobjc -framework Foundation -framework Metal -framework IOSurface -framework IOKit \
    -o "$2/memory_frontend"
echo "built $2/m2v_gm20b $2/qc_frontend $2/present_frontend $2/memory_frontend"

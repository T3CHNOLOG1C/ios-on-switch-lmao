#!/bin/sh
# raw GM20B path (r7-raw, r8-rawca) host build: the mock GPU with nvdec (pack/), nvkframe (frontend + host bridge + NVK twin),
# rawframe (raw C++ API), facadeframe (RawMetal.mm over the frontend as its m2v fallback), blitframe (copies), orderframe (the
# two drivers' order on the GPU, over the mock's asynchronous GPU) and cabench (CPU per frame).
# usage: test/build.sh <host libnative_metal_bridge.a> <frontend dir (NativeBridgeMetalFrontend.{mm,h})> <tree> <outdir>
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
RAW=$HERE/..
BRIDGE=$1; FRONT=$2; TREE=$3; OUT=$4
mkdir -p "$OUT"
FLAGS="-O2 -g -Wall -Wextra -std=c++17"
for f in mme_fermi mme_fermi_sim; do xcrun clang -O2 -std=c11 -w -I "$RAW/pack/mesa" -c "$RAW/pack/mesa/$f.c" -o "$OUT/$f.o"; done
xcrun clang -O1 -Wall -c -I "$TREE/drivers/TegraGM20B" "$RAW/pack/mock_iokit.c" -o "$OUT/mock_iokit.o"
xcrun clang++ $FLAGS -c -I "$TREE/drivers/TegraGM20B" "$RAW/pack/cap.cpp" -o "$OUT/cap.o"
# the mock's decoder runs the 2D engine's blits too (raw/fx/test/twod_sim: the mip chains)
xcrun clang++ $FLAGS -DNVDEC_TWOD -c "$RAW/pack/nvdec.cpp" -o "$OUT/nvdec.o"
xcrun clang++ $FLAGS -c "$RAW/fx/test/twod_sim.cpp" -o "$OUT/twod_sim.o"
xcrun clang++ -dynamiclib "$OUT/mock_iokit.o" "$OUT/cap.o" "$OUT/nvdec.o" "$OUT/twod_sim.o" "$OUT/mme_fermi.o" "$OUT/mme_fermi_sim.o" -framework IOKit \
  -framework CoreFoundation -install_name @rpath/libmock_iokit.dylib -o "$OUT/libmock_iokit.dylib"
OBJC="-O2 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -I $FRONT -I $TREE/drivers/NativeMetal -I $TREE/drivers/TegraGM20B"
LIBS="-L $OUT -lmock_iokit -Wl,-rpath,$OUT -lobjc -framework Foundation -framework Metal -framework IOSurface -framework IOKit -framework CoreFoundation"
xcrun clang++ $OBJC -x objective-c++ "$HERE/nvkframe.mm" "$FRONT/NativeBridgeMetalFrontend.mm" -x none "$BRIDGE" $LIBS -o "$OUT/nvkframe" 2>&1 | grep -v "duplicate libraries" || true
xcrun clang++ $FLAGS -I "$TREE/drivers/TegraGM20B" "$HERE/rawframe.cpp" "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" "$RAW/runtime/raw_kernel.cpp" \
  "$RAW/runtime/raw_pack.cpp" $LIBS -o "$OUT/rawframe"
xcrun clang++ $OBJC -Wall -Wextra -DRAW_FACADE -x objective-c++ "$HERE/nvkframe.mm" "$RAW/runtime/RawMetal.mm" "$FRONT/NativeBridgeMetalFrontend.mm" \
  -x c++ "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_pack.cpp" -x none "$BRIDGE" $LIBS \
  -o "$OUT/facadeframe" 2>&1 | grep -v "duplicate libraries" || true
echo "built $OUT/nvkframe $OUT/rawframe $OUT/facadeframe"
xcrun clang++ $OBJC -Wall -Wextra -x objective-c++ "$HERE/blitframe.mm" "$RAW/runtime/RawMetal.mm" "$FRONT/NativeBridgeMetalFrontend.mm" \
  -x c++ "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_pack.cpp" -x none "$BRIDGE" $LIBS \
  -o "$OUT/blitframe" 2>&1 | grep -v "duplicate libraries" || true
echo "built $OUT/blitframe"
xcrun clang++ $OBJC -Wall -Wextra -x objective-c++ "$HERE/orderframe.mm" "$RAW/runtime/RawMetal.mm" "$FRONT/NativeBridgeMetalFrontend.mm" \
  -x c++ "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_pack.cpp" -x none "$BRIDGE" $LIBS \
  -o "$OUT/orderframe" 2>&1 | grep -v "duplicate libraries" || true
echo "built $OUT/orderframe"
xcrun clang++ $OBJC -Wall -Wextra -x objective-c++ "$HERE/selcheck.mm" "$RAW/runtime/RawMetal.mm" "$FRONT/NativeBridgeMetalFrontend.mm" \
  -x c++ "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_pack.cpp" -x none "$BRIDGE" $LIBS \
  -o "$OUT/selcheck" 2>&1 | grep -v "duplicate libraries" || true
echo "built $OUT/selcheck"
xcrun clang++ $OBJC -Wall -Wextra -x objective-c++ "$HERE/planarframe.mm" "$RAW/runtime/RawMetal.mm" "$FRONT/NativeBridgeMetalFrontend.mm" \
  -x c++ "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_pack.cpp" -x none "$BRIDGE" $LIBS \
  -o "$OUT/planarframe" 2>&1 | grep -v "duplicate libraries" || true
echo "built $OUT/planarframe"
xcrun clang++ $OBJC -x objective-c++ "$HERE/cabench.mm" "$RAW/runtime/RawMetal.mm" "$FRONT/NativeBridgeMetalFrontend.mm" \
  -x c++ "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_pack.cpp" -x none "$BRIDGE" $LIBS \
  -o "$OUT/cabench" 2>&1 | grep -v "duplicate libraries" || true
echo "built $OUT/cabench"
xcrun clang++ $OBJC -Wall -Wextra -x objective-c++ "$HERE/mipframe.mm" "$RAW/runtime/RawMetal.mm" "$FRONT/NativeBridgeMetalFrontend.mm" \
  -x c++ "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_pack.cpp" -x none "$BRIDGE" $LIBS \
  -o "$OUT/mipframe" 2>&1 | grep -v "duplicate libraries" || true
echo "built $OUT/mipframe"

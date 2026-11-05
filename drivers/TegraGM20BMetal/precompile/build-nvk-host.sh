#!/bin/zsh
# The host twin of an iOS NVK dylib, for the shader cache precompile (//gpu:seed, precompile.mm): the same Mesa
# checkout built for this Mac (aarch64-apple-darwin) with the darwin nvkmd backend, so NAK compiles exactly as on the device. Run
# it on the checkout the iOS dylib was built from, as it was (build-nvk-ios.sh leaves it so), with the same NVK_BUILD_ID (the pin,
# when the iOS build had one); both dylibs then get <dylib>.source (nvk-source.sh), which the build step requires to be equal.
# Unpinned, the build step gives the twin the iOS dylib's LC_UUID, which NVK's pipelineCacheUUID then hashes as on the device.
# The options are the iOS build's (release, no shader cache, mesa_clc and vtn_bindgen2 from nativebin) but the cross file.
# <tools> is the directory build-nvk-ios.sh ran in: its nativebin/ (mesa_clc, vtn_bindgen2) and venv/ (mako). ~1 min, ~360 MB.
#   [NVK_BUILD_ID=<hex>] build-nvk-host.sh <mesa checkout> <build dir> <tools> <out dylib> [<iOS dylib to write the .source of too>]
set -e
SRC=${1:A}; BUILD=${2:A}; TOOLS=${3:A}; OUT=${4:A}; IOS=${5:+${5:A}}
HERE=${0:A:h}
TREE=${HERE:h:h:h}   # tree/ (drivers/TegraGM20BMetal/precompile -> tree)
export PATH=$TOOLS/nativebin:$TOOLS/venv/bin:/opt/homebrew/opt/llvm/bin:$PATH
export BINDGEN_EXTRA_CLANG_ARGS="--target=arm64-apple-macosx14.0 -isysroot $(xcrun --show-sdk-path)"
[[ -d $BUILD ]] || (cd $SRC && meson setup $BUILD -Dvulkan-drivers=nouveau -Dgallium-drivers= -Dplatforms= -Dglx=disabled \
  -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dgbm=disabled -Dllvm=disabled -Dmesa-clc=system \
  -Dprecomp-compiler=system -Dbuildtype=release -Db_ndebug=true -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled \
  -Dzlib=disabled -Dexpat=disabled -Dxmlconfig=disabled -Dshader-cache=disabled -Dnvk-darwin-abi-dir=$TREE/drivers/TegraGM20B)
meson configure $BUILD -Dnvk-build-id=${NVK_BUILD_ID:-}
ninja -C $BUILD src/nouveau/vulkan/libvulkan_nouveau.dylib
cp $BUILD/src/nouveau/vulkan/libvulkan_nouveau.dylib $OUT
sh $HERE/nvk-source.sh $SRC ${NVK_BUILD_ID:-} > $OUT.source
[[ -z $IOS ]] || cp $OUT.source $IOS.source
echo "$OUT ($(cat $OUT.source))"

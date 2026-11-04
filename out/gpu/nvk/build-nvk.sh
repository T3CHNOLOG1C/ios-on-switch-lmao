#!/bin/zsh
# r8-webgpu: the iOS NVK dylib and its host twin from r8-webgpu/mesa (r6-render's 4440332 = the deployed r6 dylib's tree 1ceaf6f,
# + patches/nvk-0004-sandbox-safe-darwin.patch + nvk-0005-join-copy-regions.patch), then relink-nvk.py (absent entrypoints
# absolute at 0: no flat-namespace lookups at dlopen), with r6-render's recipe (build-nvk-ios-r6.sh) and r5-m2vfast's host tools.
# NVK_BUILD_ID unset, as r6: the pipeline cache UUID hashes the dylib's LC_UUID (the twin takes it from the iOS dylib at the
# bundle build). Outputs nvk-out/libvulkan_nouveau.dylib (+ .source) and nvk-out/libvulkan_nouveau.host.dylib (+ .source).
set -e
S=${0:A:h}; T=$S/../r5-m2vfast/nvk
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
ABI=/Users/brunostjohn/Development/Projects/ioswitch/out/native-a57/root-v1/tree/drivers/TegraGM20B
PRE=/Users/brunostjohn/Development/Projects/ioswitch/out/native-a57/root-v1/tree/drivers/TegraGM20BMetal/precompile
export PATH=$T/nativebin:$T/venv/bin:/opt/homebrew/opt/llvm/bin:$PATH
mkdir -p $S/nvk-out
(
  export BINDGEN_EXTRA_CLANG_ARGS="--target=arm64-apple-ios17.0 -isysroot $SDK"
  [[ -d $S/nvk-ios-build ]] || (cd $S/mesa && meson setup $S/nvk-ios-build --cross-file $S/ios-cross.ini -Dvulkan-drivers=nouveau \
    -Dgallium-drivers= -Dplatforms= -Dglx=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dgbm=disabled \
    -Dllvm=disabled -Dmesa-clc=system -Dprecomp-compiler=system -Dbuildtype=release -Db_ndebug=true -Dvalgrind=disabled -Dlibunwind=disabled \
    -Dzstd=disabled -Dzlib=disabled -Dexpat=disabled -Dxmlconfig=disabled -Dshader-cache=disabled -Dnvk-darwin-abi-dir=$ABI)
  meson configure $S/nvk-ios-build -Dnvk-build-id=
  ninja -C $S/nvk-ios-build src/nouveau/vulkan/libvulkan_nouveau.dylib
  python3 $S/relink-nvk.py $S/nvk-ios-build
  cp $S/nvk-ios-build/src/nouveau/vulkan/libvulkan_nouveau.dylib $S/nvk-out/libvulkan_nouveau.dylib
  sh $PRE/nvk-source.sh $S/mesa > $S/nvk-out/libvulkan_nouveau.dylib.source
)
zsh $PRE/build-nvk-host.sh $S/mesa $S/nvk-host-build $T $S/nvk-out/libvulkan_nouveau.host.dylib
python3 $S/relink-nvk.py $S/nvk-host-build
cp $S/nvk-host-build/src/nouveau/vulkan/libvulkan_nouveau.dylib $S/nvk-out/libvulkan_nouveau.host.dylib
otool -L $S/nvk-out/libvulkan_nouveau.dylib
cat $S/nvk-out/*.source

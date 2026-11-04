#!/bin/sh
# Write our metal2vulkan changes (the working tree against tag ioswitch-base6 = 720ef1ce + NativeMetal patches 0001-0006) as
# the numbered series //gpu:bundle applies (patches/series): <out dir>/0001-*.patch ...
set -e
M=$1; OUT=$2
mkdir -p "$OUT"; rm -f "$OUT"/*.patch
p() { # name subject body files...
  name=$1; subject=$2; body=$3; shift 3
  { printf 'From: ioswitch <noreply@ioswitch.local>\nSubject: [PATCH] %s\n\n%s\n---\n' "$subject" "$body"
    git -C "$M" diff ioswitch-base6 -- "$@"; } > "$OUT/$name"
}
p 0001-unordered-framebuffer-fetch.patch "Run framebuffer fetch and raster order groups unordered where neither ordered form exists" \
"NVK on Maxwell (GM20B) has neither rasterizationOrderColorAttachmentAccess nor fragmentShaderPixelInterlock, so every QuartzCore
pipeline that reads its color attachment (p3_clamp, edr_*, harmony, display_overdrive_*, luma_boost, path_blit_*) and the
raster-order-group inplace_copy were refused even under FramebufferFetchPolicy::AcceptUnordered: the compile layer emitted
the unordered input attachments and the runtime refused them. Under AcceptUnordered on such a device the runtime now runs
that form: color targets are input attachments as in the ordered form, no rasterization-order flags are stated, and a draw
that fetches after another draw of its render pass instance records the draws' self-dependency barrier first
(InterlockedStorage::inputs). Raster order groups on a function's own buffers and textures run without the critical
section. Trade-off, recorded as PrecisionNote::UnorderedFramebufferFetch: overlapping fragments of one draw read or write a
pixel in no defined order; draws stay ordered." \
  crates/metal2vulkan-runtime/src/framebuffer_fetch.rs crates/metal2vulkan-runtime/src/render.rs \
  crates/metal2vulkan-runtime/src/render/pending.rs crates/metal2vulkan-runtime/src/render_target.rs \
  crates/metal2vulkan-runtime/src/pipeline/graphics.rs crates/metal2vulkan-runtime/src/pipeline/render.rs \
  crates/metal2vulkan/src/device/profile.rs crates/metal2vulkan/src/lower/fragment/mod.rs crates/metal2vulkan/src/lower/fragment/rog.rs \
  crates/metal2vulkan-runtime/src/tests/framebuffer_fetch.rs crates/metal2vulkan/src/tests/engine_fragment.rs
p 0002-core-image-imageblock-role.patch "Decode Core Image's air.ci_imageblock role and pointer layouts" \
"iOS 17's ci_stdlib and ci_stdlib_h declare their _ci_*_block helpers with an air.ci_imageblock parameter, and
CIPortraitBlurV2's slm_calcV2 states air.struct_type_info on an air.ci_pointer: each was an unknown key, so the whole module
(every function of the library) failed to decode. The role is decoded (a struct holding a pointer); the layout is read and
not kept." \
  crates/metal2vulkan-air/src/abi/keys.rs crates/metal2vulkan-air/src/abi/mod.rs crates/metal2vulkan-air/src/abi/core_image.rs \
  crates/metal2vulkan-air/src/abi/check.rs
p 0003-llvm-bitreverse.patch "Lower llvm.bitreverse as air.reverse_bits" \
"ARKitCore's blur_cubemap calls llvm.bitreverse.i32, an unknown intrinsic." \
  crates/metal2vulkan-air/src/intrinsics/llvm.rs crates/metal2vulkan/src/lower/arith/mod.rs \
  crates/metal2vulkan-air/src/intrinsics/tests.rs
# everything must be in some patch
covered=$(cat "$OUT"/*.patch | grep '^diff --git' | wc -l)
total=$(git -C "$M" diff ioswitch-base6 --stat | grep '|' | wc -l)
[ "$covered" -eq "$total" ] || { echo "patches cover $covered of $total changed files" >&2; exit 1; }
echo "wrote $(ls "$OUT"/*.patch | wc -l) patches, $covered files"

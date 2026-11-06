#!/bin/zsh
# raw GM20B path (r8-rawca r18, r7u's striped share-sheet thumbnails): what the raw path samples, texel for texel, over the mock
# GPU (nvdec NVDEC_TEXELS hashes the texels every pitch texture header of a draw names, read row by row as the GPU reads them):
# a 4:2:0 texture's conversion draw must sample its surface's planes (luma and chroma), its fragment buffer the surface's size
# and range, and the frame's shared A8 corner mask (written by replaceRegion into write-combined memory) its texels.
# usage: texels.sh <build dir> <pack dir> <out dir> <case>...   a case: layout[:full][:WxH], layout exact, padded or pitch16
B=$1
P=$2
O=$3
shift 3
NVK=${NVK:-${0:A:h}/../../../../bazel-bin/gpu/nvk/host/libvulkan_nouveau.dylib} # bazel build //gpu/nvk:host
QC=${QC:-${0:A:h}/../../eq/qc/QuartzCore.framework/default.metallib}
C=${CORPUS:-${0:A:h}/../../precompile/corpus.log}
mkdir -p $O
export RAWCAP_PROGRAMS=$P/work/programs.bin RAWCAP_LAYOUTS=$P/layouts.txt NVDEC_REGS=1 NVDEC_TEXELS=1 NVDEC_HEAD=1 MESA_SHADER_CACHE_DISABLE=true NMB_ASYNC_PSO=0 NMB_ASYNC_PSO_API=0
fail=0
for c in "$@"; do
    v=${c%%:*}
    rest=${c#$v}
    full=
    size=
    [[ $rest == *:full* ]] && full=1
    [[ $rest =~ ':([0-9]+x[0-9]+)' ]] && size=$match[1]
    n=${c//:/-}
    mkdir -p $O/cache-$n
    env RAWCAP_LOG=$O/$n.log FRAME_YCBCR=$v FRAME_MASK=1 ${full:+FRAME_YCBCR_FULL=1} ${size:+FRAME_YCBCR_SIZE=$size} FRAME_FALLBACK=$NVK \
        $B/facadeframe $P/pack.bin $O/cache-$n $QC $C 2 >$O/$n.out 2>$O/$n.err || {
        echo "$c: facadeframe failed"
        tail -3 $O/$n.err
        fail=1
        continue
    }
    luma=$(sed -n 's/^YCBCR texels luma=\([0-9a-f]*\) .*/\1/p' $O/$n.out | head -1)
    chroma=$(sed -n 's/^YCBCR texels .* chroma=\([0-9a-f]*\)$/\1/p' $O/$n.out | head -1)
    shape=$(sed -n 's/^YCBCR shape=//p' $O/$n.out | head -1)
    mask=$(sed -n 's/^MASK texels=\([0-9a-f]*\) .*/\1/p' $O/$n.out | head -1)
    nl=$(grep -c "texels=$luma" $O/$n.log)
    nc=$(grep -c "texels=$chroma" $O/$n.log)
    ns=$(grep -cF "$shape" $O/$n.log)
    nm=$(grep -c "texels=$mask" $O/$n.log)
    ok=FAIL
    [[ $nl -gt 0 && $nc -gt 0 && $ns -gt 0 && $nm -gt 0 ]] && ok=PASS || fail=1
    echo "$c: luma $nl chroma $nc shape $ns handles; A8 mask $nm handles: $ok"
done
exit $fail

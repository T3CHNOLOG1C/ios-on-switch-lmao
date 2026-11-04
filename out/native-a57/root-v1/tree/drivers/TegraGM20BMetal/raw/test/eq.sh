#!/bin/zsh
# raw GM20B path (r8-rawca): host equivalence of the raw path with m2v + NVK, draw by draw (pack/eqcheck.py over nvdec logs).
# usage: eq.sh <build dir> <pack dir (pack.bin, layouts.txt, work/programs.bin)> <out dir> <label> <frames> [blur draws ubytes vbytes]
# environment passes through (FRAME_ALL_STATES, FRAME_RING, FRAME_DRAW_FORMS, FRAME_STATES ...)
B=$1; P=$2; O=$3; L=$4; shift 4
NVK=${NVK:-/Users/brunostjohn/Development/Projects/ioswitch/out/gpu/nvk/libvulkan_nouveau.r8.host.dylib}
QC=${0:A:h}/../../eq/qc/QuartzCore.framework/default.metallib   # the path suffix the facade keys QuartzCore by
C=/Users/brunostjohn/Development/Projects/ioswitch/out/native-a57/root-v1/tree/drivers/TegraGM20BMetal/precompile/corpus.log
mkdir -p $O/cache-$L
# the states the frame may use: the pack's (FRAME_KEYS=all: every corpus state, the ones m2v draws included)
if [[ "$FRAME_KEYS" != all ]]; then
  python3 -c "import json,sys; [print(r['key']) for r in json.load(open(sys.argv[1]))['pipelines']]" $P/pack.json > $O/$L.keys
  export FRAME_KEYS=$O/$L.keys
else
  unset FRAME_KEYS
fi
export RAWCAP_PROGRAMS=$P/work/programs.bin RAWCAP_LAYOUTS=$P/layouts.txt NVDEC_REGS=1 MESA_SHADER_CACHE_DISABLE=true NMB_ASYNC_PSO=0 NMB_ASYNC_PSO_API=0
RAWCAP_LOG=$O/$L.nvk.log FRAME_INPLACE=1 $B/nvkframe $NVK $O/cache-$L $QC $C "$@" > $O/$L.nvk.out 2> $O/$L.nvk.err || { echo "nvkframe failed"; tail -5 $O/$L.nvk.err; }
RAWCAP_LOG=$O/$L.raw.log $B/rawframe $P/pack.bin $C "$@" > $O/$L.raw.out 2> $O/$L.raw.err || { echo "rawframe failed"; tail -5 $O/$L.raw.err; }
RAWCAP_LOG=$O/$L.facade.log FRAME_FALLBACK=$NVK $B/facadeframe $P/pack.bin $O/cache-$L $QC $C "$@" > $O/$L.facade.out 2> $O/$L.facade.err || { echo "facadeframe failed"; tail -5 $O/$L.facade.err; }
for side in raw facade; do
  echo "== $L: nvk vs $side"
  python3 ${0:A:h}/../pack/eqcheck.py $O/$L.nvk.log $O/$L.$side.log --show 2 | tail -25
done

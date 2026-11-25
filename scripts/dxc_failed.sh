#!/bin/bash
# Compile every HLSL in analysis/shader_cache/failed on its own with DXC and record its first error.
# Output: analysis/shader_cache/dxc_errors.txt  (hash<TAB>stage<TAB>vs|ps<TAB>first error)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DXC="$ROOT/tools/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc.exe"
DIR="$ROOT/analysis/shader_cache/failed"
OUT="$ROOT/analysis/shader_cache/dxc_errors.txt"
LOGDIR="$ROOT/analysis/shader_cache/dxc_logs"
mkdir -p "$LOGDIR"
: > "$OUT"
one() {
  f="$1"; b=$(basename "$f" .hlsl); hash=${b%%_*}; stage=${b#*_}
  if grep -q '\[shader("pixel")\]' "$f"; then t=ps_6_0; k=ps; else t=vs_6_0; k=vs; fi
  if [ "$stage" = spirv ]; then extra="-spirv -fvk-use-dx-layout"; [ $k = vs ] && extra="$extra -fvk-invert-y"; else extra=""; fi
  "$DXC" -T $t -HV 2021 -all-resources-bound -Wno-ignored-attributes -E main $extra -Fo NUL "$f" > "$LOGDIR/$b.txt" 2>&1
  if [ "$stage" = dxil ] && grep -q "External function used in non-library profile" "$LOGDIR/$b.txt"; then "$DXC" -T lib_6_3 -HV 2021 -all-resources-bound -Wno-ignored-attributes -Fo NUL "$f" > "$LOGDIR/$b.txt" 2>&1; fi
  err=$(grep -m1 -E "error|fatal" "$LOGDIR/$b.txt" | sed -E 's/^[^:]*hlsl:[0-9]+:[0-9]+: //')
  printf "%s\t%s\t%s\t%s\n" "$hash" "$stage" "$k" "$err"
}
export -f one; export DXC LOGDIR
ls "$DIR"/*.hlsl | xargs -P 8 -I{} bash -c 'one "{}"' >> "$OUT"
echo "done: $(wc -l < "$OUT") files"

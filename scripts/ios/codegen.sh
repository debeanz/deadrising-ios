#!/bin/bash
# Generates everything that comes from YOUR game files, on a Mac (or Linux):
#   ppc/                          the recompiled game code (XenonRecomp, from default.xex)
#   analysis/shaders_2008/        the game's shaders in the container layout XenosRecomp reads
#   analysis/shader_cache/        the translated shaders (SPIR-V; MoltenVK runs them on iOS)
# Nothing written here may be committed or published (docs/ios.md).
#
# Usage: scripts/ios/codegen.sh /path/to/default.xex /path/to/ntvX/system/shader.arc
set -euo pipefail

XEX="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
ARC="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

step() { printf '\n==> %s\n' "$1"; }

# Host programs (this Mac), not the phone.
unset IPHONEOS_DEPLOYMENT_TARGET || true
HOST_FLAGS=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_SYSROOT=macosx)

# Upstream tools + the Dead Rising changes (patches/). Already applied: skip.
apply_patch() {
    local dir="$1" patch="$ROOT/patches/$2"
    if git -C "$dir" apply --check "$patch" 2>/dev/null; then
        git -C "$dir" apply --whitespace=nowarn "$patch"
        echo "applied $2"
    elif git -C "$dir" apply --check -R "$patch" 2>/dev/null; then
        echo "$2 already applied"
    else
        echo "error: $2 does not apply to $dir" >&2
        exit 1
    fi
}

step "Patching the tools"
apply_patch tools/XenonRecomp XenonRecomp-deadrising.patch
apply_patch tools/XenosRecomp XenosRecomp-deadrising.patch
apply_patch tools/plume plume-deadrising.patch

# DR_CODEGEN_PATCH_ONLY=1: ppc/ and analysis/ came from an earlier build (the workflow keeps them in the
# private data repository when the game files, tools, patches and config are the same).
if [ "${DR_CODEGEN_PATCH_ONLY:-0}" = 1 ]; then
    step "Done"
    echo "generated $(ls ppc/*.cpp | wc -l | tr -d ' ') files in ppc/ (reused from an earlier build)"
    test -s analysis/shader_cache/shader_cache.cpp || { echo "error: the reused code has no shader_cache.cpp"; exit 1; }
    exit 0
fi

step "XenonRecomp (game code)"
cmake -S tools/XenonRecomp -B build/host/XenonRecomp "${HOST_FLAGS[@]}"
cmake --build build/host/XenonRecomp --target XenonRecomp
mkdir -p private ppc
cp "$XEX" private/default.xex
XENONRECOMP="$(find build/host/XenonRecomp -type f -name XenonRecomp -perm -u+x | head -n 1)"
"$XENONRECOMP" config/DR.toml tools/XenonRecomp/XenonUtils/ppc_context.h
echo "generated $(ls ppc/*.cpp | wc -l | tr -d ' ') files in ppc/"

step "Shaders: unpack shader.arc"
rm -rf analysis/shader_arc analysis/shaders_2008 analysis/shader_cache
mkdir -p analysis/shader_arc analysis/shaders_2008 analysis/shader_cache
pwsh -NoProfile -File scripts/unarc.ps1 -Arc "$ARC" -OutDir analysis/shader_arc

step "Shaders: xfb2xenos"
mkdir -p build/host
clang++ -std=c++20 -O2 -isysroot "$(xcrun --sdk macosx --show-sdk-path 2>/dev/null || true)" \
    -o build/host/xfb2xenos xfb2xenos/main.cpp
build/host/xfb2xenos analysis/shaders_2008 analysis/shader_arc

step "Shaders: XenosRecomp (SPIR-V)"
cmake -S tools/XenosRecomp -B build/host/XenosRecomp "${HOST_FLAGS[@]}"
cmake --build build/host/XenosRecomp --target XenosRecomp
XENOSRECOMP="$(find build/host/XenosRecomp -type f -name XenosRecomp -perm -u+x | head -n 1)"
DXC_LIB="$ROOT/tools/XenosRecomp/thirdparty/dxc-bin/lib/$(uname -m | sed 's/x86_64/x64/')"
echo "dxcompiler: $(ls -l "$DXC_LIB"/libdxcompiler.dylib 2>&1)"
otool -L "$XENOSRECOMP" | grep -i dxc || true
# Full output in analysis/shader_cache/xenosrecomp.log (it names the game's shaders: private log
# only); here the exit status, the counts and the last lines that are not per-shader.
set +e
DYLD_LIBRARY_PATH="$DXC_LIB" LD_LIBRARY_PATH="$DXC_LIB" "$XENOSRECOMP" analysis/shaders_2008 \
    analysis/shader_cache/shader_cache.cpp tools/XenosRecomp/XenosRecomp/shader_common.h \
    > analysis/shader_cache/xenosrecomp.log 2>&1
status=$?
set -e
echo "XenosRecomp exit status $status: $(grep -c Translated analysis/shader_cache/xenosrecomp.log || true) translated, $(grep -c FAILED analysis/shader_cache/xenosrecomp.log || true) failed"
grep -v -E "Translated|FAILED" analysis/shader_cache/xenosrecomp.log | tail -25 || true
if [ "$status" -ne 0 ] && ! grep -q -E "Translated|FAILED|Recompiling" analysis/shader_cache/xenosrecomp.log; then
    # It died before naming any shader: these lines are safe for the public log ("diag:").
    {
        file "$XENOSRECOMP" "$DXC_LIB/libdxcompiler.dylib"
        "$XENOSRECOMP" > /dev/null 2>&1; echo "XenosRecomp without arguments: exit status $?"
        echo "its output:"; head -30 analysis/shader_cache/xenosrecomp.log
        echo "under lldb:"
        lldb --batch -o "settings set target.env-vars DYLD_LIBRARY_PATH=$DXC_LIB" -o run -o "bt 40" -o quit -- \
            "$XENOSRECOMP" analysis/shaders_2008 "$ROOT/build/host/lldb_cache.cpp" tools/XenosRecomp/XenosRecomp/shader_common.h 2>&1 |
            grep -E "frame #|stop reason|error|exited" | head -60
    } 2>&1 | sed 's/^/diag: /'
fi
if [ "$status" -ne 0 ]; then
    # The crash handler's stack (function names of the tools only) and how far it got: safe for the
    # public log ("diag:").
    {
        echo "progress lines: $(grep -c Recompiling analysis/shader_cache/xenosrecomp.log || true)"
        sed -n '/^XenosRecomp: signal/,$p' analysis/shader_cache/xenosrecomp.log | grep -v -E "Translated|FAILED|Recompiling" | head -60
    } 2>&1 | sed 's/^/diag: /'
fi
test -s analysis/shader_cache/shader_cache.cpp || { echo "error: XenosRecomp wrote no shader_cache.cpp"; exit 1; }

step "Done"
echo "ppc/ and analysis/ are ready; configure the runtime with CMAKE_SYSTEM_NAME=iOS (docs/ios.md)."

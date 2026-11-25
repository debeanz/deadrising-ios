#!/bin/sh
# Configures FFmpeg with nothing but the XMA decoders (libavcodec + libavutil), as the runtime's
# XMA audio (runtime/src/apu/xma.cpp) needs. Used by scripts/build_ffmpeg_win.bat and the iOS
# workflow.
#   configure.sh <ffmpeg source dir> <build dir> [extra configure options...]
set -e
src="$1"; build="$2"; shift 2
# FFmpeg refuses out-of-tree builds from a path with spaces; on Windows use the short (8.3) path.
if command -v cygpath >/dev/null 2>&1; then
    src="$(cygpath -u "$(cygpath -d "$src")")"
    mkdir -p "$build"; build="$(cygpath -u "$(cygpath -d "$build")")"
fi
mkdir -p "$build"
cd "$build"
"$src/configure" \
    --disable-everything --disable-programs --disable-doc --disable-avdevice --disable-avformat \
    --disable-swscale --disable-swresample --disable-avfilter --disable-postproc --disable-network \
    --disable-asm --disable-debug --disable-autodetect --disable-iconv \
    --enable-decoder=xma1 --enable-decoder=xma2 --enable-static --disable-shared "$@" > configure.out 2>&1 ||
    { tail -30 configure.out; tail -40 ffbuild/config.log; exit 1; }
grep -E "^(Enabled decoders|License)" -A1 configure.out

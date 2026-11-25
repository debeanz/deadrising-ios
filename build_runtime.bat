@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set PATH=C:\Program Files\LLVM\bin;C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
cd /d "F:\downloads\Dead Rising\_recomp\runtime"
rem FFmpeg (XMA audio) from scripts\build_ffmpeg_win.bat, when built.
set FFMPEG=
if exist "..\deps\ffmpeg-win\lib\avcodec.lib" set FFMPEG="-DDR_FFMPEG_DIR=F:/downloads/Dead Rising/_recomp/deps/ffmpeg-win"
rem Configure every time so the generated ppc/*.cpp list stays current.
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl %FFMPEG% > "F:\downloads\Dead Rising\_recomp\build_runtime.log" 2>&1 || exit /b 1
cmake --build build -- -k 0 >> "F:\downloads\Dead Rising\_recomp\build_runtime.log" 2>&1
echo build exit %ERRORLEVEL% >> "F:\downloads\Dead Rising\_recomp\build_runtime.log"

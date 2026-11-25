@echo off
rem Builds FFmpeg's XMA decoders (static libavcodec + libavutil, nothing else) for the Windows
rem runtime into deps\ffmpeg-win; build_runtime.bat then passes DR_FFMPEG_DIR. Log: deps\ffmpeg-win.log
setlocal
set "ROOT=%~dp0.."
set "DEPS=%ROOT%\deps"
set "LOG=%DEPS%\ffmpeg-win.log"
if not exist "%DEPS%" mkdir "%DEPS%"
if not exist "%DEPS%\ffmpeg\configure" (
    git clone --depth 1 --branch n7.1 https://github.com/FFmpeg/FFmpeg.git "%DEPS%\ffmpeg" > "%LOG%" 2>&1 || exit /b 1
)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
"C:\Program Files\Git\bin\bash.exe" "%~dp0ffmpeg\configure.sh" "%DEPS%\ffmpeg" "%DEPS%\ffmpeg-build-win" --toolchain=msvc --target-os=win64 --arch=x86_64 > "%LOG%" 2>&1 || exit /b 1
cmake -S "%~dp0ffmpeg" -B "%DEPS%\ffmpeg-build-win\cmake" -G Ninja -DCMAKE_C_COMPILER=cl -DCMAKE_BUILD_TYPE=Release ^
    -DFFMPEG_SRC="%DEPS%\ffmpeg" -DFFMPEG_CFG="%DEPS%\ffmpeg-build-win" -DCMAKE_INSTALL_PREFIX="%DEPS%\ffmpeg-win" >> "%LOG%" 2>&1 || exit /b 1
cmake --build "%DEPS%\ffmpeg-build-win\cmake" >> "%LOG%" 2>&1 || exit /b 1
cmake --install "%DEPS%\ffmpeg-build-win\cmake" >> "%LOG%" 2>&1 || exit /b 1
echo ffmpeg ok >> "%LOG%"

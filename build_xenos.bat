@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set PATH=C:\Program Files\LLVM\bin;C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
cd /d "F:\downloads\Dead Rising\_recomp\tools\XenosRecomp"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl > "F:\downloads\Dead Rising\_recomp\build_xenos.log" 2>&1 || exit /b 1
cmake --build build --target XenosRecomp >> "F:\downloads\Dead Rising\_recomp\build_xenos.log" 2>&1
echo build exit %ERRORLEVEL% >> "F:\downloads\Dead Rising\_recomp\build_xenos.log"

@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "F:\downloads\Dead Rising\_recomp\xfb2xenos"
"C:\Program Files\LLVM\bin\clang-cl.exe" /std:c++20 /O2 /EHsc -D_CRT_SECURE_NO_WARNINGS main.cpp /Fe:xfb2xenos.exe

@echo off
rem Regenerates the translated shaders the runtime links (docs/shaders.md):
rem   analysis\shaders_2008\*.xenos + shader_keys.cpp + listing.txt   (xfb2xenos)
rem   analysis\shader_cache\shader_cache.cpp + failed\*                (XenosRecomp)
rem Needs analysis\shader_arc (scripts\unarc.ps1 on ntvX\system\shader.arc) and both tools built
rem (xfb2xenos\build.bat, build_xenos.bat). Rebuild the runtime afterwards.
setlocal
set ROOT=%~dp0..
cd /d "%ROOT%"
if not exist analysis\shaders_2008 mkdir analysis\shaders_2008
del /q analysis\shaders_2008\*.xenos 2>nul
xfb2xenos\xfb2xenos.exe analysis\shaders_2008 analysis\shader_arc || exit /b 1
if not exist analysis\shader_cache mkdir analysis\shader_cache
del /q analysis\shader_cache\failed\*.hlsl analysis\shader_cache\failed\*.xenos 2>nul
tools\XenosRecomp\build\XenosRecomp\XenosRecomp.exe analysis\shaders_2008 analysis\shader_cache\shader_cache.cpp tools\XenosRecomp\XenosRecomp\shader_common.h > analysis\shader_cache\stdout.txt 2> analysis\shader_cache\stderr.txt
findstr /c:"Translated" analysis\shader_cache\stdout.txt

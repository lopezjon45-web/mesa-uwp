@echo off
rem Usage: build.cmd C:\path\to\mesa\include   (from a Visual Studio x64 developer prompt)
rem Builds glprobe.exe in the current directory.
if "%~1"=="" (
   echo usage: build.cmd MESA_INCLUDE_DIR
   exit /b 1
)
set SRC=%~dp0
cl /nologo /O2 /EHsc /std:c++17 /W3 /I"%~1" "%SRC%glprobe.cpp" "%SRC%scenes_alu.cpp" "%SRC%scenes_tex.cpp" "%SRC%scenes_raster.cpp" "%SRC%scenes_draw.cpp" "%SRC%platform_wgl.cpp" /Fe:glprobe.exe user32.lib gdi32.lib opengl32.lib

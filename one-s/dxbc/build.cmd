@echo off
rem Builds dxbccheck.exe in the current directory (from a Visual Studio x64 developer prompt).
set SRC=%~dp0
cl /nologo /O2 /EHsc /std:c++17 /W3 "%SRC%dxbccheck.cpp" /Fe:dxbccheck.exe

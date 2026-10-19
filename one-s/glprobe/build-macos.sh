#!/bin/sh
# Local lever only: macOS has no WGL, so this builds glprobe against a CGL offscreen
# GL 4.1 core context. Usage: ./build-macos.sh /path/to/mesa/include
set -e
cd "$(dirname "$0")"
MESA_INCLUDE=${1:?usage: build-macos.sh MESA_INCLUDE_DIR}
clang++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -DGL_SILENCE_DEPRECATION \
   -I"$MESA_INCLUDE" glprobe.cpp scenes_alu.cpp scenes_tex.cpp scenes_raster.cpp scenes_draw.cpp \
   platform_cgl.cpp -framework OpenGL -o glprobe

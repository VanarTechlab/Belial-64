#!/bin/bash
# Crea la carpeta build-n64 (CMake + Ninja). Ejecutar una vez, despues de compilar_sdl.sh.
set -e
N64="$(cd "$(dirname "$0")" && pwd)"
cd "$N64/.."
cmake -S . -B build-n64 -G Ninja -DCMAKE_TOOLCHAIN_FILE="$N64/n64.toolchain.cmake" -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DSDL_INCLUDE_DIR="$N64/sdl/include" -DSDL_LIBRARY="$N64/libsdl_n64.a" "$@"

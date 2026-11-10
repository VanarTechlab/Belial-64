#!/bin/bash
# Compila la capa SDL 1.2 de la N64 (sdl_n64.c) y crea n64/libsdl_n64.a
set -e
N64_INST="${N64_INST:-/opt/libdragon}"
cd "$(dirname "$0")"
"$N64_INST/bin/mips64-elf-gcc" -march=vr4300 -mtune=vr4300 -mabi=o64 -O2 -g -falign-functions=32 \
  -ffunction-sections -fdata-sections -DN64 -DLIBDRAGON_PREVIEW=2 -include ktls.h \
  -I"$N64_INST/mips64-elf/include/newlib_overrides" -I"$N64_INST/mips64-elf/include" -Isdl/include \
  -Wall -Werror -Wno-error=unused-function -c sdl_n64.c -o sdl_n64.o
"$N64_INST/bin/mips64-elf-ar" rcs libsdl_n64.a sdl_n64.o
echo "libsdl_n64.a OK"

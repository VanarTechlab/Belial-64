#!/bin/bash
# Compila la capa SDL y el motor, y crea n64/salida/belial64.z64
set -e
N64_INST="${N64_INST:-/opt/libdragon}"; export N64_INST
N64="$(cd "$(dirname "$0")" && pwd)"
B="$N64_INST/bin"
"$N64/compilar_sdl.sh"
cd "$N64/../build-n64"
rm -f devilutionx   # ninja no ve el cambio de libsdl_n64.a
ninja
OUT="$N64/salida"; mkdir -p "$OUT"
cp devilutionx "$OUT/belial64.elf"
"$B/n64sym" --all "$OUT/belial64.elf" "$OUT/belial64.elf.sym"
cp "$OUT/belial64.elf" "$OUT/belial64.elf.stripped"
"$B/mips64-elf-strip" -s "$OUT/belial64.elf.stripped"
"$B/n64elfcompress" -o "$OUT/" -c 1 "$OUT/belial64.elf.stripped"
"$B/n64tool" --toc --title "BELIAL 64" --output "$OUT/belial64.z64" --align 256 "$OUT/belial64.elf.stripped" "$OUT/belial64.elf.sym"
md5sum "$OUT/belial64.z64"
# Traduccion al espanol (va en devilutionx/assets/es.gmo de la SD)
python3 "$N64/traduccion/msgfmt.py" "$N64/traduccion/es_n64.po" "$OUT/es.gmo"

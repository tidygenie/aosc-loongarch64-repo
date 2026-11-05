#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$HERE/libvkgl.so}"
OBJ="${OUT%.so}-linux-dmabuf.o"
SPV="${SPIRV_CROSS_DIR:-$HERE/spirv-cross}"

if [[ -f "$SPV/build/libspirv-cross-core.a" ]]; then
  SPV_INCLUDE="$SPV"
  SPV_LIB="$SPV/build"
else
  SPV_INCLUDE="$SPV/include"
  SPV_LIB="$SPV/lib"
fi

# Compile the generated protocol as C. If g++ handles the .c file directly,
# the unreferenced const interface gets internal linkage and disappears.
gcc -std=c11 -O2 -fPIC -c "$HERE/linux-dmabuf-protocol.c" -o "$OBJ" \
  $(pkg-config --cflags wayland-client)

g++ -std=c++17 -O2 -fPIC -shared -o "$OUT" "$HERE/vkgl.cpp" "$OBJ" \
  -I"$SPV_INCLUDE" \
  $(pkg-config --cflags --libs wayland-client) \
  -lEGL -lGLESv2 -lgbm \
  "$SPV_LIB/libspirv-cross-glsl.a" \
  "$SPV_LIB/libspirv-cross-util.a" \
  "$SPV_LIB/libspirv-cross-core.a" \
  -lpthread

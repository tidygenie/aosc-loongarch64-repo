#!/usr/bin/env bash
set -euo pipefail
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT=${1:-/tmp/lg110-draw-count-build}
VKGL_TESTS=$(CDPATH= cd -- "$HERE/../../tests/vkgl" && pwd)
mkdir -p "$OUT"
cc -std=c11 -O2 -g -fPIC -shared -Wall -Wextra -Wpedantic -Werror \
  $(pkg-config --cflags libdrm) -I/usr/src/loonggpu-1.0.2/include \
  -o "$OUT/lg110-draw-count-mutation.so" \
  "$HERE/lg110-draw-count-mutation.c" -ldl -lpthread
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -I"$VKGL_TESTS" \
  -o "$OUT/vktest-draw-count-readback" \
  "$HERE/vktest-draw-count-readback.c" "$VKGL_TESTS/xdg-shell-protocol.c" \
  $(pkg-config --cflags --libs wayland-client) -lvulkan
mkdir -p "$OUT/first-vertex"
glslangValidator -V "$HERE/grad4.vert" -o "$OUT/first-vertex/grad.vert.spv"
cp "$HERE/vktest-gradient-sample-readback.frag.spv" "$OUT/first-vertex/"
echo "Built under $OUT; no GPU test was executed. Run from $HERE for shaders."
echo "For firstVertex experiments, run from $OUT/first-vertex instead."

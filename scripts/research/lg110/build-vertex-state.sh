#!/usr/bin/env bash
# Build the guarded tracer and constant-pixel vertex-state differential only.
set -euo pipefail
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT=${1:-/tmp/lg110-vertex-state-build}
VKGL_TESTS=$(CDPATH= cd -- "$HERE/../../tests/vkgl" && pwd)
mkdir -p "$OUT"
cc -std=c11 -O2 -g -fPIC -shared -Wall -Wextra -Wpedantic -Werror \
  $(pkg-config --cflags libdrm) -I/usr/src/loonggpu-1.0.2/include \
  -o "$OUT/lg110-draw-count-mutation.so" \
  "$HERE/lg110-draw-count-mutation.c" -ldl -lpthread
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -I"$VKGL_TESTS" \
  -o "$OUT/vktest-vertex-state" "$HERE/vktest-vertex-state.c" \
  "$VKGL_TESTS/xdg-shell-protocol.c" \
  $(pkg-config --cflags --libs wayland-client) -lvulkan
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -I"$VKGL_TESTS" \
  -o "$OUT/vktest-vertex-pair" "$HERE/vktest-vertex-pair.c" \
  "$VKGL_TESTS/xdg-shell-protocol.c" \
  $(pkg-config --cflags --libs wayland-client) -lvulkan
mkdir -p "$OUT/run"
glslangValidator -V "$VKGL_TESTS/vktest-vertex-bindings.vert" -o "$OUT/run/grad.vert.spv"
glslangValidator -V "$HERE/vktest-attribute-color.frag" -o "$OUT/run/vktest-attribute-color.frag.spv"
cp "$HERE/vktest-gradient-sample-readback.frag.spv" "$OUT/run/"
echo "Built under $OUT; no GPU test was executed. Run from $OUT/run."

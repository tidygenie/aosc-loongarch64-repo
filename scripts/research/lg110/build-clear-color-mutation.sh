#!/usr/bin/env bash
set -euo pipefail
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT=${1:-/tmp/lg110-clear-color-build}
VKGL_DEPS=/home/houge/infra/vkgl
mkdir -p "$OUT"
cc -std=c11 -O2 -g -fPIC -shared -Wall -Wextra -Wpedantic -Werror \
  $(pkg-config --cflags libdrm) \
  -I/usr/src/loonggpu-1.0.2/include \
  -o "$OUT/lg110-clear-color-mutation.so" \
  "$HERE/lg110-clear-color-mutation.c" -ldl -lpthread
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -I"$VKGL_DEPS" \
  -o "$OUT/vktest-clear-color-readback" \
  "$HERE/vktest-clear-color-readback.c" "$VKGL_DEPS/xdg-shell-protocol.c" \
  $(pkg-config --cflags --libs wayland-client) -lvulkan
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
  -o "$OUT/vktest-offscreen-clear-alpha" \
  "$HERE/vktest-offscreen-clear-alpha.c" -lvulkan
echo "Built under $OUT; no GPU test was executed."

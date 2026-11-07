#!/bin/sh
# Build all strict 0.1.30 tests; this script never executes a GPU workload.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:-/tmp/vkgl-0.1.30-tests}
sh "$here/build-0.1.29-tests.sh" "$out"
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -I"$here" \
  -o "$out/vktest-vertex-bindings" "$here/vktest-vertex-bindings.c" \
  "$here/xdg-shell-protocol.c" $(pkg-config --cflags --libs wayland-client) -lvulkan
mkdir -p "$out/bindings"
glslangValidator -V "$here/vktest-vertex-bindings.vert" -o "$out/bindings/grad.vert.spv"
cp "$here/../../research/lg110/vktest-gradient-sample-readback.frag.spv" "$out/bindings/"
printf 'Built 0.1.30 vertex-binding tests under %s; run from %s/bindings.\n' "$out" "$out"

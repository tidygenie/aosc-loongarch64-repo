#!/bin/sh
# Build all strict 0.1.27 tests; this script never executes a GPU workload.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:-/tmp/vkgl-0.1.27-tests}
sh "$here/build-0.1.26-tests.sh" "$out"
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -I"$here" \
  -o "$out/vktest-draw-instance" "$here/vktest-draw-instance.c" \
  "$here/xdg-shell-protocol.c" $(pkg-config --cflags --libs wayland-client) -lvulkan
mkdir -p "$out/instance"
glslangValidator -V "$here/vktest-instance.vert" -o "$out/instance/grad.vert.spv"
cp "$here/../../research/lg110/vktest-gradient-sample-readback.frag.spv" "$out/instance/"
printf 'Built 0.1.27 tests under %s; run instance matrix from %s/instance.\n' "$out" "$out"

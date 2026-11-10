#!/bin/sh
# Build the three-frame uniform-array regression; run no GPU test.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:-/tmp/lg110-uniform-array}
mkdir -p "$out/shaders"
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -I"$here/../../tests/vkgl" \
  -o "$out/vktest-uniform-array" "$here/vktest-uniform-array.c" \
  "$here/../../tests/vkgl/xdg-shell-protocol.c" \
  $(pkg-config --cflags --libs wayland-client) -lvulkan
glslangValidator -V "$here/grad.vert" -o "$out/shaders/grad.vert.spv"
glslangValidator -V "$here/uniform-array.frag" -o "$out/shaders/uniform-array.frag.spv"
glslangValidator -V "$here/uniform-tail.frag" -o "$out/shaders/uniform-tail.frag.spv"
printf 'Built uniform-array regression under %s; no GPU test was executed.\n' "$out"

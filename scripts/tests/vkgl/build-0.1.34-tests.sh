#!/bin/sh
# Build strict scissor tests; this script never executes a GPU workload.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:-/tmp/vkgl-0.1.34-tests}
sh "$here/build-0.1.33-tests.sh" "$out"
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -I"$here" \
  -o "$out/vktest-scissor" "$here/vktest-scissor.c" \
  "$here/xdg-shell-protocol.c" $(pkg-config --cflags --libs wayland-client) -lvulkan
mkdir -p "$out/scissor"
glslangValidator -V "$here/../../research/lg110/grad.vert" -o "$out/scissor/grad.vert.spv"
glslangValidator -V "$here/vktest-scissor.frag" -o "$out/scissor/scissor.frag.spv"
printf 'Built scissor tests under %s; run from %s/scissor.\n' "$out" "$out"

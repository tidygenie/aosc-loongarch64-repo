#!/bin/sh
# Build all strict 0.1.29 tests; this script never executes a GPU workload.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:-/tmp/vkgl-0.1.29-tests}
sh "$here/build-0.1.28-tests.sh" "$out"
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -I"$here" \
  -o "$out/vktest-index-offset" "$here/vktest-index-offset.c" \
  "$here/xdg-shell-protocol.c" $(pkg-config --cflags --libs wayland-client) -lvulkan
mkdir -p "$out/offset"
glslangValidator -V "$here/vktest-index-offset.vert" -o "$out/offset/grad.vert.spv"
cp "$here/../../research/lg110/vktest-gradient-sample-readback.frag.spv" "$out/offset/"
printf 'Built 0.1.29 signed-offset tests under %s; run from %s/offset.\n' "$out" "$out"

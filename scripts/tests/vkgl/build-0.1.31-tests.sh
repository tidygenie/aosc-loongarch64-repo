#!/bin/sh
# Build all strict 0.1.31 tests; this script never executes a GPU workload.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:-/tmp/vkgl-0.1.31-tests}
sh "$here/build-0.1.30-tests.sh" "$out"
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -I"$here" \
  -o "$out/vktest-instance-rate" "$here/vktest-instance-rate.c" \
  "$here/xdg-shell-protocol.c" $(pkg-config --cflags --libs wayland-client) -lvulkan
mkdir -p "$out/instance-rate"
glslangValidator -V "$here/vktest-instance-rate.vert" -o "$out/instance-rate/instance-rate.vert.spv"
glslangValidator -V "$here/vktest-instance-rate.frag" -o "$out/instance-rate/instance-rate.frag.spv"
printf 'Built 0.1.31 instance-rate tests under %s; run from %s/instance-rate.\n' "$out" "$out"

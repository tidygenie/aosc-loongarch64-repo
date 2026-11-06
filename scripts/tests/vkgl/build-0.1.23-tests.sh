#!/bin/sh
# Build 0.1.23 sampler tests and earlier tests; do not execute GPU programs.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
research=$(CDPATH= cd -- "$here/../../research/lg110" && pwd)
out=${1:-/tmp/vkgl-0.1.23-tests}
mkdir -p "$out"
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
  $(pkg-config --cflags vulkan) "$here/vktest-sampler-create.c" \
  -o "$out/vktest-sampler-create" $(pkg-config --libs vulkan)
glslangValidator -V -S frag \
  -o "$out/vktest-sampler-offset.frag.spv" \
  "$research/vktest-sampler-offset.frag"
cc -std=gnu11 -O2 -Wall -Wextra -Werror \
  -I"$research" -I"$here" \
  "$research/vktest-gradient-sample-readback.c" "$here/xdg-shell-protocol.c" \
  -o "$out/vktest-gradient-sample-readback" \
  $(pkg-config --cflags --libs wayland-client) -lvulkan
sh "$here/build-0.1.22-tests.sh" "$out"
printf 'Built 0.1.23 sampler tests under %s; no GPU test was executed.\n' "$out"

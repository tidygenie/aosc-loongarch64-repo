#!/bin/sh
# Build 0.1.20 correctness tests only; never initializes a GPU.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:-/tmp/vkgl-0.1.20-tests}
mkdir -p "$out"
strict='-Wall -Wextra -Wpedantic -Werror'

c++ -std=c++17 -O2 $strict -x c++ \
  "$here/vktest-submit-fence.c" -o "$out/vktest-submit-fence"
cc -std=c11 -O2 $strict -I"$here" \
  "$here/vktest-submit-fence-gpu.c" "$here/xdg-shell-protocol.c" \
  -o "$out/vktest-submit-fence-gpu" \
  $(pkg-config --cflags --libs wayland-client) -lvulkan
cc -std=c11 -O2 $strict \
  $(pkg-config --cflags vulkan) "$here/vktest-tight-upload.c" \
  -o "$out/vktest-tight-upload" $(pkg-config --libs vulkan)
printf 'Built tests under %s; no GPU test was executed.\n' "$out"

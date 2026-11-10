#!/bin/sh
# Build strict deferred UpdateBuffer tests; never executes a GPU workload.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:-/tmp/vkgl-0.1.35-tests}
sh "$here/build-0.1.34-tests.sh" "$out"
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
  -o "$out/vktest-update-buffer" "$here/vktest-update-buffer.c" -lvulkan
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -I"$here" \
  -o "$out/vktest-update-indirect" "$here/vktest-update-indirect.c" \
  "$here/xdg-shell-protocol.c" $(pkg-config --cflags --libs wayland-client) -lvulkan
mkdir -p "$out/update-indirect"
cp "$out/scissor/grad.vert.spv" "$out/scissor/scissor.frag.spv" "$out/update-indirect/"
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -I"$here" \
  -o "$out/vktest-update-vertex" "$here/vktest-update-vertex.c" \
  "$here/xdg-shell-protocol.c" $(pkg-config --cflags --libs wayland-client) -lvulkan
mkdir -p "$out/update-vertex"
cp "$out/bindings/grad.vert.spv" "$out/bindings/vktest-gradient-sample-readback.frag.spv" "$out/update-vertex/"
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -I"$here" \
  -o "$out/vktest-update-uniform" "$here/vktest-update-uniform.c" \
  "$here/xdg-shell-protocol.c" $(pkg-config --cflags --libs wayland-client) -lvulkan
mkdir -p "$out/update-uniform"
cp "$out/scissor/grad.vert.spv" "$out/update-uniform/"
glslangValidator -V "$here/update-uniform.frag" -o "$out/update-uniform/update-uniform.frag.spv"
printf 'Built UpdateBuffer tests under %s; no GPU test was executed.\n' "$out"

#!/bin/sh
# Build the passive full-IB tracer and two-color workload; run no GPU test.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:-/tmp/lg110-uniform-stream}
mkdir -p "$out/shaders"
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -I"$here/../../tests/vkgl" \
  -o "$out/vktest-uniform-stream" "$here/vktest-uniform-stream.c" \
  "$here/../../tests/vkgl/xdg-shell-protocol.c" \
  $(pkg-config --cflags --libs wayland-client) -lvulkan
cc -std=c11 -O2 -g -fPIC -shared -Wall -Wextra -Wpedantic \
  $(pkg-config --cflags libdrm) -I/usr/src/loonggpu-1.0.2/include \
  -o "$out/lg110-iotrace.so" "$here/lg110-iotrace-fullline.c" -ldl -lpthread
cc -std=c11 -O2 -g -fPIC -shared -Wall -Wextra -Wpedantic \
  $(pkg-config --cflags libdrm) -I/usr/src/loonggpu-1.0.2/include \
  -o "$out/lg110-iotrace-bohash.so" "$here/lg110-iotrace-bohash.c" -ldl -lpthread
cc -std=c11 -O2 -g -fPIC -shared -Wall -Wextra -Wpedantic \
  $(pkg-config --cflags libdrm) -I/usr/src/loonggpu-1.0.2/include \
  -o "$out/lg110-uniform-mutation.so" "$here/lg110-uniform-mutation.c" -ldl -lpthread
cc -std=c11 -O2 -g -fPIC -shared -Wall -Wextra -Wpedantic \
  $(pkg-config --cflags libdrm) -I/usr/src/loonggpu-1.0.2/include \
  -o "$out/lg110-uniform-pointer-mutation.so" \
  "$here/lg110-uniform-pointer-mutation.c" -ldl -lpthread
glslangValidator -V "$here/grad.vert" -o "$out/shaders/grad.vert.spv"
glslangValidator -V "$here/../../tests/vkgl/update-uniform.frag" \
  -o "$out/shaders/update-uniform.frag.spv"
printf 'Built uniform-stream tools under %s; no GPU test was executed.\n' "$out"

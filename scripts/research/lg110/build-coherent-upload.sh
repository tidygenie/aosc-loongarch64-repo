#!/bin/sh
# Build controlled coherent-upload test; shaders are reused from the public suite.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tests=$(CDPATH= cd -- "$here/../../tests/vkgl" && pwd)
out=${1:-/tmp/lg110-coherent-upload}
mkdir -p "$out"
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -I"$tests" \
  -o "$out/vktest-coherent-upload" "$here/vktest-coherent-upload.c" \
  "$tests/xdg-shell-protocol.c" $(pkg-config --cflags --libs wayland-client) -lvulkan
printf 'Built %s; no GPU test was executed. Run from %s for shaders.\n' "$out" "$here"

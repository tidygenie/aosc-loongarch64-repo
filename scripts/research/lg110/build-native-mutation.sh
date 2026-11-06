#!/bin/sh
# Compile only. Never executes the test or loads the interposer.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:-/tmp/lg110-native-mutation-public}
tests="$here/../../tests/vkgl"
loonggpu_include=${LOONGGPU_INCLUDE:-/usr/src/loonggpu-1.0.2/include}
mkdir -p "$out"

${CC:-cc} -std=c11 -O2 -g -fPIC -shared \
  -Wall -Wextra -Wpedantic -Werror \
  $(pkg-config --cflags libdrm) \
  -I"$loonggpu_include" \
  -o "$out/lg110-native-mutation.so" \
  "$here/lg110-native-mutation.c" -ldl -lpthread \
  2>"$out/interposer-build.log"

${CC:-cc} -std=c11 -O2 \
  -Wall -Wextra -Wpedantic -Werror -I"$tests" \
  -o "$out/vktest-clear-readback" \
  "$here/vktest-clear-readback.c" "$tests/xdg-shell-protocol.c" \
  $(pkg-config --cflags --libs wayland-client) -lvulkan \
  2>"$out/test-build.log"

printf 'Built under %s; no GPU test was executed.\n' "$out"

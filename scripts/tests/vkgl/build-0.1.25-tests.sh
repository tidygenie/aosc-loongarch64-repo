#!/bin/sh
# Build 0.1.25 dispatchable lifetime test and earlier tests; no GPU execution.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:-/tmp/vkgl-0.1.25-tests}
mkdir -p "$out"
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
  $(pkg-config --cflags vulkan) "$here/vktest-dispatch-lifetime.c" \
  -o "$out/vktest-dispatch-lifetime" $(pkg-config --libs vulkan)
sh "$here/build-0.1.23-tests.sh" "$out"
printf 'Built 0.1.25 dispatchable lifetime test under %s; no GPU test was executed.\n' "$out"

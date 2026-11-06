#!/bin/sh
# Build 0.1.22 format tests plus existing upload/fence tests; no GPU is run.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:-/tmp/vkgl-0.1.22-tests}
mkdir -p "$out"
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
  $(pkg-config --cflags vulkan) "$here/vktest-format-properties.c" \
  -o "$out/vktest-format-properties" $(pkg-config --libs vulkan)
sh "$here/build-0.1.20-tests.sh" "$out"
printf 'Built 0.1.22 format test under %s; no GPU test was executed.\n' "$out"

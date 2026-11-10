#!/bin/sh
# Build strict deferred UpdateBuffer tests; never executes a GPU workload.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:-/tmp/vkgl-0.1.35-tests}
sh "$here/build-0.1.34-tests.sh" "$out"
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
  -o "$out/vktest-update-buffer" "$here/vktest-update-buffer.c" -lvulkan
printf 'Built UpdateBuffer tests under %s; no GPU test was executed.\n' "$out"

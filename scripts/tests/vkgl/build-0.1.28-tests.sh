#!/bin/sh
# Build all strict 0.1.28 tests; this script never executes a GPU workload.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:-/tmp/vkgl-0.1.28-tests}
sh "$here/build-0.1.27-tests.sh" "$out"
printf 'Built 0.1.28 UINT16/UINT32 tests under %s; run from %s/instance.\n' "$out" "$out"

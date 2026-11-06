#!/bin/sh
# Build all strict 0.1.26 tests; this script never executes a GPU workload.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:-/tmp/vkgl-0.1.26-tests}
sh "$here/build-0.1.25-tests.sh" "$out"
sh "$here/../../research/lg110/build-coherent-upload.sh" "$out"
printf 'Built 0.1.26 tests under %s; run coherent benchmark from the lg110 research directory.\n' "$out"

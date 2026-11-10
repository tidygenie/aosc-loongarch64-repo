#!/bin/sh
# Build strict primitive-restart tests; this script never executes a GPU workload.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:-/tmp/vkgl-0.1.33-tests}
sh "$here/build-0.1.31-tests.sh" "$out"
mkdir -p "$out/restart"
glslangValidator -V "$here/vktest-primitive-restart.vert" -o "$out/restart/grad.vert.spv"
cp "$here/../../research/lg110/vktest-gradient-sample-readback.frag.spv" "$out/restart/vktest-gradient-sample-readback.frag.spv"
printf 'Built primitive-restart tests under %s; run from %s/restart.\n' "$out" "$out"

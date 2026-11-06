#!/usr/bin/env bash
# Render smoke plus honest validation counts; no FPS/performance assertion.
set -euo pipefail
icd=${1:?usage: run-vkgl-smoke.sh ICD_JSON LABEL [OUTPUT_DIR]}
label=${2:?label required}
out=${3:-/tmp/vkgl-smoke}
mkdir -p "$out"
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/1000}
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0}
export VK_ICD_FILENAMES=$icd VK_DRIVER_FILES=$icd
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
unset LD_PRELOAD LG110_MUTATE_DRAW_FIELD LG110_TRACE_MAX LG110_TRACE_IB_DWORDS VKGL_DEBUG
for path in dmabuf shm; do
  log=$out/smoke-$label-$path.log
  : >"$log"
  if [[ $path == shm ]]; then export VKGL_NO_DMABUF=1; else unset VKGL_NO_DMABUF; fi
  {
    echo "== $path $label"
    echo '== vulkaninfo without validation (Vulkan 1.0 ICD)'
    timeout 30 env -u VK_INSTANCE_LAYERS vulkaninfo --summary
    echo '== vkcube 60 frames with validation'
    timeout 45 vkcube --wsi wayland --c 60 --width 640 --height 480 --gpu_number 0
    echo '== vkmark five FIFO scenes with validation'
    timeout 90 vkmark --winsys wayland --size 640x480 -p fifo \
      -b vertex:duration=2 -b texture:duration=2 -b shading:duration=2 \
      -b effect2d:duration=2 -b desktop:duration=2
  } >"$log" 2>&1
  grep -q 'LoongGPU LG110 (vkgl GLES2)' "$log"
  if grep -Eqi 'llvmpipe|swrast|segmentation fault' "$log"; then
    echo "FAIL $label $path: software renderer or crash"; exit 1
  fi
  validation_count=$(grep -c 'Validation Error:' "$log" || true)
  echo "RENDER_PASS $label $path validation_errors=$validation_count"
  if [[ ${REQUIRE_VALIDATION_CLEAN:-0} == 1 && $validation_count != 0 ]]; then
    exit 1
  fi
done

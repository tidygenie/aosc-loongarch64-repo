#!/bin/bash
set -e
SPV=/tmp/spirv-cross
g++ -std=c++17 -O2 -fPIC -shared -o libvkgl.so vkgl.cpp \
  -I"$SPV" \
  -DWAYLAND_CLIENT \
  $(pkg-config --cflags wayland-client 2>/dev/null) \
  $(pkg-config --libs wayland-client 2>/dev/null) \
  -lEGL -lGLESv2 -lgbm \
  "$SPV/build/libspirv-cross-glsl.a" "$SPV/build/libspirv-cross-util.a" "$SPV/build/libspirv-cross-core.a" \
  -lpthread

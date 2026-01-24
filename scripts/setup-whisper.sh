#!/bin/bash
set -e

cd "$(dirname "$0")/.."

echo "Setting up whisper.cpp..."

if [ ! -d "libs/whisper.cpp" ]; then
    echo "Error: libs/whisper.cpp not found (this repo vendors whisper.cpp)."
    exit 1
fi

cd libs/whisper.cpp

# Build static library
make clean 2>/dev/null || true

# Prefer Vulkan backend when available (much faster than CPU on many systems).
# Disable by setting XFCE_WHISPER_VULKAN=0.
GGML_VULKAN_FLAG=""
if [ "${XFCE_WHISPER_VULKAN:-1}" != "0" ]; then
    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists vulkan && command -v glslc >/dev/null 2>&1; then
        GGML_VULKAN_FLAG="GGML_VULKAN=1"
        echo "Vulkan detected via pkg-config; building whisper.cpp with GGML_VULKAN=1"
    else
        echo "Vulkan not detected (missing pkg-config entry). Building whisper.cpp CPU-only."
        echo "To enable Vulkan install Vulkan dev packages + glslc (Debian/Ubuntu: libvulkan-dev + glslc/shaderc; Fedora: vulkan-loader-devel + shaderc)."
    fi
else
    echo "XFCE_WHISPER_VULKAN=0 set; building whisper.cpp CPU-only."
fi

make $GGML_VULKAN_FLAG libwhisper.a -j"$(nproc)"

echo "whisper.cpp built successfully"

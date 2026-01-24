#!/bin/bash
set -e

cd "$(dirname "$0")/.."

WHISPER_VERSION="v1.7.2"

echo "Setting up whisper.cpp..."

mkdir -p libs
cd libs

if [ ! -d "whisper.cpp" ]; then
    git clone --depth 1 --branch "$WHISPER_VERSION" https://github.com/ggerganov/whisper.cpp.git
fi

cd whisper.cpp

# Build static library
make clean 2>/dev/null || true

# Prefer Vulkan backend when available (much faster than CPU on many systems).
# Disable by setting XFCE_WHISPER_VULKAN=0.
GGML_VULKAN_FLAG=""
if [ "${XFCE_WHISPER_VULKAN:-1}" != "0" ]; then
    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists vulkan; then
        GGML_VULKAN_FLAG="GGML_VULKAN=1"
        echo "Vulkan detected via pkg-config; building whisper.cpp with GGML_VULKAN=1"
    else
        echo "Vulkan not detected (missing pkg-config entry). Building whisper.cpp CPU-only."
        echo "To enable Vulkan install Vulkan dev packages (e.g. libvulkan-dev) and rerun."
    fi
else
    echo "XFCE_WHISPER_VULKAN=0 set; building whisper.cpp CPU-only."
fi

make $GGML_VULKAN_FLAG libwhisper.a -j"$(nproc)"

echo "whisper.cpp built successfully"

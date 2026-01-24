#!/bin/bash
set -e

cd "$(dirname "$0")/.."

# ONNX Runtime version
ONNX_VERSION="1.17.0"
ARCH="x64"

echo "Downloading ONNX Runtime ${ONNX_VERSION}..."

mkdir -p libs
cd libs

FILENAME="onnxruntime-linux-${ARCH}-${ONNX_VERSION}.tgz"
URL="https://github.com/microsoft/onnxruntime/releases/download/v${ONNX_VERSION}/${FILENAME}"

if [ ! -d "onnxruntime" ]; then
    curl -L -o "$FILENAME" "$URL"
    tar xzf "$FILENAME"
    mv "onnxruntime-linux-${ARCH}-${ONNX_VERSION}" onnxruntime
    rm "$FILENAME"
fi

echo "ONNX Runtime ready at libs/onnxruntime"

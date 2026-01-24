#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

ARCH="${ARCH:-amd64}"
VERSION="${VERSION:-${GITHUB_REF_NAME:-0.0.0}}"
VERSION="${VERSION#v}"

OUT_DIR="${OUT_DIR:-$ROOT_DIR/dist}"
PKGROOT="$(mktemp -d)"
trap 'rm -rf "$PKGROOT"' EXIT

mkdir -p "$OUT_DIR"

# Build
make clean
make

# Stage install
make install DESTDIR="$PKGROOT" PREFIX=/usr

mkdir -p "$PKGROOT/DEBIAN"
cat > "$PKGROOT/DEBIAN/control" <<EOF
Package: auriscribe
Version: $VERSION
Section: utils
Priority: optional
Architecture: $ARCH
Maintainer: Auriscribe <noreply@example.com>
Depends: libgtk-3-0, libayatana-appindicator3-1, libpulse0, libjson-c5, libcurl4, libx11-6, libstdc++6, libgomp1, libvulkan1
Description: Auriscribe - speech-to-text tray app
 A lightweight GTK tray application using whisper.cpp for offline speech-to-text.
EOF

dpkg-deb --build "$PKGROOT" "$OUT_DIR/auriscribe_${VERSION}_${ARCH}.deb"
echo "Wrote $OUT_DIR/auriscribe_${VERSION}_${ARCH}.deb"

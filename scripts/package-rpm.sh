#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

ARCH="${ARCH:-x86_64}"
VERSION="${VERSION:-${GITHUB_REF_NAME:-0.0.0}}"
VERSION="${VERSION#v}"
RELEASE="${RELEASE:-1}"

OUT_DIR="${OUT_DIR:-$ROOT_DIR/dist}"
mkdir -p "$OUT_DIR"

# Build
make clean
make

TOPDIR="$(mktemp -d)"
trap 'rm -rf "$TOPDIR"' EXIT

mkdir -p "$TOPDIR"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}

TARBALL="$TOPDIR/SOURCES/auriscribe-${VERSION}.tar.gz"
# GitHub Actions can run with a workspace owner mismatch, causing:
# "fatal: detected dubious ownership in repository".
# Use a per-command safe.directory override so rpm packaging works in CI.
git -c safe.directory="$ROOT_DIR" archive --format=tar.gz --prefix="auriscribe-${VERSION}/" -o "$TARBALL" HEAD

cat > "$TOPDIR/SPECS/auriscribe.spec" <<EOF
Name:           auriscribe
Version:        ${VERSION}
Release:        ${RELEASE}%{?dist}
Summary:        Auriscribe speech-to-text tray app
License:        MIT
URL:            https://github.com/your-org/auriscribe
Source0:        auriscribe-${VERSION}.tar.gz

BuildRequires:  gcc, make, pkgconfig
BuildRequires:  gcc-c++
BuildRequires:  gtk3-devel
BuildRequires:  libayatana-appindicator-gtk3-devel
BuildRequires:  pulseaudio-libs-devel
BuildRequires:  json-c-devel
BuildRequires:  libcurl-devel
BuildRequires:  libX11-devel
BuildRequires:  vulkan-loader-devel
BuildRequires:  /usr/bin/glslc

Requires:       gtk3
Requires:       libayatana-appindicator-gtk3
Requires:       pulseaudio-libs
Requires:       json-c
Requires:       libcurl
Requires:       libX11
Requires:       vulkan-loader

%description
A lightweight GTK tray application using whisper.cpp for offline speech-to-text.

%prep
%setup -q

%build
make

%install
rm -rf %{buildroot}
make install DESTDIR=%{buildroot} PREFIX=/usr

%files
%license LICENSE
/usr/bin/auriscribe
/usr/bin/auriscribe-worker
/usr/share/applications/auriscribe.desktop
/usr/share/icons/hicolor/scalable/apps/auriscribe.svg

%changelog
* Tue Jan 01 2026 Auriscribe <noreply@example.com> - ${VERSION}-${RELEASE}
- Automated build
EOF

rpmbuild --define "_topdir ${TOPDIR}" -bb "$TOPDIR/SPECS/auriscribe.spec"
RPM_PATH="$(find "$TOPDIR/RPMS" -type f -name "*.rpm" | head -n 1)"
cp -f "$RPM_PATH" "$OUT_DIR/"
echo "Wrote $OUT_DIR/$(basename "$RPM_PATH")"

#!/bin/sh
# Cross-compile the SS2VR installer for Linux and Windows into dist/.
# Usage: ./build.sh [version]   (default: git describe of this repo, else "dev")
set -eu
cd "$(dirname "$0")"
VERSION="${1:-$(git describe --tags --always --dirty 2>/dev/null || echo dev)}"
mkdir -p dist
build() {
	echo "Building dist/$3 ($1/$2, version $VERSION)"
	CGO_ENABLED=0 GOOS="$1" GOARCH="$2" go build -trimpath \
		-ldflags "-s -w -X main.version=$VERSION" -o "dist/$3" .
}
build linux amd64 ss2vr-installer-linux-x86_64
build windows amd64 ss2vr-installer-windows-x86_64.exe
(cd dist && sha256sum ss2vr-installer-linux-x86_64 ss2vr-installer-windows-x86_64.exe > SHA256SUMS)
ls -l dist

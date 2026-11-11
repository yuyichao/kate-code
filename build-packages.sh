#!/bin/bash
# Build .deb and .rpm packages using Docker
# (Arch Linux packaging is maintained in the archlinuxcn package repositories.)
# Usage: ./build-packages.sh [deb|rpm|all]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

mkdir -p dist

build_deb() {                                                                                                                                                                                                                         
    echo "=== Building .deb package ==="                  
    docker build -f Dockerfile.deb -t kate-code-deb .
    docker rm deb-out 2>/dev/null || true
    docker create --name deb-out kate-code-deb
    docker cp deb-out:/dist/. dist/
    docker rm deb-out
    echo "=== .deb package built in dist/ ==="
}

build_rpm() {
    echo "=== Building .rpm package ==="
    docker build -f Dockerfile.rpm -t kate-code-rpm .
    docker rm rpm-out 2>/dev/null || true
    docker create --name rpm-out kate-code-rpm
    docker cp rpm-out:/dist/. dist/
    docker rm rpm-out
    echo "=== .rpm package built in dist/ ==="
}

case "${1:-all}" in
    deb)
        build_deb
        ;;
    rpm)
        build_rpm
        ;;
    all)
        build_deb
        build_rpm
        ;;
    *)
        echo "Usage: $0 [deb|rpm|all]"
        exit 1
        ;;
esac

echo ""
echo "Packages built:"
ls -la dist/

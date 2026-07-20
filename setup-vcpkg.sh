#!/bin/bash
set -euo pipefail

VCPKG_VERSION="2026.04.27"
VCPKG_ROOT="${VCPKG_ROOT:-$(cd "$(dirname "$0")" && pwd)/vcpkg}"
VCPKG_EXE="$VCPKG_ROOT/vcpkg"

export VCPKG_DEFAULT_TRIPLET="x64-mingw-static"

if [ ! -d "$VCPKG_ROOT" ]; then
    echo "Cloning vcpkg $VCPKG_VERSION into $VCPKG_ROOT ..."
    git clone https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT"
fi

(
    cd "$VCPKG_ROOT"
    echo "Checking out vcpkg $VCPKG_VERSION ..."
    git fetch --tags
    git checkout "$VCPKG_VERSION"

    if [ ! -f "$VCPKG_EXE" ]; then
        echo "Bootstrapping vcpkg ..."
        if [ "$OSTYPE" = "msys" ] || [ "$OSTYPE" = "cygwin" ]; then
            ./bootstrap-vcpkg.bat
        else
            ./bootstrap-vcpkg.sh
        fi
    fi

    echo "Installing dependencies for $VCPKG_DEFAULT_TRIPLET ..."
    ./vcpkg install
)

echo "OK: vcpkg dependencies installed."
echo "Run: ./build.ps1"

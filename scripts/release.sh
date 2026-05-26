#!/bin/sh
set -eu

VERSION="${1:-0.1.16}"
make clean
make VERSION="$VERSION" package

echo
echo "Upload dist/winecord-${VERSION}-macos-universal.tar.gz to:"
echo "  https://github.com/Zard-Studios/WineCord/releases/tag/v${VERSION}"
echo
echo "Then update Formula/winecord.rb if the sha256 changed."

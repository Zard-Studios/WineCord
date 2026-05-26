#!/bin/sh
set -eu

TAP_DIR="${1:-../homebrew-tap}"
VERSION="${VERSION:-0.1.7}"

if [ ! -f "Formula/winecord.rb" ]; then
  echo "Run this from the WineCord repository root." >&2
  exit 1
fi

if [ ! -f "dist/winecord-${VERSION}-macos-universal.tar.gz" ]; then
  echo "Missing dist/winecord-${VERSION}-macos-universal.tar.gz. Run: make package" >&2
  exit 1
fi

mkdir -p "$TAP_DIR/Formula"
mkdir -p "$TAP_DIR/releases"

if [ ! -d "$TAP_DIR/.git" ]; then
  git init -b main "$TAP_DIR" 2>/dev/null || git init "$TAP_DIR"
fi

cp Formula/winecord.rb "$TAP_DIR/Formula/winecord.rb"
cp "dist/winecord-${VERSION}-macos-universal.tar.gz" "$TAP_DIR/releases/"

(
  cd "$TAP_DIR"
  git add Formula/winecord.rb "releases/winecord-${VERSION}-macos-universal.tar.gz"
  git commit -m "Release WineCord ${VERSION}" || true
)

echo "Tap prepared at: $TAP_DIR"
echo
echo "Create/push it as:"
echo "  https://github.com/Zard-Studios/homebrew-tap"
echo
echo "Then users can run:"
echo "  brew install zard-studios/tap/winecord"

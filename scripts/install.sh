#!/bin/sh
set -eu

TAP="${WINECORD_TAP:-Zard-Studios/tap}"
FORMULA="${WINECORD_FORMULA:-winecord}"
OWNER="${TAP%%/*}"
REPO="${TAP#*/}"
TAP_URL="https://github.com/${OWNER}/homebrew-${REPO}.git"

if ! command -v brew >/dev/null 2>&1; then
  echo "Homebrew is required: https://brew.sh" >&2
  exit 1
fi

if ! git ls-remote "$TAP_URL" >/dev/null 2>&1; then
  echo "WineCord Homebrew tap is not published yet:" >&2
  echo "  $TAP_URL" >&2
  echo >&2
  echo "Create that repository and add Formula/winecord.rb first." >&2
  exit 1
fi

brew tap "$TAP"
brew install "$FORMULA"
winecord setup "$@"

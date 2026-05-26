#!/bin/sh
set -eu

TAP="${WINECORD_TAP:-Zard-Studios/tap}"
FORMULA="${WINECORD_FORMULA:-winecord}"

if ! command -v brew >/dev/null 2>&1; then
  echo "Homebrew is required: https://brew.sh" >&2
  exit 1
fi

brew tap "$TAP"
brew install "$FORMULA"
winecord setup "$@"

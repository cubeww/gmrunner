#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/macos-bundle-release"
DIST_DIR="${ROOT_DIR}/dist"

cmake --preset macos-bundle-release
cmake --build --preset macos-bundle-release --parallel

mkdir -p "${DIST_DIR}"
APP_PATH="${BUILD_DIR}/bin/gmrunner.app"
DMG_PATH="${DIST_DIR}/gmrunner-macos.dmg"

rm -f "${DMG_PATH}"
hdiutil create -volname "gmrunner" -srcfolder "${APP_PATH}" -ov -format UDZO "${DMG_PATH}"
echo "Packaged ${DMG_PATH}"

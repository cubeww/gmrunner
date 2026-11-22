#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/linux-x64-release"
DIST_DIR="${ROOT_DIR}/dist/gmrunner-linux-x64"

cmake --preset linux-x64-release
cmake --build --preset linux-x64-release --parallel

rm -rf "${DIST_DIR}"
mkdir -p "${DIST_DIR}/bin" "${DIST_DIR}/share/applications"

cp "${BUILD_DIR}/bin/gmrunner" "${DIST_DIR}/bin/gmrunner"
cp "${ROOT_DIR}/packaging/linux/gmrunner.desktop" "${DIST_DIR}/share/applications/gmrunner.desktop"
cp "${ROOT_DIR}/LICENSE" "${DIST_DIR}/LICENSE"
cp "${ROOT_DIR}/README.md" "${DIST_DIR}/README.md"

tar -C "${ROOT_DIR}/dist" -czf "${ROOT_DIR}/dist/gmrunner-linux-x64.tar.gz" gmrunner-linux-x64
echo "Packaged ${ROOT_DIR}/dist/gmrunner-linux-x64.tar.gz"

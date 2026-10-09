#!/usr/bin/env bash
# Build this checkout and generate a native NSIS installer, without installing it.
set -euo pipefail
[[ "${MSYSTEM:-}" == UCRT64 ]] || { echo 'Use the MSYS2 UCRT64 environment.' >&2; exit 1; }
cd "$(dirname "$0")/../.."
: "${SUNSHINE_NODE_DIR:?Native Node.js directory is required. Run build.bat.}"
export PATH="$(cygpath -u "$SUNSHINE_NODE_DIR"):$PATH"
node -e 'if (process.platform !== "win32" || process.arch !== "x64" || Number(process.versions.node.split(".")[0]) < 22) process.exit(1)'
for command in cmake ninja g++ makensis; do
  command -v "$command" >/dev/null || { echo "Missing $command. Run setup.bat." >&2; exit 1; }
done
git submodule update --init --recursive
cmake -S . -B cmake-build-windows -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_DOCS=OFF -DBUILD_TESTS=OFF \
  -DSUNSHINE_BUILD_WIX=OFF -DSUNSHINE_ASSETS_DIR=assets \
  -DSUNSHINE_PUBLISHER_NAME=JacKAsterisK \
  -DSUNSHINE_PUBLISHER_WEBSITE=https://github.com/JacKAsterisK/Sunshine \
  -DSUNSHINE_PUBLISHER_ISSUE_URL=https://github.com/JacKAsterisK/Sunshine/issues
cmake --build cmake-build-windows --parallel "${SUNSHINE_BUILD_JOBS:-4}"
cpack -G NSIS --config cmake-build-windows/CPackConfig.cmake

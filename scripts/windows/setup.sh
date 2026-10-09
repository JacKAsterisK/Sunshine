#!/usr/bin/env bash
# Install the same UCRT64 native dependencies used by ci-windows.yml.
set -euo pipefail
[[ "${MSYSTEM:-}" == UCRT64 ]] || { echo 'Use the MSYS2 UCRT64 environment.' >&2; exit 1; }
dependencies=(git)
for package in boost cmake cppwinrt curl-winssl gcc miniupnpc nlohmann-json onevpl openssl opus toolchain qt6-static MinHook nsis; do
  dependencies+=("mingw-w64-ucrt-x86_64-${package}")
done
pacman -S --needed --noconfirm "${dependencies[@]}"

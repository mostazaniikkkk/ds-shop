#!/bin/sh
# Builds the homebrew inside the official devkitPro Docker image, so nothing
# has to be installed or changed on the host: only Docker is needed.
#   ./build.sh          -> DSShop.nds
#   ./build.sh r4       -> also DSShop_R4.nds (patched with the R4 DLDI driver)
#   ./build.sh clean
# Pinned image: devkitARM r65 with libnds 1.8.3 / dswifi 0.4.2 (the code
# targets the libnds 1.x API; newer images ship libnds 2.x).
IMAGE=devkitpro/devkitarm:20240918@sha256:62dd4415568347a2d18b75c754bf173a0ef3e253ef8ad4aa28782cd121bb7bf6

cd "$(dirname "$0")" || exit 1
HERE="$(pwd)"
# Docker Desktop on Windows (Git Bash/MSYS) needs a Windows path for the mount
if command -v cygpath >/dev/null 2>&1; then HERE="$(cygpath -w "$HERE")"; fi

MSYS_NO_PATHCONV=1 exec docker run --rm -v "$HERE:/src" -w /src "$IMAGE" make "$@"

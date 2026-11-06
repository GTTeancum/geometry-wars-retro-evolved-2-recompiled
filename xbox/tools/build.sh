#!/usr/bin/env bash
# Builds the Xbox XBE: prepares the recompiled sources, then runs nxdk's make.
#   xbox/tools/build.sh [make targets...]
# nxdk needs an MSYS2 MinGW64 shell; from any other shell (Git Bash) the
# script re-runs itself in C:\msys64.
set -euo pipefail

xbox_dir="$(cd "$(dirname "$0")/.." && pwd)"

if ! command -v make >/dev/null; then
  MSYS_BASH="${MSYS_BASH:-/c/msys64/usr/bin/bash.exe}"
  exec env MSYSTEM=MINGW64 CHERE_INVOKING=1 "${MSYS_BASH}" -lc \
    "cd '${xbox_dir}' && tools/build.sh $*"
fi

NXDK_DIR="${NXDK_DIR:-/c/nxdk}"
LLVM_BIN="${LLVM_BIN:-/c/Program Files/LLVM/bin}"
export NXDK_DIR
export PATH="${LLVM_BIN}:${NXDK_DIR}/bin:/usr/bin:/mingw64/bin:${PATH}"

python "${xbox_dir}/tools/prepare_generated.py"
cd "${xbox_dir}"
exec make -j"$(nproc)" "$@"

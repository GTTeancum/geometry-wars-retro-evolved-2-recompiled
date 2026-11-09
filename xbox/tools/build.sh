#!/usr/bin/env bash
# Builds the Xbox XBE: prepares the recompiled sources, then runs nxdk's make.
#   xbox/tools/build.sh [make targets...]
# nxdk needs an MSYS2 MinGW64 shell; from any other shell (Git Bash) the
# script re-runs itself in C:\msys64.
set -euo pipefail

xbox_dir="$(cd "$(dirname "$0")/.." && pwd)"

if ! command -v make >/dev/null; then
  # (The Python here has the image libraries tools/xbe_meta.py needs; the
  # MSYS2 shell's own does not.)
  # The login shell starts from a clean environment: passed in its command.
  GW_PYTHON="${GW_PYTHON:-$(command -v python || true)}"
  MSYS_BASH="${MSYS_BASH:-/c/msys64/usr/bin/bash.exe}"
  exec env MSYSTEM=MINGW64 CHERE_INVOKING=1 "${MSYS_BASH}" -lc \
    "cd '${xbox_dir}' && GW_PYTHON='${GW_PYTHON}' tools/build.sh $*"
fi

NXDK_DIR="${NXDK_DIR:-/c/nxdk}"
LLVM_BIN="${LLVM_BIN:-/c/Program Files/LLVM/bin}"
export NXDK_DIR
export PATH="${LLVM_BIN}:${NXDK_DIR}/bin:/usr/bin:/mingw64/bin:${PATH}"

python "${xbox_dir}/tools/prepare_generated.py"
cd "${xbox_dir}"
make -j"$(nproc)" "$@"
# The 360 game's title ID in the XBE (its saves' folder; cxbe has no switch).
if [ -f build/release/default.xbe ]; then
  python "${xbox_dir}/tools/xbe_meta.py" title-id build/release/default.xbe
  # The dashboard's name and the game's icon in it, from this machine's copy
  # of the game; default.plain.xbe keeps it without them (the installer's).
  image="${xbox_dir}/../game/analysis/default.image.bin"
  if [ -f "${image}" ]; then
    "${GW_PYTHON:-python}" "${xbox_dir}/tools/xbe_meta.py" dashboard build/release/default.xbe "${image}" build/release build/release/default.plain.xbe
  else
    cp build/release/default.xbe build/release/default.plain.xbe
  fi
fi

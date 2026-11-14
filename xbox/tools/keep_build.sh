#!/usr/bin/env bash
# Test only: keeps the build just made (its XBE, link map and image) under
# build/variants/<name>, for tools/xbdm_deploy.sh <folder>.
#   bash xbox/tools/keep_build.sh <name>
set -euo pipefail
xbox="$(cd "$(dirname "$0")/.." && pwd)"
dir="$xbox/build/variants/$1"
mkdir -p "$dir"
cp "$xbox/build/release/default.xbe" "$xbox/build/gw2.map" "$xbox/main.exe" "$dir/"
echo "$dir"

#!/usr/bin/env bash
# Test only: puts a built XBE on the console and starts it. The game running
# now is first asked to leave the APU quiet (tools/xbdm_quiet.py: only if it
# is the build last deployed), so a chip still playing is not left writing
# into memory the next program owns. The new build's link map and image are
# kept as build/deployed.map and build/deployed.exe for that.
#   bash xbox/tools/xbdm_deploy.sh            the build just made
#   bash xbox/tools/xbdm_deploy.sh <folder>   a build kept by tools/keep_build.sh
set -euo pipefail
xbox="$(cd "$(dirname "$0")/.." && pwd)"
bin=/c/XDK/Xbox/Bin
if [ $# -ge 1 ]; then
  xbe="$1/default.xbe"; map="$1/gw2.map"; exe="$1/main.exe"
else
  xbe="$xbox/build/release/default.xbe"; map="$xbox/build/gw2.map"; exe="$xbox/main.exe"
fi
python "$xbox/tools/xbdm_quiet.py" 10.0.0.231 >/dev/null 2>&1 && python -c "import time; time.sleep(0.5)" || true
"$bin/xbcp.exe" -y -t "$(cygpath -w "$xbe")" 'xe:\Games\GW2\default.xbe' >/dev/null
cp "$map" "$xbox/build/deployed.map"
cp "$exe" "$xbox/build/deployed.exe"
# (a kept build: its map and image also where the xbdm tools look for the running build's)
if [ $# -ge 1 ]; then cp "$map" "$xbox/build/gw2.map"; cp "$exe" "$xbox/main.exe"; fi
"$bin/xbreboot.exe" 'xe:\Games\GW2\default.xbe' >/dev/null

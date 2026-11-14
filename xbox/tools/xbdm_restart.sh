#!/usr/bin/env bash
# Test only: restarts the game on the console through the debug monitor, the
# APU left quiet first (tools/xbdm_quiet.py: a session killed while the APU
# runs leaves it running, src/apu.cpp). Software sound builds have no such
# switch; nothing is asked then.
#   bash xbox/tools/xbdm_restart.sh
xbox="$(cd "$(dirname "$0")/.." && pwd)"
python "$xbox/tools/xbdm_quiet.py" 10.0.0.231 >/dev/null 2>&1 && python -c "import time; time.sleep(0.5)"
/c/XDK/Xbox/Bin/xbreboot.exe 'xe:\Games\GW2\default.xbe'

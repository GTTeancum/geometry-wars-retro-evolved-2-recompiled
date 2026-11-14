#!/usr/bin/env bash
# Test only: the slowdown benchmark. Restarts the game on the console (a
# TEST_CHEATS build: invincible, every mode shown), starts Evolved and lets
# enemies pile up with nobody firing, recording the game's speed.
#   bash xbox/tools/stress_evolved.sh <name> [seconds]
# Output: xbox/test-artifacts/stress-<name>.log and a screenshot at the end.
set -euo pipefail
name="$1"
seconds="${2:-360}"
xbox="$(cd "$(dirname "$0")/.." && pwd)"
host=10.0.0.231
bash "$xbox/tools/xbdm_restart.sh"  # (the APU left quiet first)
python -c "import time; time.sleep(25)"
python "$xbox/tools/xemu_pad.py" --xbdm "$host" "press A" "sleep 3" "press UP" "sleep 0.5" "press UP" "sleep 0.5" \
  "press UP" "sleep 0.5" "press DOWN" "sleep 0.5" "press DOWN" "sleep 1" "press A" "sleep 3" "press A" "sleep 3" \
  "shot stress-$name-start" >/dev/null
python "$xbox/tools/xbdm_speed.py" "$host" "$seconds" --every 10 > "$xbox/test-artifacts/stress-$name.log"
python "$xbox/tools/xbdm_shot.py" "$host" "$xbox/test-artifacts/stress-$name-end.png" >/dev/null
tail -1 "$xbox/test-artifacts/stress-$name.log"

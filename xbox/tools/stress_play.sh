#!/usr/bin/env bash
# Test only: the playing benchmark. Like stress_evolved.sh, but the ship moves
# and fires (tools/stress_fire.txt: steps timed in game updates, so a run
# repeats closely), as a player keeps the crowd down.
#   bash xbox/tools/stress_play.sh <name> [seconds]
set -euo pipefail
name="$1"
seconds="${2:-360}"
xbox="$(cd "$(dirname "$0")/.." && pwd)"
host=10.0.0.231
bash "$xbox/tools/xbdm_restart.sh"  # (the APU left quiet first)
python -c "import time; time.sleep(25)"
python "$xbox/tools/xemu_pad.py" --xbdm "$host" "press A" "sleep 3" "press UP" "sleep 0.5" "press UP" "sleep 0.5" \
  "press UP" "sleep 0.5" "press DOWN" "sleep 0.5" "press DOWN" "sleep 1" "press A" "sleep 3" "press A" "sleep 3" >/dev/null
python "$xbox/tools/xemu_pad.py" --xbdm "$host" --file "$xbox/tools/stress_fire.txt" > "$xbox/test-artifacts/play-$name.log" 2>&1 &
python "$xbox/tools/xbdm_speed.py" "$host" "$seconds" --every 10 > "$xbox/test-artifacts/stress-$name.log"
wait
python "$xbox/tools/xbdm_shot.py" "$host" "$xbox/test-artifacts/stress-$name-end.png" >/dev/null || true
tail -1 "$xbox/test-artifacts/stress-$name.log"

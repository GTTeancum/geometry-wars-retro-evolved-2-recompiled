#!/usr/bin/env bash
# Test only: the last "APU bins" report lines of the running game (peaks over 5 s).
xbox="$(cd "$(dirname "$0")/.." && pwd)"
PYTHONIOENCODING=utf-8 timeout 60 python "$xbox/tools/xbdm_log.py" 10.0.0.231 --tail 9000 2>&1 | grep -aE "APU bins" | tail -${1:-1} | cut -c1-500

"""Print the game's log ring (gw_log_ring) from a development Xbox through
xbdm, as tools/read_log.py does for XEMU: works while the game runs, and
after it hangs, when D:\\gw2log.txt is no longer written.

  python xbox/tools/xbdm_log.py <console address> [--tail 6000]

Addresses come from the link map (xbox/build/gw2.map).
"""
import argparse
import pathlib
import re
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import xbdm_shot  # noqa: E402


def symbol(text, name):
    m = re.search(r'\s_' + name + r'\s+([0-9a-fA-F]{8,16})\s', text)
    if not m:
        raise SystemExit(f'{name} not in the map')
    return int(m[1], 16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('host')
    ap.add_argument('--tail', type=int, default=6000)
    args = ap.parse_args()
    text = (pathlib.Path(__file__).resolve().parents[1] / 'build' / 'gw2.map').read_text(errors='replace')
    x = xbdm_shot.Xbdm(args.host)
    pos_at, ring_at = symbol(text, 'gw_log_pos'), symbol(text, 'gw_log_ring')
    pos = struct.unpack('<I', x.getmem(pos_at, 4))[0]
    size = 65536
    n = min(pos, size, args.tail)
    out = bytearray()
    for p in range(pos - n, pos, 0x800):
        k = min(0x800, pos - p)
        start = p & (size - 1)
        first = min(k, size - start)
        out += x.getmem(ring_at + start, first)
        if first < k:
            out += x.getmem(ring_at, k - first)
    print(f'log position {pos}')
    sys.stdout.write(out.decode(errors='replace'))


if __name__ == '__main__':
    main()

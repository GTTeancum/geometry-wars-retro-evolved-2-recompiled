"""Game speed over time on a development Xbox, through xbdm: every few
seconds the game's updates and drawn pictures per second (gw_stats; 60
updates a second is full speed), optionally a screenshot every N seconds.

  python xbox/tools/xbdm_speed.py <console address> <seconds> [--every 5] [--shots 60 NAME]

Screenshots go to xbox/test-artifacts/speed/NAME-<seconds>.png.
"""
import argparse
import pathlib
import re
import struct
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import xbdm_shot  # noqa: E402

XBOX = pathlib.Path(__file__).resolve().parents[1]


def symbol(text, name):
    m = re.search(r'\s+\d{4}:[\da-fA-F]+\s+(?:_' + name + r'|\?' + name + r'@@\S*)\s+([\da-fA-F]+)', text)
    if not m:
        raise SystemExit(f'{name} not in the link map')
    return int(m[1], 16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('host')
    ap.add_argument('seconds', type=float)
    ap.add_argument('--every', type=float, default=5)
    ap.add_argument('--shots', nargs=2, metavar=('SECONDS', 'NAME'))
    args = ap.parse_args()
    text = (XBOX / 'build' / 'gw2.map').read_text(errors='replace')
    stats = symbol(text, 'gw_stats')
    x = xbdm_shot.Xbdm(args.host)
    out = XBOX / 'test-artifacts' / 'speed'
    out.mkdir(parents=True, exist_ok=True)

    def read():
        swaps, drawn = struct.unpack('<2I', x.getmem(stats, 8))
        return time.time(), swaps, drawn

    start = last = read()
    next_shot = 0.0
    rates = []
    while last[0] - start[0] < args.seconds:
        time.sleep(args.every)
        now = read()
        dt = now[0] - last[0]
        ups, fps = (now[1] - last[1]) / dt, (now[2] - last[2]) / dt
        rates.append(ups)
        line = f'{now[0] - start[0]:6.0f} s  update {now[1] - start[1]:7d}  updates/s {ups:5.1f}  pictures/s {fps:5.1f}'
        if args.shots and now[0] - start[0] >= next_shot:
            name = f'{args.shots[1]}-{int(now[0] - start[0]):04d}.png'
            try:
                subprocess.run([sys.executable, str(XBOX / 'tools' / 'xbdm_shot.py'), args.host, str(out / name)],
                               check=True, capture_output=True, timeout=60)
            except Exception as e:  # a screenshot is a nice-to-have
                line += f'  (shot failed: {e})'
            next_shot += float(args.shots[0])
        print(line, flush=True)
        last = now
    if rates:
        rates.sort()
        print(f'updates/s: median {rates[len(rates) // 2]:.1f}, lowest {rates[0]:.1f}')


if __name__ == '__main__':
    main()

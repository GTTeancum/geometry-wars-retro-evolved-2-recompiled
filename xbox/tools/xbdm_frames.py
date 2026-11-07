"""Fetch the frames recorded around the ship's spawn (make SPAWN_CAPTURE=y,
src/nv2a.cpp) from a development Xbox through xbdm, as PNGs.

  python xbox/tools/xbdm_frames.py <console address> <output directory>

Addresses come from the link map (xbox/build/gw2.map).
"""
import pathlib
import re
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import xbdm_shot  # noqa: E402

FRAMES, W, H = 24, 320, 180


def symbol(text, name):
    m = re.search(r'\s_' + name + r'\s+([0-9a-fA-F]{8,16})\s', text)
    if not m:
        raise SystemExit(f'{name} not in the map')
    return int(m[1], 16)


def main():
    host, out = sys.argv[1], pathlib.Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    text = (pathlib.Path(__file__).resolve().parents[1] / 'build' / 'gw2.map').read_text(errors='replace')
    state_at, frames_at = symbol(text, 'gw_capture_state'), symbol(text, 'gw_capture_frames')
    x = xbdm_shot.Xbdm(host)
    magic, written, frozen, left = struct.unpack('<4I', x.getmem(state_at, 16))
    print(f'magic {magic:08X} written {written} frozen {frozen} left {left}')
    for n in range(min(written, FRAMES)):
        # oldest first
        k = (written - min(written, FRAMES) + n) % FRAMES
        data = x.getmem(frames_at + k * W * H * 2, W * H * 2)
        rows = []
        for y in range(H):
            row = bytearray()
            for px in struct.unpack_from(f'<{W}H', data, y * W * 2):
                row += bytes(((px >> 11) << 3, ((px >> 5) & 63) << 2, (px & 31) << 3))
            rows.append(bytes(row))
        path = out / f'frame{n:02d}.png'
        path.write_bytes(xbdm_shot.png_bytes(W, H, rows) if hasattr(xbdm_shot, 'png_bytes') else b'')
        if not hasattr(xbdm_shot, 'png_bytes'):
            xbdm_shot.png(str(path), W, H, rows)
        print(path)


if __name__ == '__main__':
    main()

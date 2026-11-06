"""Save and decode the one-frame GPU capture (xbox/src/gpu_capture.cpp).

  python xbox/tools/gpu_capture.py [--port 9257] [--out xbox/test-artifacts/gpu]

Writes capture.bin, one shader_<type>_<address>.bin per shader, and
report.txt (one line per draw with the state that matters for rendering).
"""
import argparse
import collections
import pathlib
import re
import socket
import struct
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]

PRIMS = {1: 'points', 2: 'lines', 3: 'line_strip', 4: 'triangles', 5: 'tri_fan', 6: 'tri_strip',
         8: 'rects', 13: 'quads', 14: 'quad_strip'}
SOURCES = {0: 'indexed', 1: 'immediate', 2: 'auto'}
NAMES = {
    0x2000: 'RB_SURFACE_INFO', 0x2001: 'RB_COLOR_INFO', 0x2002: 'RB_DEPTH_INFO',
    0x2104: 'RB_COLOR_MASK', 0x2180: 'SQ_PROGRAM_CNTL', 0x2200: 'RB_DEPTHCONTROL',
    0x2201: 'RB_BLENDCONTROL0', 0x2202: 'RB_COLORCONTROL', 0x2205: 'PA_SU_SC_MODE_CNTL',
    0x2206: 'PA_CL_VTE_CNTL', 0x2208: 'RB_MODECONTROL', 0x2318: 'RB_COPY_CONTROL',
    0x2319: 'RB_COPY_DEST_BASE', 0x231A: 'RB_COPY_DEST_PITCH', 0x231B: 'RB_COPY_DEST_INFO',
}


def symbol(map_text, name):
    m = re.search(r'\s+\d{4}:[\da-fA-F]+\s+(?:_' + name + r'|\?' + name + r'@@\S*)\s+([\da-fA-F]+)', map_text)
    if not m:
        raise SystemExit(f'{name} not in the link map')
    return int(m[1], 16)


class Monitor:
    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=30)
        self.response()

    def response(self):
        data = b''
        while not data.endswith(b'(qemu) '):
            part = self.sock.recv(65536)
            if not part:
                raise SystemExit('monitor closed')
            data += part
        return data.decode(errors='replace')

    def command(self, text):
        self.sock.sendall((text + '\n').encode())
        return self.response()

    def word(self, address):
        text = self.command(f'x /1wx 0x{address:x}')
        found = re.findall(r':\s+0x([0-9a-fA-F]+)', text)
        return int(found[0], 16) if found else None  # None until the XBE is loaded


def decode(words, out):
    regs = {}
    shaders = {}
    lines = []
    i = 0
    draws = 0
    stats = collections.Counter()
    while i < len(words):
        tag = words[i]
        i += 1
        if tag == 0x50414E53:  # SNAP
            for k in range(0x400):
                regs[0x2000 + k] = words[i + k]
            i += 0x400
            for k in range(0x928):
                regs[0x4000 + k] = words[i + k]
            i += 0x928
        elif tag == 0x52444853:  # SHDR
            kind, address, n = words[i:i + 3]
            i += 3
            blob = words[i:i + n]
            i += n
            shaders[address] = (kind, blob)
            name = f"shader_{'vs' if kind == 0 else 'ps'}_{address:08X}.bin"
            (out / name).write_bytes(struct.pack(f'>{n}I', *blob))
        elif tag == 0x57415244:  # DRAW
            opcode, n = words[i:i + 2]
            i += 2
            packet = words[i:i + n]
            i += n
            vs, ps, changed = words[i:i + 3]
            i += 3
            for k in range(changed):
                regs[words[i]] = words[i + 1]
                i += 2
            initiator = packet[1] if opcode == 0x22 else packet[0]
            prim = initiator & 0x3F
            source = (initiator >> 6) & 3
            count = initiator >> 16
            mode = regs.get(0x2208, 0) & 7
            surface = regs.get(0x2000, 0)
            color = regs.get(0x2001, 0)
            blend = regs.get(0x2201, 0)
            textures = []
            for slot in range(16):
                f = [regs.get(0x4800 + slot * 6 + j, 0) for j in range(6)]
                if f[0] & 3 == 2:  # texture fetch constant
                    fmt = f[1] & 0x3F
                    w = (f[2] & 0x1FFF) + 1
                    h = ((f[2] >> 13) & 0x1FFF) + 1
                    textures.append(f't{slot}:{w}x{h} fmt{fmt} @{(f[1] & ~0xFFF):08X}')
            lines.append(
                f'{draws:4d} {PRIMS.get(prim, prim)!s:10} {SOURCES.get(source, source):9} n={count:5d} '
                f'vs={vs:08X} ps={ps:08X} mode={mode} surf={surface:08X} color={color:08X} '
                f'blend={blend:08X} {" ".join(textures)}')
            stats[(vs, ps)] += 1
            draws += 1
        elif tag == 0x50415753:  # SWAP
            fb, w, h = words[i:i + 3]
            i += 3
            lines.append(f'SWAP frontbuffer {fb:08X} {w}x{h}')
        else:
            lines.append(f'unknown record {tag:08X} at word {i - 1}')
            break
    summary = [f'{draws} draws, {len(shaders)} shaders captured', 'shader pairs (vs, ps): draws']
    summary += [f'  {vs:08X} {ps:08X}: {n}' for (vs, ps), n in stats.most_common()]
    (out / 'report.txt').write_text('\n'.join(summary + [''] + lines) + '\n')
    print('\n'.join(summary))


def save_shaders(mon, base, out):
    count, used = mon.word(base + 4), mon.word(base + 8)
    target = (out / 'shaders.bin').resolve()
    mon.command(f'memsave 0x{base + 12:x} {used * 4} "{target.as_posix()}"')
    time.sleep(1)
    data = target.read_bytes()
    words = struct.unpack(f'<{len(data) // 4}I', data)
    i = 0
    while i < len(words) and words[i] == 0x52444853:
        kind, h, n = words[i + 1:i + 4]
        blob = words[i + 4:i + 4 + n]
        name = f"hash_{'vs' if kind == 0 else 'ps'}_{h:08X}.bin"
        (out / name).write_bytes(struct.pack(f'>{n}I', *blob))
        i += 4 + n
    print(f'{count} shaders saved to {out}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=9257)
    parser.add_argument('--out', type=pathlib.Path, default=ROOT / 'xbox' / 'test-artifacts' / 'gpu')
    parser.add_argument('--shaders', action='store_true',
                        help='save every distinct shader loaded so far (gw_shader_dump), named by hash')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    map_text = (ROOT / 'xbox' / 'build' / 'gw2.map').read_text(errors='replace')
    if args.shaders:
        save_shaders(Monitor(args.port), symbol(map_text, 'gw_shader_dump'), args.out)
        return
    base = symbol(map_text, 'gw_capture')
    mon = Monitor(args.port)
    for _ in range(180):
        if mon.word(base) == 0x50414347 and mon.word(base + 4) == 2:
            break
        time.sleep(1)
    else:
        raise SystemExit(f'capture not finished (state {mon.word(base + 4)})')
    used = mon.word(base + 8)
    target = (args.out / 'capture.bin').resolve()
    mon.command(f'memsave 0x{base + 12:x} {used * 4} "{target.as_posix()}"')
    time.sleep(1)
    data = target.read_bytes()
    words = list(struct.unpack(f'<{len(data) // 4}I', data))
    decode(words, args.out)


if __name__ == '__main__':
    main()

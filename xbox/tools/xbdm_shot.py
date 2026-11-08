"""Screenshot of a development Xbox running the game, through its debug
monitor (xbdm, port 731). xbdm's own 'screenshot' (and RXDK xbcapture) look
for the XDK Direct3D's state, which an nxdk program does not have; instead
this reads the scan-out address from the NV2A's PCRTC_START register and the
front buffer from memory with 'getmem'.

  python xbox/tools/xbdm_shot.py <console address> <out.png> [width height]

The frame is R5G6B5; its width, height and pitch are pbkit's own
(pb_FrameBuffersWidth/Height/Pitch, addresses from the link map
xbox/build/gw2.map), unless given.
"""
import pathlib
import re
import socket
import struct
import sys
import zlib

PCRTC_START = 0xFD600800
TILE_PITCHES = [0x0200, 0x0300, 0x0400, 0x0500, 0x0600, 0x0700, 0x0800, 0x0A00, 0x0C00, 0x0E00, 0x1000,
                0x1400, 0x1800, 0x1C00, 0x2000, 0x2800]


class Xbdm:
    def __init__(self, host):
        self.sock = socket.create_connection((host, 731), timeout=20)
        self.buf = b''
        self.line()  # 201- connected

    def line(self):
        while b'\r\n' not in self.buf:
            part = self.sock.recv(1 << 20)
            if not part:
                raise SystemExit('connection closed')
            self.buf += part
        line, _, self.buf = self.buf.partition(b'\r\n')
        return line.decode(errors='replace')

    def getmem(self, address, length):
        self.sock.sendall(f'getmem addr=0x{address:08X} length=0x{length:X}\r\n'.encode())
        status = self.line()
        if not status.startswith('202'):
            raise SystemExit(f'xbdm getmem: {status}')
        data = bytearray()
        while True:
            line = self.line()
            if line == '.':
                return bytes(data)
            data += bytes.fromhex(line.replace('??', '00'))


def png(path, width, height, rgb_rows):
    raw = b''.join(b'\0' + row for row in rgb_rows)

    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)

    data = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
    data += chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(data)


def pbkit_frame(x):
    """(width, height, pitch) of pbkit's frame buffers, from the console."""
    text = (pathlib.Path(__file__).resolve().parents[1] / 'build' / 'gw2.map').read_text(errors='replace')
    values = []
    for name in ('Width', 'Height', 'Pitch'):
        m = re.search(r'\s_pb_FrameBuffers' + name + r'\s+([0-9a-fA-F]+)\s', text)
        values.append(struct.unpack('<I', x.getmem(int(m[1], 16), 4))[0])
    return tuple(values)


def check_build(x):
    """Refuses to go on unless the running game is the build that
    xbox/build/gw2.map describes: its stamp (gw_build_stamp, src/build_stamp.cpp)
    read from the console must equal the one in the built xbox/main.exe. A map
    from another build names other addresses: reading through them gives
    nonsense, writing (the pad tool) hits whatever the running build keeps
    there."""
    xbox = pathlib.Path(__file__).resolve().parents[1]
    text = (xbox / 'build' / 'gw2.map').read_text(errors='replace')
    m = re.search(r'\s_gw_build_stamp\s+([0-9a-fA-F]+)\s', text)
    if not m:
        raise SystemExit('gw_build_stamp not in xbox/build/gw2.map')
    va = int(m[1], 16)
    image = (xbox / 'main.exe').read_bytes()
    pe = struct.unpack_from('<I', image, 0x3C)[0]
    sections, optional_size = struct.unpack_from('<H', image, pe + 6)[0], struct.unpack_from('<H', image, pe + 20)[0]
    image_base = struct.unpack_from('<I', image, pe + 24 + 28)[0]
    rva, built = va - image_base, None
    for s in range(sections):
        at = pe + 24 + optional_size + s * 40
        size, address, raw_size, raw = struct.unpack_from('<IIII', image, at + 8)
        if address <= rva < address + max(size, raw_size):
            built = image[raw + rva - address:raw + rva - address + 8]
            break
    running = x.getmem(va, 8)
    if built is None or running != built:
        raise SystemExit(f'the console runs another build than xbox/build/gw2.map describes (stamp '
                         f'{running.hex()}, built {built.hex() if built else "?"}): deploy the build first')


def main():
    host, out = sys.argv[1], sys.argv[2]
    x = Xbdm(host)
    check_build(x)
    if len(sys.argv) > 4:
        width, height = int(sys.argv[3]), int(sys.argv[4])
        pitch = next(p for p in TILE_PITCHES if p >= width * 2)
    else:
        width, height, pitch = pbkit_frame(x)
    start = struct.unpack('<I', x.getmem(PCRTC_START, 4))[0]
    frame = bytearray()
    for offset in range(0, pitch * height, 0x8000):
        frame += x.getmem(0x80000000 | (start + offset), min(0x8000, pitch * height - offset))
    rows = []
    for y in range(height):
        px = struct.unpack('<%dH' % width, frame[y * pitch:y * pitch + width * 2])
        rows.append(bytes(c for p in px for c in (((p >> 11) & 31) * 255 // 31, ((p >> 5) & 63) * 255 // 63,
                                                 (p & 31) * 255 // 31)))
    png(out, width, height, rows)
    print(f'front buffer at {start:#x}, {width}x{height} -> {out}')


if __name__ == '__main__':
    main()

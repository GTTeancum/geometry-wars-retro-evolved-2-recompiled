"""Screenshot of a development Xbox running the game, through its debug
monitor (xbdm, port 731). xbdm's own 'screenshot' (and RXDK xbcapture) look
for the XDK Direct3D's state, which an nxdk program does not have; instead
this reads the scan-out address from the NV2A's PCRTC_START register and the
front buffer from memory with 'getmem'.

  python xbox/tools/xbdm_shot.py <console address> <out.png> [width height]

The back buffers are 640x480 R5G6B5 (src/status.cpp, src/nv2a.cpp) unless
given; the pitch is the width's bytes rounded to pbkit's tile pitches.
"""
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


def main():
    host, out = sys.argv[1], sys.argv[2]
    width, height = (int(sys.argv[3]), int(sys.argv[4])) if len(sys.argv) > 4 else (640, 480)
    pitch = next(p for p in TILE_PITCHES if p >= width * 2)
    x = Xbdm(host)
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

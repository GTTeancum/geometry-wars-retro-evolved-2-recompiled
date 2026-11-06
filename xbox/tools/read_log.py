"""Read the game's log ring (gw_log_ring) from a running XEMU via its monitor.

  python xbox/tools/read_log.py [--port 9257] [--tail 4000]

Symbol addresses come from xbox/build/gw2.map (nxdk-link /map).
"""
import argparse
import pathlib
import re
import socket
import struct

ROOT = pathlib.Path(__file__).resolve().parents[2]


def symbol(map_text, name):
    # C names are _name; C++ globals are mangled as ?name@@...
    m = re.search(r'\s+\d{4}:[\da-fA-F]+\s+(?:_' + name + r'|\?' + name + r'@@\S*)\s+([\da-fA-F]+)', map_text)
    if not m:
        raise SystemExit(f'{name} not in the link map')
    return int(m[1], 16)


class Monitor:
    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=5)
        self.response()

    def response(self):
        data = b''
        while not data.endswith(b'(qemu) '):
            part = self.sock.recv(65536)
            if not part:
                raise SystemExit('monitor closed')
            data += part
        return data.decode(errors='replace')

    def read(self, address, size):
        out = bytearray()
        while len(out) < size:
            words = min(256, (size - len(out) + 3) // 4)
            self.sock.sendall(f'x /{words}wx 0x{address + len(out):x}\n'.encode())
            text = self.response()
            values = []
            for line in text.splitlines():
                if re.match(r'^\s*[0-9a-fA-F]+:', line):
                    values += [int(v, 16) for v in re.findall(r'0x([0-9a-fA-F]+)', line.split(':', 1)[1])]
            if not values:
                raise SystemExit('monitor returned no data: ' + text)
            for v in values[:words]:
                out += struct.pack('<I', v)
        return bytes(out[:size])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=9257)
    parser.add_argument('--tail', type=int, default=4000)
    args = parser.parse_args()
    map_text = (ROOT / 'xbox' / 'build' / 'gw2.map').read_text(errors='replace')
    pos_addr = symbol(map_text, 'gw_log_pos')
    ring_addr = symbol(map_text, 'gw_log_ring')
    stats_addr = symbol(map_text, 'gw_stats')
    mon = Monitor(args.port)
    pos = struct.unpack('<I', mon.read(pos_addr, 4))[0]
    stats = struct.unpack('<8I', mon.read(stats_addr, 32))
    size = 65536
    tail = min(args.tail, pos, size)
    start = pos - tail
    ring = mon.read(ring_addr, size) if tail > size // 2 else None
    if ring is None:
        first = start % size
        if first + tail <= size:
            text = mon.read(ring_addr + first, tail)
        else:
            text = mon.read(ring_addr + first, size - first) + mon.read(ring_addr, tail - (size - first))
    else:
        text = bytes(ring[(start + i) % size] for i in range(tail))
    names = ('swaps', 'interrupts', 'files', 'bytes_read', 'threads', 'unimplemented', 'gpu_packets', 'draws')
    print(' '.join(f'{n}={v}' for n, v in zip(names, stats)))
    print(text.decode(errors='replace'))


if __name__ == '__main__':
    main()

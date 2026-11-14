"""Test only: reads and writes the APU's registers on the running game
(make APU_PROBE=y, src/apu.cpp gw_apu_probe_poll) through xbdm.
Offsets are BAR0 offsets (0xFE800000 + offset), in hex.
  python xbox/tools/xbdm_apu.py <ip> read <off> [count]
  python xbox/tools/xbdm_apu.py <ip> write <off> <value>
  python xbox/tools/xbdm_apu.py <ip> sleep <ms>
  python xbox/tools/xbdm_apu.py <ip> frames [ms]      GP frames and sample counter over ms
  python xbox/tools/xbdm_apu.py <ip> init             the whole bring-up again
  python xbox/tools/xbdm_apu.py <ip> pci              its PCI configuration
  python xbox/tools/xbdm_apu.py <ip> pciwrite <off> <value>
Several commands may be chained with ';' in one argument list.
"""
import pathlib
import struct
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import xbdm_log  # noqa: E402
import xbdm_shot  # noqa: E402

OPS = {'read': 1, 'write': 2, 'sleep': 3, 'frames': 4, 'init': 5, 'pci': 6, 'pciwrite': 7}


class Probe:
    def __init__(self, host):
        text = (pathlib.Path(__file__).resolve().parents[1] / 'build' / 'gw2.map').read_text(errors='replace')
        self.at = xbdm_log.symbol(text, 'gw_apu_probe')
        self.out = xbdm_log.symbol(text, 'gw_apu_probe_out')
        self.x = xbdm_shot.Xbdm(host)
        self.seq = struct.unpack('<6I', self.x.getmem(self.at, 24))[5]

    def setmem(self, address, data):
        self.x.sock.sendall(f'setmem addr=0x{address:08X} data={data.hex()}\r\n'.encode())
        status = self.x.line()
        if not status.startswith('200'):
            raise SystemExit(f'xbdm setmem: {status}')

    def run(self, op, off=0, value=0, count=1):
        self.seq = (self.seq + 1) & 0xFFFFFFFF or 1
        # the command words, then the sequence number (written last: setmem goes byte by byte, in order)
        self.setmem(self.at, struct.pack('<4I', op, off, value, count))
        self.setmem(self.at + 16, struct.pack('<I', self.seq))
        t = time.time()
        while struct.unpack('<I', self.x.getmem(self.at + 20, 4))[0] != self.seq:
            if time.time() - t > 5:
                raise SystemExit('the game did not take the command (no sound pass in 5 s)')
            time.sleep(0.01)
        return struct.unpack(f'<{max(count, 4)}I', self.x.getmem(self.out, 4 * max(count, 4)))


def main():
    host, args = sys.argv[1], sys.argv[2:]
    p = Probe(host)
    cmds, cur = [], []
    for a in args:
        if a == ';':
            cmds.append(cur)
            cur = []
        else:
            cur.append(a)
    if cur:
        cmds.append(cur)
    for c in cmds:
        name, rest = c[0], [int(v, 16) for v in c[1:]]
        if name == 'read':
            off, n = rest[0], rest[1] if len(rest) > 1 else 1
            r = p.run(1, off, 0, n)
            for i in range(0, n, 4):
                print(f'{off + 4 * i:05X}: ' + ' '.join(f'{w:08X}' for w in r[i:min(i + 4, n)]))
        elif name == 'write':
            r = p.run(2, rest[0], rest[1])
            print(f'{rest[0]:05X} <- {rest[1]:08X}, reads {r[0]:08X}')
        elif name == 'sleep':
            p.run(3, 0, int(c[1]))
        elif name == 'frames':
            ms = int(c[1]) if len(c) > 1 else 50
            r = p.run(4, 0, ms)
            print(f'GP frames {r[0]} -> {r[2]} (+{(r[2] - r[0]) & 0xFFFFFF}) in {ms} ms; samples +{(r[3] - r[1]) & 0xFFFFFFFF}')
        elif name == 'pci':
            r = p.run(6, 0, rest[0] if rest else 0, 64)  # (optional: another PCI slot)
            for i in range(0, 64, 4):
                print(f'pci {4 * i:02X}: ' + ' '.join(f'{w:08X}' for w in r[i:i + 4]))
        elif name == 'pciwrite':
            r = p.run(7, rest[0], rest[1], rest[2] if len(rest) > 2 else 1)  # (optional third: another PCI slot)
            print(f'pci {rest[0]:02X} <- {rest[1]:08X}, reads {r[0]:08X}')
        elif name == 'init':
            r = p.run(5)
            print(f'bring-up: ready {r[0]}, GP frames {r[1]}')
        else:
            raise SystemExit(f'unknown command {name}')


if __name__ == '__main__':
    main()

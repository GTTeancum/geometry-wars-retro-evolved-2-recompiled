"""Reads the NV2A display controller's (CRTC) registers and its scan-out
start on a development Xbox through xbdm, to compare a working display
with a broken one.

  python xbox/tools/xbdm_crtc.py <console address>

The CRTC registers are behind an index/data port pair (0xFD6013D4 /
0xFD6013D5): each read sets the index (xbdm 'setmem') and reads the data.
The extended registers need CR1F unlocked (0x57); the lock and the index
are put back afterwards. This races with anything on the console using the
same ports, so run it while the game is idle in a menu if possible.
"""
import struct
import sys
import pathlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import xbdm_shot  # noqa: E402

INDEX, DATA = 0xFD6013D4, 0xFD6013D5


def setmem(x, address, data):
    x.sock.sendall(f'setmem addr=0x{address:08X} data={data.hex()}\r\n'.encode())
    status = x.line()
    if not status.startswith('200'):
        raise SystemExit(f'xbdm setmem: {status}')


def main():
    x = xbdm_shot.Xbdm(sys.argv[1])
    old_index = x.getmem(INDEX, 1)
    setmem(x, INDEX, b'\x1f')
    lock = x.getmem(DATA, 1)
    setmem(x, DATA, b'\x57')
    values = []
    for r in range(0x60):
        setmem(x, INDEX, bytes([r]))
        values.append(x.getmem(DATA, 1)[0])
    setmem(x, INDEX, b'\x1f')
    setmem(x, DATA, lock)
    setmem(x, INDEX, old_index)
    for row in range(0, 0x60, 16):
        print(f'CR{row:02X}: ' + ' '.join(f'{v:02X}' for v in values[row:row + 16]))
    start = struct.unpack('<I', x.getmem(0xFD600800, 4))[0]
    print(f'PCRTC_START {start:08X}')
    for name, address in (('PRAMDAC general control', 0xFD680600), ('PRAMDAC FP horizontal', 0xFD680820),
                          ('PCRTC config', 0xFD600804), ('PCRTC interrupt enable', 0xFD600140)):
        print(f'{name}: {struct.unpack("<I", x.getmem(address, 4))[0]:08X}')


if __name__ == '__main__':
    main()

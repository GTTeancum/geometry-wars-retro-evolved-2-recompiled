"""Watch a development Xbox's debug monitor (xbdm, port 731) for exceptions,
breaks and debug output while the game runs, optionally starting the game.

  python xbox/tools/xbdm_watch.py <console address> [seconds] [--launch xe:\\path\\default.xbe]

Each exception or break is printed with the thread's registers ('getcontext')
and the function from the link map (xbox/build/gw2.map) that holds its
address, so a crash on hardware points at a function name.
"""
import bisect
import pathlib
import re
import socket
import sys
import time

MAP = pathlib.Path(__file__).resolve().parents[1] / 'build' / 'gw2.map'


def load_map():
    names = []
    if MAP.exists():
        for m in re.finditer(r'^\s*\d{4}:[0-9a-fA-F]+\s+(\S+)\s+([0-9a-fA-F]{8,16})\s', MAP.read_text(errors='replace'),
                             re.M):
            names.append((int(m[2], 16), m[1]))
    names.sort()
    return names


def where(names, address):
    i = bisect.bisect_right([a for a, _ in names], address) - 1
    if i < 0:
        return '?'
    return f'{names[i][1]}+{address - names[i][0]:X}'


class Conn:
    def __init__(self, host):
        self.sock = socket.create_connection((host, 731), timeout=10)
        self.buf = b''
        self.line()

    def line(self):
        while b'\r\n' not in self.buf:
            part = self.sock.recv(4096)
            if not part:
                raise EOFError
            self.buf += part
        out, self.buf = self.buf.split(b'\r\n', 1)
        return out.decode(errors='replace')

    def cmd(self, text):
        self.sock.sendall(text.encode() + b'\r\n')
        first = self.line()
        lines = [first]
        if first.startswith('202'):
            while True:
                part = self.line()
                if part == '.':
                    break
                lines.append(part)
        return lines


def main():
    host = sys.argv[1]
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith('--') else 60
    launch = sys.argv[sys.argv.index('--launch') + 1] if '--launch' in sys.argv else None
    names = load_map()
    control = Conn(host)
    print(control.cmd('debugger connect'))
    notify = Conn(host)
    print(notify.cmd('notify'))
    notify.sock.settimeout(1)
    if launch:
        path = launch.rsplit('\\', 1)
        print(control.cmd(f'magicboot title="{launch}" debug'))
        time.sleep(3)
        control = None
        for _ in range(60):
            try:
                control = Conn(host)
                print(control.cmd('debugger connect'))
                notify = Conn(host)
                print(notify.cmd('notify'))
                notify.sock.settimeout(1)
                break
            except OSError:
                time.sleep(1)
        if control is None:
            raise SystemExit('console did not come back')
    end = time.time() + seconds
    while time.time() < end:
        try:
            text = notify.line()
        except socket.timeout:
            continue
        except EOFError:
            print('notification channel closed')
            break
        print(text)
        if text.startswith(('exception', 'break', 'data', 'singlestep')) or 'stop' in text.split(' ')[0]:
            m = re.search(r'thread=(0x[0-9a-fA-F]+|\d+)', text)
            a = re.search(r'address=(0x[0-9a-fA-F]+)', text)
            if a:
                print('  at', where(names, int(a[1], 16)))
            if m:
                ctx = control.cmd(f'getcontext thread={m[1]} control int')
                print('  ', ' '.join(ctx[1:]))
                eip = re.search(r'Eip=(0x[0-9a-fA-F]+)', ' '.join(ctx))
                if eip:
                    print('  eip in', where(names, int(eip[1], 16)))
                esp = re.search(r'Esp=(0x[0-9a-fA-F]+)', ' '.join(ctx))
                if esp:
                    sp = int(esp[1], 16)
                    mem = control.cmd(f'getmem addr=0x{sp:08X} length=0x200')
                    data = bytes.fromhex(''.join(l for l in mem[1:] if all(c in '0123456789abcdefABCDEF?' for c in l)).replace('?', '0'))
                    for k in range(0, len(data) - 3, 4):
                        v = int.from_bytes(data[k:k + 4], 'little')
                        w = where(names, v)
                        if names and names[0][0] <= v < names[-1][0] + 0x10000 and '+' in w:
                            print(f'  stack+{k:03X}: {v:08X} {w}')


if __name__ == '__main__':
    main()

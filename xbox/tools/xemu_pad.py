"""Press buttons in the game, in XEMU or on a development Xbox, through its
test-only remote pad.

The pad record (gw_remote_pad in src/input.cpp) is written through XEMU's GDB
stub (boot_xemu.ps1 -GdbPort, default 9258); frames are counted from gw_stats
through the monitor. Never sends host input. Adapted from the Jedi Power
Battles project's xemu_pad.py.

  python xbox/tools/xemu_pad.py "press A" "wait 60" "stick 0 1 30" "shot menu"
  python xbox/tools/xemu_pad.py --xbdm 10.0.0.231 "press A" "shot menu"

With --xbdm the console's debug monitor (port 731) reads and writes the
game's memory instead (the XBE loads at its fixed base, so the link map's
addresses hold), and screenshots come from tools/xbdm_shot.py.

Steps:
  press A[,B...] [frames]   hold the buttons for frames (default 6), then release
  hold A[,B...]             hold until the next release/press
  release                   release everything and centre the sticks
  stick X Y [frames]        left stick, -1..1 (Y positive = up); frames > 0 recentres
  rstick X Y [frames]       right stick (aim)
  trig L R [frames]         triggers 0..1
  wait N                    wait for N swaps
  sleep S                   wall-clock seconds
  shot NAME                 screenshot to xbox/test-artifacts/pad/NAME.png
  off                       disable the remote pad
  player N                  the steps after drive player N's pad (1-4; default 1)

--file FILE adds steps from a file, one a line (a long run). Each step's line
starts with the wall-clock time, so the frame counts give the game's speed.
"""
import argparse
import pathlib
import re
import socket
import subprocess
import sys
import time

MAGIC = 0x44415047
BUTTONS = dict(UP=0x0001, DOWN=0x0002, LEFT=0x0004, RIGHT=0x0008, START=0x0010, BACK=0x0020,
               LS=0x0040, RS=0x0080, LB=0x0100, RB=0x0200, A=0x1000, B=0x2000, X=0x4000, Y=0x8000)
XBOX = pathlib.Path(__file__).resolve().parents[1]


def symbol(map_text, name):
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

    def command(self, text):
        self.sock.sendall(text.encode() + b'\n')
        return self.response()

    def word(self, address):
        for line in self.command(f'x /1wx 0x{address:x}').splitlines():
            m = re.match(r'^\s*[\da-fA-F]+:\s+0x([\da-fA-F]+)', line)
            if m:
                return int(m[1], 16)
        return None


class Gdb:
    """Minimal GDB remote client: interrupt, write memory, continue."""

    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=10)
        self.buffer = b''
        self.packet('?')
        self.running = False
        self.resume()

    def recv_packet(self):
        while True:
            start = self.buffer.find(b'$')
            if start >= 0:
                end = self.buffer.find(b'#', start)
                if end >= 0 and len(self.buffer) >= end + 3:
                    body = self.buffer[start + 1:end]
                    self.buffer = self.buffer[end + 3:]
                    self.sock.sendall(b'+')
                    return body.decode(errors='replace')
            part = self.sock.recv(65536)
            if not part:
                raise SystemExit('GDB stub closed')
            self.buffer += part

    def send(self, body):
        self.sock.sendall(f'${body}#{sum(body.encode()) & 0xff:02x}'.encode())

    def packet(self, body):
        self.send(body)
        return self.recv_packet()

    def stop(self):
        if self.running:
            self.sock.sendall(b'\x03')
            self.recv_packet()
            self.running = False

    def resume(self):
        if not self.running:
            self.send('c')
            self.running = True

    def write(self, address, data):
        self.stop()
        try:
            reply = self.packet(f'M{address:x},{len(data):x}:{data.hex()}')
            # (a stop reply left over from the interrupt can come first)
            while reply[:1] in ('T', 'S') and reply != 'OK':
                reply = self.recv_packet()
            if reply != 'OK':
                raise SystemExit(f'GDB write failed: {reply}')
        finally:
            self.resume()

    def close(self):
        try:
            self.stop()
            self.send('D')
            self.sock.close()
        except OSError:
            pass


class XbdmTarget:
    """A development Xbox's debug monitor, standing in for both the XEMU
    monitor (word) and the GDB stub (write)."""

    def __init__(self, host):
        sys.path.insert(0, str(XBOX / 'tools'))
        import xbdm_shot
        self.host = host
        self.xbdm = xbdm_shot.Xbdm(host)
        xbdm_shot.check_build(self.xbdm)  # the pad's address is from the link map

    def word(self, address):
        return int.from_bytes(self.xbdm.getmem(address, 4), 'little')

    def write(self, address, data):
        self.xbdm.sock.sendall(f'setmem addr=0x{address:08X} data={data.hex()}\r\n'.encode())
        status = self.xbdm.line()
        if not status.startswith('200'):
            raise SystemExit(f'xbdm setmem: {status}')

    def close(self):
        self.xbdm.sock.close()


class Session:
    def __init__(self, args):
        map_text = (XBOX / 'build/gw2.map').read_text()
        self.pad = symbol(map_text, 'gw_remote_pad')
        self.stats = symbol(map_text, 'gw_stats')
        self.xbdm_host = args.xbdm
        if args.xbdm:
            self.monitor = self.gdb = XbdmTarget(args.xbdm)
        else:
            self.monitor = Monitor(args.port)
            self.gdb = Gdb(args.gdb_port)
        self.out = XBOX / 'test-artifacts/pad'
        self.buttons, self.left, self.right, self.triggers = 0, (0, 0), (0, 0), (0, 0)
        self.player = 0  # 0-3: gw_remote_pad[player], 20 bytes each
        self.states = [(0, (0, 0), (0, 0), (0, 0)) for _ in range(4)]

    def select(self, player):
        self.states[self.player] = (self.buttons, self.left, self.right, self.triggers)
        self.player = player
        self.buttons, self.left, self.right, self.triggers = self.states[player]

    def frame(self):
        return self.monitor.word(self.stats)

    def publish(self, magic=MAGIC):
        def stick(v):
            x, y = (int(max(-1, min(1, c)) * 32767) & 0xffff for c in v)
            return x | (y << 16)
        lt, rt = (int(max(0, min(1, v)) * 255) for v in self.triggers)
        record = [magic, self.buttons, stick(self.left), stick(self.right), lt | (rt << 16)]
        self.gdb.write(self.pad + 20 * self.player, b''.join(v.to_bytes(4, 'little') for v in record))

    def wait(self, frames):
        start = self.frame()
        deadline = time.monotonic() + max(10, frames / 2)
        while (self.frame() - start) & 0xffffffff < frames:
            if time.monotonic() > deadline:
                raise SystemExit(f'guest did not advance {frames} frames')
            time.sleep(0.05)

    def shot(self, name):
        self.out.mkdir(parents=True, exist_ok=True)
        path = self.out / f'{name}.png'
        if self.xbdm_host:
            subprocess.run([sys.executable, str(XBOX / 'tools/xbdm_shot.py'), self.xbdm_host, str(path)], check=True,
                           capture_output=True)
            return path
        subprocess.run(['powershell', '-ExecutionPolicy', 'Bypass', '-File', str(XBOX / 'tools/xemu_shot.ps1'),
                        '-Out', str(path)], check=True, capture_output=True)
        return path

    def step(self, text):
        words = text.split()
        op = words[0].lower()
        if op == 'press':
            self.buttons = sum(BUTTONS[b] for b in words[1].upper().split(','))
            self.publish()
            self.wait(int(words[2]) if len(words) > 2 else 6)
            self.buttons = 0
            self.publish()
            self.wait(3)
        elif op == 'hold':
            self.buttons = sum(BUTTONS[b] for b in words[1].upper().split(','))
            self.publish()
        elif op == 'release':
            self.buttons, self.left, self.right, self.triggers = 0, (0, 0), (0, 0), (0, 0)
            self.publish()
        elif op in ('stick', 'rstick', 'trig'):
            value = (float(words[1]), float(words[2]))
            attr = dict(stick='left', rstick='right', trig='triggers')[op]
            setattr(self, attr, value)
            self.publish()
            if len(words) > 3:
                self.wait(int(words[3]))
                setattr(self, attr, (0, 0))
                self.publish()
        elif op == 'wait':
            self.wait(int(words[1]))
        elif op == 'sleep':
            time.sleep(float(words[1]))
        elif op == 'shot':
            return str(self.shot(words[1]))
        elif op == 'player':
            n = int(words[1])
            if not 1 <= n <= 4:
                raise SystemExit('player 1-4')
            self.select(n - 1)
        elif op == 'off':
            self.buttons, self.left, self.right, self.triggers = 0, (0, 0), (0, 0), (0, 0)
            self.publish(0)
        else:
            raise SystemExit(f'unknown step: {text}')
        return None


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--port', type=int, default=9257)
    p.add_argument('--gdb-port', type=int, default=9258)
    p.add_argument('--xbdm', help='a development Xbox address (its debug monitor) instead of XEMU')
    p.add_argument('--file', help='more steps, one a line, after those given')
    p.add_argument('steps', nargs='*')
    args = p.parse_args()
    if args.file:
        args.steps += [line.strip() for line in open(args.file) if line.strip()]
    session = Session(args)
    try:
        for step in args.steps:
            result = session.step(step)
            print(f'{time.time():.2f} {step}: frame {session.frame()}' + (f' -> {result}' if result else ''), flush=True)
    finally:
        session.gdb.close()


if __name__ == '__main__':
    main()

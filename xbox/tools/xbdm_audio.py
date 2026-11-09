"""Reads the last 8 s of sound a capture build sent out (make AUDIO_CAPTURE=y,
src/audio.cpp; with APU_TEST=y the APU's output) into a WAV file: freezes
the recording, reads it in order, and lets it run again.

  python xbox/tools/xbdm_audio.py <console ip> <out.wav>
  python xbox/tools/xbdm_audio.py --xemu [gdb port, default 9258] <out.wav>

--xemu reads through XEMU's GDB stub (tools/boot_xemu.ps1 -GdbPort).
"""
import pathlib
import re
import socket
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import xbdm_shot  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[1]


class Gdb:
    """Minimal GDB remote client: memory read and write (the CPU stopped
    meanwhile)."""

    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=20)
        self.buffer = b''
        self.packet('?')
        self.send('c')
        self.running = True

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
            part = self.sock.recv(1 << 20)
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

    def getmem(self, address, length):
        self.stop()
        try:
            out = bytearray()
            while len(out) < length:
                n = min(0x800, length - len(out))
                reply = self.packet(f'm{address + len(out):x},{n:x}')
                # (a stop reply left over from the interrupt comes first)
                while reply[:1] in ('T', 'S', 'O') and not re.fullmatch(r'[0-9a-fA-F]*', reply):
                    reply = self.recv_packet()
                if reply.startswith('E') and len(reply) == 3:
                    raise SystemExit(f'GDB read failed at {address + len(out):08X}: {reply}')
                out += bytes.fromhex(reply)
            return bytes(out)
        finally:
            self.resume()

    def setmem(self, address, data):
        self.stop()
        try:
            reply = self.packet(f'M{address:x},{len(data):x}:{data.hex()}')
            if reply != 'OK':
                raise SystemExit(f'GDB write failed: {reply}')
        finally:
            self.resume()


class Xbdm:
    def __init__(self, host):
        self.x = xbdm_shot.Xbdm(host)
        xbdm_shot.check_build(self.x)

    def getmem(self, address, length):
        return self.x.getmem(address, length)

    def setmem(self, address, data):
        self.x.sock.sendall(f'setmem addr=0x{address:08X} data={data.hex()}\r\n'.encode())
        self.x.line()


def main():
    args = sys.argv[1:]
    if args and args[0] == '--xemu':
        port = int(args[1]) if len(args) == 3 else 9258
        target, out = Gdb(port), args[-1]
    elif len(args) == 2:
        target, out = Xbdm(args[0]), args[1]
    else:
        sys.exit(__doc__)
    m = (ROOT / 'build' / 'gw2.map').read_text()

    def sym(n):
        return int(re.search(r'\s+\d{4}:[\da-fA-F]+\s+_' + n + r'\s+([\da-fA-F]+)', m)[1], 16)

    state, ring = sym('gw_audio_capture_state'), sym('gw_audio_capture_ring')
    target.setmem(state + 8, struct.pack('<I', 1))
    magic, written, _, frames = struct.unpack('<4I', target.getmem(state, 16))
    if magic != 0x50414341:
        sys.exit('not a capture build (make AUDIO_CAPTURE=y)')
    count = min(written, frames)
    data = target.getmem(ring, frames * 1024 if written >= frames else count * 1024)
    target.setmem(state + 8, struct.pack('<I', 0))
    first = written % frames if written >= frames else 0
    pcm = b''.join(bytes(data[((first + i) % frames) * 1024:((first + i) % frames + 1) * 1024]) for i in range(count))
    header = b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 2, 48000,
                                                                                    192000, 4, 16)
    pathlib.Path(out).write_bytes(header + b'data' + struct.pack('<I', len(pcm)) + pcm)
    print(f'{out}: {count / 187.5:.1f} s (frames {written - count}..{written})')


if __name__ == '__main__':
    main()

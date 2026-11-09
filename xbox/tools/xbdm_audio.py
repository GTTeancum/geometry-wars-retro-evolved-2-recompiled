"""Reads the last 8 s of sound the console sent out (make AUDIO_CAPTURE=y,
src/audio.cpp) into a WAV file: freezes the recording, reads it in order,
and lets it run again.

  python xbox/tools/xbdm_audio.py <console ip> <out.wav>
"""
import pathlib
import re
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import xbdm_shot  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    host, out = sys.argv[1], sys.argv[2]
    x = xbdm_shot.Xbdm(host)
    xbdm_shot.check_build(x)
    m = (ROOT / 'build' / 'gw2.map').read_text()

    def sym(n):
        return int(re.search(r'\s+\d{4}:[\da-fA-F]+\s+_' + n + r'\s+([\da-fA-F]+)', m)[1], 16)

    state, ring = sym('gw_audio_capture_state'), sym('gw_audio_capture_ring')

    def setmem(address, data):
        x.sock.sendall(f'setmem addr=0x{address:08X} data={data.hex()}\r\n'.encode())
        x.line()

    setmem(state + 8, struct.pack('<I', 1))
    magic, written, _, frames = struct.unpack('<4I', x.getmem(state, 16))
    if magic != 0x50414341:
        sys.exit('not a capture build (make AUDIO_CAPTURE=y)')
    data = bytearray()
    for start in range(0, frames * 1024, 0x10000):
        data += x.getmem(ring + start, min(0x10000, frames * 1024 - start))
    setmem(state + 8, struct.pack('<I', 0))
    count = min(written, frames)
    first = written % frames if written >= frames else 0
    pcm = b''.join(bytes(data[((first + i) % frames) * 1024:((first + i) % frames + 1) * 1024]) for i in range(count))
    header = b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 2, 48000,
                                                                                    192000, 4, 16)
    pathlib.Path(out).write_bytes(header + b'data' + struct.pack('<I', len(pcm)) + pcm)
    print(f'{out}: {count / 187.5:.1f} s (frames {written - count}..{written})')


if __name__ == '__main__':
    main()

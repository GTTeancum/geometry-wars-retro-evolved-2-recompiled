"""Decodes the waves of the game's sound banks (audio\\*.baf) to WAV files
with ffmpeg, as a first step towards sound on the Xbox (see TODO.md).

A bank: 'BANK' header (0x34 bytes; wave count at +12), then one 0x5C-byte
'WAVE' record per sample (name at +12, then big-endian words: data offset
+0x2C, size +0x30, sample rate +0x3C, bits +0x40, channels +0x44). The data
is XMA in 2 KB packets; wrapped in an XMA2 RIFF header, ffmpeg decodes it.

  python xbox/tools/bank_to_wav.py <bank.baf> [more banks...] --out <folder>
"""
import argparse
import pathlib
import shutil
import struct
import subprocess
import sys

PACKET = 2048


def waves(bank):
    data = bank.read_bytes()
    if data[:4] != b'BANK':
        sys.exit(f'{bank} is not a sound bank')
    count = struct.unpack('>I', data[12:16])[0]
    for i in range(count):
        rec = data[0x34 + i * 0x5C:0x34 + (i + 1) * 0x5C]
        if rec[:4] != b'WAVE':
            break
        name = rec[12:44].split(b'\0')[0].decode(errors='replace') or f'wave{i}'
        offset, size = struct.unpack('>II', rec[0x2C:0x34])
        rate, bits, channels = struct.unpack('>III', rec[0x3C:0x48])
        yield i, name, data[offset:offset + size], rate, channels


def xma2_riff(payload, rate, channels):
    fmt = struct.pack('<HHIIHHH', 0x166, channels, rate, rate * 2 * channels, PACKET, 16, 34)
    fmt += struct.pack('<HIIIIIIIBBH', 1, 4 if channels == 1 else 3, 0, len(payload), 0, 0, 0, 0, 0, 4,
                       len(payload) // PACKET)
    body = b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt)) + fmt + b'data' + struct.pack('<I', len(payload)) + payload
    return b'RIFF' + struct.pack('<I', len(body)) + body


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('banks', nargs='+', type=pathlib.Path)
    p.add_argument('--out', type=pathlib.Path, required=True)
    args = p.parse_args()
    ffmpeg = shutil.which('ffmpeg')
    if not ffmpeg:
        sys.exit('ffmpeg is not on PATH')
    failed = total_seconds = 0
    for bank in args.banks:
        folder = args.out / bank.stem
        folder.mkdir(parents=True, exist_ok=True)
        for i, name, payload, rate, channels in waves(bank):
            xma = folder / f'{i:03d}_{name}.xma.wav'
            wav = folder / f'{i:03d}_{name}.wav'
            xma.write_bytes(xma2_riff(payload, rate, channels))
            r = subprocess.run([ffmpeg, '-hide_banner', '-loglevel', 'error', '-y', '-i', str(xma), '-acodec',
                                'pcm_s16le', str(wav)], capture_output=True, text=True)
            xma.unlink()
            if r.returncode or not wav.exists():
                failed += 1
                print(f'{bank.name} #{i} {name}: {r.stderr.strip()[:200]}')
                continue
            total_seconds += (wav.stat().st_size - 44) / (2 * channels * rate)
        print(f'{bank.name}: done')
    print(f'{failed} failed; {total_seconds:.0f} s of audio decoded')


if __name__ == '__main__':
    main()

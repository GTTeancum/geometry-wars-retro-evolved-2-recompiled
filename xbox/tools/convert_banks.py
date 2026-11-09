"""Converts the game's sound banks (audio\\*.baf) from XMA to IMA ADPCM for
the Xbox, which has no XMA (development; the installer has its own,
installer/SoundConvert.cs). Needs ffmpeg. tools/xadpcm_banks.cmd then turns
the result into the Xbox ADPCM banks the game plays.

A bank: 'BANK' header (0x34 bytes; wave count at +12), then one 'WAVE'
record per sample, each with its own size at +4 (0x5C, or 0x50 without loop
data): data type +8 (8 XMA, 3 16-bit big-endian PCM), name at +12, then
big-endian words data offset +0x2C, size +0x30, volume +0x34, format +0x38,
sample rate +0x3C, bits +0x40, channels +0x44.

Each wave's XMA, wrapped as XMA2 RIFF, goes through ffmpeg once into 4-bit
IMA ADPCM (Microsoft layout, trellis search; about the size of the XMA). The
game's banks stay as they are (the game reads them); next to each,
<bank>.adpcm holds the converted waves, found by their place in the original
bank (all little-endian words):

  'GWAD', wave count, then per wave: original data offset, original size,
  sample rate, channels, samples, ADPCM block size, ADPCM offset, ADPCM size;
  then the ADPCM data.

The Xbox runtime gives the game's XMA decoding the samples of the wave whose
original data it was pointed at; XMA is never decoded on the Xbox.

  python xbox/tools/convert_banks.py <audio folder> <output folder>
"""
import argparse
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

PACKET = 2048


def records(data):
    """(index, name, offset, size, rate, channels, data type) for each WAVE record."""
    count = struct.unpack('>I', data[12:16])[0]
    at = 0x34
    for i in range(count):
        if data[at:at + 4] != b'WAVE':
            raise RuntimeError(f'record {i} at {at:#x} is not a WAVE')
        size = struct.unpack('>I', data[at + 4:at + 8])[0]
        name = data[at + 12:at + 44].split(b'\0')[0].decode(errors='replace')
        offset, length = struct.unpack('>II', data[at + 0x2C:at + 0x34])
        rate, _bits, channels = struct.unpack('>III', data[at + 0x3C:at + 0x48])
        kind = struct.unpack('>I', data[at + 8:at + 12])[0]
        yield i, name, offset, length, rate, channels, kind
        at += size


def xma2_riff(payload, rate, channels):
    fmt = struct.pack('<HHIIHHH', 0x166, channels, rate, rate * 2 * channels, PACKET, 16, 34)
    fmt += struct.pack('<HIIIIIIIBBH', 1, 4 if channels == 1 else 3, 0, len(payload), 0, 0, 0, 0, 0, 4,
                       max(1, len(payload) // PACKET))
    body = b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt)) + fmt + b'data' + struct.pack('<I', len(payload)) + payload
    return b'RIFF' + struct.pack('<I', len(body)) + body


def chunks(riff):
    at = 12
    while at + 8 <= len(riff):
        tag, size = riff[at:at + 4], struct.unpack('<I', riff[at + 4:at + 8])[0]
        yield tag, riff[at + 8:at + 8 + size]
        at += 8 + size + (size & 1)


def to_adpcm(ffmpeg, payload, rate, channels, work, pcm=False):
    """-> (ADPCM data, block size, samples). pcm: the payload is 16-bit
    big-endian PCM (data type 3), not XMA."""
    dst = work / 'out.wav'
    if pcm:
        src = work / 'in.raw'
        src.write_bytes(payload)
        source = ['-f', 's16be', '-ar', str(rate), '-ac', str(channels), '-i', str(src)]
    else:
        src = work / 'in.wav'
        src.write_bytes(xma2_riff(payload, rate, channels))
        source = ['-i', str(src)]
    r = subprocess.run([ffmpeg, '-hide_banner', '-loglevel', 'error', '-y'] + source +
                       ['-acodec', 'adpcm_ima_wav', '-trellis', '8', str(dst)], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr.strip()[:300])
    block = samples = None
    data = b''
    for tag, body in chunks(dst.read_bytes()):
        if tag == b'fmt ':
            block = struct.unpack('<H', body[12:14])[0]
        elif tag == b'fact':
            samples = struct.unpack('<I', body[:4])[0]
        elif tag == b'data':
            data = body
    if block is None or samples is None:
        raise RuntimeError('ffmpeg wrote no ADPCM')
    return data, block, samples


def convert_bank(ffmpeg, path, out_path, work):
    data = path.read_bytes()
    if data[:4] != b'BANK':
        raise RuntimeError(f'{path} is not a sound bank')
    entries, blob, seconds = [], bytearray(), 0.0
    for i, name, offset, length, rate, channels, kind in records(data):
        try:
            adpcm, block, samples = to_adpcm(ffmpeg, data[offset:offset + length], rate, channels, work,
                                             pcm=kind == 3)
        except RuntimeError as e:
            raise RuntimeError(f'{path.name} #{i} {name}: {e}') from None
        entries.append((offset, length, rate, channels, samples, block, len(blob), len(adpcm)))
        blob += adpcm
        while len(blob) % 4:
            blob.append(0)
        seconds += samples / rate
    header = b'GWAD' + struct.pack('<I', len(entries))
    table_size = len(header) + 32 * len(entries)
    table = b''.join(struct.pack('<8I', o, n, r, c, s, b, table_size + a, z) for o, n, r, c, s, b, a, z in entries)
    out_path.write_bytes(header + table + bytes(blob))
    return len(entries), seconds, table_size + len(blob)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('audio', type=pathlib.Path)
    p.add_argument('out', type=pathlib.Path)
    args = p.parse_args()
    ffmpeg = shutil.which('ffmpeg')
    if not ffmpeg:
        sys.exit("ffmpeg is needed once to read the game's sounds (https://ffmpeg.org); put it on PATH.")
    args.out.mkdir(parents=True, exist_ok=True)
    waves = total_seconds = total_bytes = 0
    with tempfile.TemporaryDirectory() as tmp:
        for bank in sorted(args.audio.glob('*.baf')):
            n, seconds, size = convert_bank(ffmpeg, bank, args.out / (bank.stem + '.adpcm'), pathlib.Path(tmp))
            waves, total_seconds, total_bytes = waves + n, total_seconds + seconds, total_bytes + size
            print(f'{bank.name}: {n} waves, {bank.stat().st_size // 1024} KB XMA -> {size // 1024} KB ADPCM')
    print(f'{waves} waves, {total_seconds:.0f} s of sound, {total_bytes / 1048576:.1f} MB of ADPCM')


if __name__ == '__main__':
    main()

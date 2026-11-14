"""Test only: dumps APU voice records of the running game (src/apu.cpp's
table, found through the deployed link map) through xbdm.
  python xbox/tools/xbdm_apu_voice.py <ip> <voice> [voice ...]
"""
import pathlib
import re
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import xbdm_shot  # noqa: E402

build = pathlib.Path(__file__).resolve().parents[1] / 'build'
text = (build / ('deployed.map' if (build / 'deployed.map').exists() else 'gw2.map')).read_text(errors='replace')
m = re.search(r'\?g_voices@\S+\s+([0-9a-fA-F]{8,16})\s', text)
x = xbdm_shot.Xbdm(sys.argv[1])
table = struct.unpack('<I', x.getmem(int(m[1], 16), 4))[0]
for h in map(int, sys.argv[2:]):
    w = struct.unpack('<32I', x.getmem(table + h * 0x80, 0x80))
    print(f'voice {h} at {table + h * 0x80:08X}')
    for i in range(0, 32, 8):
        print(f'  {4 * i:02X}: ' + ' '.join(f'{v:08X}' for v in w[i:i + 8]))

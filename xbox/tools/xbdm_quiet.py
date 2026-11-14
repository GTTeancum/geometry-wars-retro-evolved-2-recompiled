"""Test only: asks the game running on the console to leave the APU quiet
(gw_apu_quiet_request, src/sound_native.cpp) before it is restarted or
replaced. The running build is the one last deployed (tools/xbdm_deploy.sh
keeps its link map and image as build/deployed.map and build/deployed.exe):
its stamp is read from the console first, and nothing is written unless it
matches (another program, or another build, keeps something else there).
  python xbox/tools/xbdm_quiet.py <console ip>
Exit code 0: asked; 1: not the deployed build (nothing written).
"""
import pathlib
import re
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import xbdm_shot  # noqa: E402

build = pathlib.Path(__file__).resolve().parents[1] / 'build'
if not (build / 'deployed.map').exists() or not (build / 'deployed.exe').exists():
    raise SystemExit(1)
text = (build / 'deployed.map').read_text(errors='replace')


def symbol(name):
    m = re.search(r'\s_' + name + r'\s+([0-9a-fA-F]{8,16})\s', text)
    return int(m[1], 16) if m else None


stamp_va, quiet_va = symbol('gw_build_stamp'), symbol('gw_apu_quiet_request')
if stamp_va is None or quiet_va is None:
    raise SystemExit(1)  # (a build that mixes in software has no such switch)
image = (build / 'deployed.exe').read_bytes()
pe = struct.unpack_from('<I', image, 0x3C)[0]
sections, optional_size = struct.unpack_from('<H', image, pe + 6)[0], struct.unpack_from('<H', image, pe + 20)[0]
image_base = struct.unpack_from('<I', image, pe + 24 + 28)[0]
rva, built = stamp_va - image_base, None
for s in range(sections):
    at = pe + 24 + optional_size + s * 40
    size, address, raw_size, raw = struct.unpack_from('<IIII', image, at + 8)
    if address <= rva < address + max(size, raw_size):
        built = image[raw + rva - address:raw + rva - address + 8]
        break
x = xbdm_shot.Xbdm(sys.argv[1])
try:
    running = x.getmem(stamp_va, 8)
except SystemExit:
    raise SystemExit(1)
if built is None or running != built:
    print('not the deployed build: nothing written')
    raise SystemExit(1)
x.sock.sendall(f'setmem addr=0x{quiet_va:08X} data=01000000\r\n'.encode())
print(x.line())

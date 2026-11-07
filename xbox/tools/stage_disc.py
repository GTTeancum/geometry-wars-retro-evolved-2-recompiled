"""Stage the Xbox disc in xbox/build/staged-disc.

  default.xbe            xbox/build/release/default.xbe
  default.xex + data     game/extracted (the package's files; the runtime
                         reads the XEX header and the game opens the rest)
  gw2/default.image.bin  game/analysis/default.image.bin (tools/dump_image)

Files are copied only when they differ.
"""
import filecmp
import pathlib
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
STAGE = ROOT / 'xbox' / 'build' / 'staged-disc'
IMAGE_SHA256 = '4168a02a62542d32660e63efde3244bb423d593c288402f177bf5e6f8c33b901'


def copy(src, dst):
    if dst.exists() and filecmp.cmp(src, dst, shallow=False):
        return 0
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dst)
    return 1


def clear_limit_64mb(xbe_path):
    # XBE init flags (header +0x124): bit 2 limits a development kit to 64 MiB.
    data = bytearray(xbe_path.read_bytes())
    flags = int.from_bytes(data[0x124:0x128], 'little')
    data[0x124:0x128] = (flags & ~0x4).to_bytes(4, 'little')
    xbe_path.write_bytes(data)


def main():
    devkit_128 = '--devkit-128mb' in sys.argv[1:]
    extracted = ROOT / 'game' / 'extracted'
    image = ROOT / 'game' / 'analysis' / 'default.image.bin'
    xbe = ROOT / 'xbox' / 'build' / 'release' / 'default.xbe'
    for path, hint in ((extracted / 'default.xex', 'tools/extract_stfs.ps1'),
                       (image, 'xbox/tools/dump_image.cmd'),
                       (xbe, 'xbox/tools/build.sh')):
        if not path.exists():
            sys.exit(f'{path} missing: run {hint}')
    import hashlib
    if hashlib.sha256(image.read_bytes()).hexdigest() != IMAGE_SHA256:
        sys.exit(f'{image} is not the supported image')
    changed = 0
    for src in extracted.rglob('*'):
        if src.is_file():
            changed += copy(src, STAGE / src.relative_to(extracted))
    # The sound banks converted for the Xbox (xbox/tools/convert_banks.py
    # into xbox/build/audio_adpcm), next to the game's banks; the native
    # sound engine (src/sound_native.cpp) plays these.
    for src in sorted((ROOT / 'xbox' / 'build' / 'audio_adpcm').glob('*.adpcm')):
        changed += copy(src, STAGE / 'Audio' / src.name)
    changed += copy(image, STAGE / 'gw2' / 'default.image.bin')
    changed += copy(xbe, STAGE / 'default.xbe')
    if devkit_128:
        # Test only: let the staged copy use a 128 MiB development kit's memory.
        clear_limit_64mb(STAGE / 'default.xbe')
    print(f'stage_disc: {changed} file(s) updated in {STAGE}')


if __name__ == '__main__':
    main()

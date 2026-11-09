"""Stage the Xbox disc in xbox/build/staged-disc.

  default.xbe            xbox/build/release/default.xbe, with the dashboard's
                         title name and images (tools/xbe_meta.py)
  TitleImage.xbx, SaveImage.xbx, TitleMeta.xbx
                         the dashboard's images and name, from the game's icon
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


def patch_menu_text(loc_path):
    # The main menu's Return to arcade (an Xbox 360 Arcade item) is the
    # in-game reset here (src/menu_native.cpp): Return to dash, written over
    # the same string (UTF-16 big-endian), the rest of its room left as
    # terminators, so no offset in the file moves.
    data = bytearray(loc_path.read_bytes())
    old = 'Return to arcade'.encode('utf-16-be')
    new = 'Return to dash'.encode('utf-16-be')
    at = data.find(old)
    if at < 0:
        if data.find(new + b'\0\0') >= 0:
            return
        sys.exit(f'{loc_path}: Return to arcade not found')
    data[at:at + len(old)] = new + b'\0' * (len(old) - len(new))
    loc_path.write_bytes(bytes(data))


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
    loc = STAGE / 'Text-en.loc'
    for src in extracted.rglob('*'):
        if src.is_file():
            dst = STAGE / src.relative_to(extracted)
            if dst == loc:
                continue
            changed += copy(src, dst)
    # The English text, with the main menu's Return to dash.
    staged_loc = STAGE / 'Text-en.loc.new'
    shutil.copyfile(extracted / 'Text-en.loc', staged_loc)
    patch_menu_text(staged_loc)
    if loc.exists() and filecmp.cmp(staged_loc, loc, shallow=False):
        staged_loc.unlink()
    else:
        staged_loc.replace(loc)
        changed += 1
    # The sound banks converted for the Xbox, next to the game's banks; the
    # native sound engine (src/sound_native.cpp) plays these: Xbox ADPCM
    # (xbox/tools/xadpcm_banks.cmd into xbox/build/audio_xadpcm, from
    # xbox/tools/convert_banks.py's IMA ADPCM in xbox/build/audio_adpcm).
    banks = ROOT / 'xbox' / 'build' / 'audio_xadpcm'
    if not any(banks.glob('*.adpcm')):
        sys.exit(f'{banks} has no banks: run xbox/tools/xadpcm_banks.cmd')
    for src in sorted(banks.glob('*.adpcm')):
        changed += copy(src, STAGE / 'Audio' / src.name)
    changed += copy(image, STAGE / 'gw2' / 'default.image.bin')
    staged_xbe = STAGE / 'default.xbe'
    if not staged_xbe.exists() or staged_xbe.stat().st_mtime < xbe.stat().st_mtime:
        shutil.copyfile(xbe, staged_xbe)
        changed += 1
    # The dashboard's title name and images, from the game's own icon
    # (tools/xbe_meta.py): in the staged XBE and next to it.
    import subprocess
    subprocess.run([sys.executable, str(ROOT / 'xbox' / 'tools' / 'xbe_meta.py'), 'dashboard', str(staged_xbe),
                    str(image), str(STAGE)], check=True)
    if devkit_128:
        # Test only: let the staged copy use a 128 MiB development kit's memory.
        clear_limit_64mb(STAGE / 'default.xbe')
    print(f'stage_disc: {changed} file(s) updated in {STAGE}')


if __name__ == '__main__':
    main()

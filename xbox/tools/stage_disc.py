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


# The game's text changed for the Xbox, in every language (installer/
# install.ps1 makes the same changes); each new string fits in the old one's
# room:
#   - the main menu's Return to arcade (ID_MENU_RETURN_TO_ARCADE, an Xbox 360
#     Arcade item) is the in-game reset here (src/menu_native.cpp): Return
#     to dash;
#   - the prompt for a missing controller (ID_X_CONNECT_CONTROLLER) names
#     the Xbox's.
TEXT_CHANGES = {
    'en': [('Return to arcade', 'Return to dash'),
           ('Connect Xbox 360 Controller', 'Connect Xbox Controller')],
    'de': [('Zur\u00fcck zu Arcade', 'Zur\u00fcck zum Dash'),
           ('Xbox 360 Controller anschlie\u00dfen', 'Xbox Controller anschlie\u00dfen')],
    'es': [('Volver a Arcade', 'Volver al Dash'),
           ('Conecta un Mando Xbox 360', 'Conecta un Mando Xbox')],
    'fr': [('Retour vers Arcade', 'Retour au Dash'),
           ('Veuillez connecter la manette Xbox 360', 'Veuillez connecter la manette Xbox')],
    'it': [('Torna a Xbox LIVE Arcade', 'Torna alla Dash'),
           ('Collega il Controller Xbox 360', 'Collega il Controller Xbox')],
}


def patch_text(loc_path, changes):
    # Each new string is written over the whole old one (UTF-16 big-endian,
    # matched between terminators), the rest of its room left as zeros, so
    # no offset in the file moves.
    data = bytearray(loc_path.read_bytes())
    for old_text, new_text in changes:
        old = old_text.encode('utf-16-be')
        new = new_text.encode('utf-16-be')
        assert len(new) <= len(old), new_text
        at = data.find(b'\0\0' + old + b'\0\0')
        if at < 0:
            sys.exit(f'{loc_path}: {old_text} not found')
        at += 2
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
    texts = {STAGE / f'Text-{lang}.loc': lang for lang in TEXT_CHANGES}
    for src in extracted.rglob('*'):
        if src.is_file():
            dst = STAGE / src.relative_to(extracted)
            if dst in texts:
                continue
            changed += copy(src, dst)
    # The game's text in each language, with the Xbox's changes (TEXT_CHANGES).
    for loc, lang in texts.items():
        staged_loc = loc.with_name(loc.name + '.new')
        shutil.copyfile(extracted / loc.name, staged_loc)
        patch_text(staged_loc, TEXT_CHANGES[lang])
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

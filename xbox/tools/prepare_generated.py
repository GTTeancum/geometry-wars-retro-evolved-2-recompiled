"""Copy the Xbox ReXGlue output (xbox/build/generated, from xbox/codegen.toml)
into xbox/build/gen for nxdk.

- The generated PCH is replaced by xbox/src/gen/pch.h, after checking that the
  image constants it hard-codes still match the generated ones.
- std::fma is rewritten to gw_fma (the Pentium III has no fused multiply-add).
- With registers as locals (xbox/codegen.toml), the SEH __finally funclets
  (entered with bl, the frame in r12 and the parent's r14-r31) would read a
  zeroed local r12: ReXGlue spills r14-r31 around those calls but not r12. So
  every function's r12 local starts from ctx.r12, and r12 is spilled with the
  rest before a funclet call.
- generated *_register.cpp is skipped: the Xbox runtime fills its dispatch
  table from PPCFuncMappings instead.

Files are only rewritten when their content changes, so make rebuilds only
what codegen actually changed.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / 'xbox' / 'build' / 'generated'
DST = ROOT / 'xbox' / 'build' / 'gen'
PCH = ROOT / 'xbox' / 'src' / 'gen' / 'pch.h'
NAME = 'geometry_wars_retro_evolved_2'

CONSTANTS = ('REX_IMAGE_BASE', 'REX_IMAGE_SIZE', 'REX_CODE_BASE', 'REX_CODE_SIZE',
             'REX_THUNK_RESERVE_SIZE', 'REXCRT_HEAP')


def constants(text):
    values = {}
    for name in CONSTANTS:
        m = re.search(r'(?m)^#define ' + name + r' (0x[0-9A-Fa-f]+|\d+)', text)
        if not m:
            sys.exit(f'{name} not found')
        values[name] = int(m[1], 0)
    return values


def write_if_changed(path, text):
    if path.exists() and path.read_text(encoding='utf-8') == text:
        return False
    path.write_text(text, encoding='utf-8')
    return True


def main():
    generated_pch = SRC / f'{NAME}_pch.h'
    if not generated_pch.exists():
        sys.exit(f'{generated_pch} missing: run xbox/tools/codegen.ps1 first')
    ours = PCH.read_text(encoding='utf-8')
    theirs = generated_pch.read_text(encoding='utf-8')
    if constants(ours) != constants(theirs):
        sys.exit(f'Image constants differ between {PCH} and {generated_pch}')

    DST.mkdir(parents=True, exist_ok=True)
    changed = 0
    wanted = set()
    for src in sorted(SRC.iterdir()):
        if src.suffix not in ('.cpp', '.h') or src.name.endswith('_register.cpp'):
            continue
        wanted.add(src.name)
        text = ours if src.name == generated_pch.name else src.read_text(encoding="utf-8")
        if src.suffix == '.cpp':
            text = text.replace('std::fma(', 'gw_fma(')
            text = text.replace('\tPPCRegister r12{};', '\tPPCRegister r12 = ctx.r12;')
            text = re.sub(r'(?m)^(\tctx\.r31 = r31;\n)(\tsub_[0-9A-F]+\(ctx, base\);)', r'\1\tctx.r12 = r12;\n\2', text)
        changed += write_if_changed(DST / src.name, text)
    for stale in DST.iterdir():
        if stale.suffix in ('.cpp', '.h') and stale.name not in wanted:
            stale.unlink()
    print(f'prepare_generated: {len(wanted)} files, {changed} updated')


if __name__ == '__main__':
    main()

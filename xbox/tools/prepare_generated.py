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
- Vector loads and stores (lvx/stvx) reverse the 16 bytes through SIMDe's
  shuffle_epi8, which the Pentium III (no SSSE3) runs as a byte loop over a
  mask table; they become gw_copy_reversed (four byte swaps, pch.h).
- fctiwz/fctidz (SIMDe's emulated cvttsd) become gw_trunc_i32/i64, and
  std::isnan the compiler builtin (nxdk builds without builtins, so it was a
  library call).
- Every label gets GW_LABEL_BARRIER, a compiler memory barrier: guest memory
  accesses are not volatile (pch.h), and loops polling memory jump through a
  label.
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

# store(dst, shuffle_epi8(load(src), VectorMaskL)): a vector register to or
# from memory with its bytes reversed.
_OPERAND = r'(REX_RAW_ADDR\(ea\)|(?:ctx\.)?v\d+\.u8)'
REVERSED_COPY = re.compile(
    r'simde_mm_store_si128\(\(simde__m128i\*\)' + _OPERAND +
    r', simde_mm_shuffle_epi8\(simde_mm_load_si128\(\(simde__m128i\*\)' + _OPERAND +
    r'\), simde_mm_load_si128\(\(simde__m128i\*\)VectorMaskL\)\)\);')

# A label of the generated code (every loop jumps back to one); gets a
# compiler memory barrier, as guest memory accesses are not volatile (pch.h).
LABEL = re.compile(r'(?m)^(loc_[0-9A-F]+:)$')

# fctiwz/fctidz through SIMDe's SSE2 emulation -> gw_trunc_i32/i64 (pch.h).
TRUNCATE = re.compile(r'simde_mm_cvttsd_si(32|64)\(simde_mm_load_sd\(&([\w.]+)\)\)')

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
            text = REVERSED_COPY.sub(r'gw_copy_reversed(\1, \2);', text)
            text = TRUNCATE.sub(r'gw_trunc_i\1(\2)', text)
            text = text.replace('std::isnan(', '__builtin_isnan(')
            text = LABEL.sub(r'\1\n\tGW_LABEL_BARRIER();', text)
        changed += write_if_changed(DST / src.name, text)
    for stale in DST.iterdir():
        if stale.suffix in ('.cpp', '.h') and stale.name not in wanted:
            stale.unlink()
    print(f'prepare_generated: {len(wanted)} files, {changed} updated')


if __name__ == '__main__':
    main()

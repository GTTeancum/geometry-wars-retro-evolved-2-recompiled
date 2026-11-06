"""Copy recompiled functions out of the generated sources, one file.

  python extract.py <xbox/build/gen> <out.inc> sub_XXXXXXXX [sub_YYYYYYYY ...]
"""
import pathlib
import sys

gen, out, names = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), sys.argv[3:]
sources = [p.read_text(encoding='utf-8') for p in sorted(gen.glob('*_recomp.*.cpp'))]
parts = []
for name in names:
    start = f'DEFINE_REX_FUNC({name})'
    for text in sources:
        at = text.find(start)
        if at >= 0:
            end = text.find('\nDEFINE_REX_FUNC(', at + len(start))
            parts.append(text[at:end if end >= 0 else len(text)])
            break
    else:
        sys.exit(f'{name} not found in {gen}')
out.write_text('\n'.join(parts) + '\n', encoding='utf-8')
print(f'{len(parts)} functions extracted')

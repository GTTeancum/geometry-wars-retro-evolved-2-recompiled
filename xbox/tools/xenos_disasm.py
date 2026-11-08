"""Disassemble Xbox 360 (Xenos) shader microcode, as captured by gpu_capture.py.

  python xbox/tools/xenos_disasm.py shader_vs_005B9000.bin [...]

Bit layouts follow ReXGlue 0.10.0 include/rex/graphics/format/ucode.h. The
output is meant for reading, not reassembly: control flow, vertex/texture
fetches, and each ALU slot's vector and scalar operations with swizzles.
"""
import struct
import sys

CF_OPS = {0: 'nop', 1: 'exec', 2: 'exec_end', 3: 'cexec', 4: 'cexec_end', 5: 'cexec_pred',
          6: 'cexec_pred_end', 7: 'loop_start', 8: 'loop_end', 9: 'ccall', 10: 'ret', 11: 'cjmp',
          12: 'alloc', 13: 'cexec_pred_clean', 14: 'cexec_pred_clean_end', 15: 'mark_vs_fetch_done'}
VEC_OPS = ['add', 'mul', 'max', 'min', 'seq', 'sgt', 'sge', 'sne', 'frc', 'trunc', 'floor', 'mad',
           'cndeq', 'cndge', 'cndgt', 'dp4', 'dp3', 'dp2add', 'cube', 'max4', 'setp_eq_push',
           'setp_ne_push', 'setp_gt_push', 'setp_ge_push', 'kill_eq', 'kill_gt', 'kill_ge',
           'kill_ne', 'dst', 'maxa']
VEC_ARGS = {'mad': 3, 'cndeq': 3, 'cndge': 3, 'cndgt': 3, 'dp2add': 3, 'frc': 1, 'trunc': 1,
            'floor': 1, 'max4': 1}
SCA_OPS = ['adds', 'adds_prev', 'muls', 'muls_prev', 'muls_prev2', 'maxs', 'mins', 'seqs', 'sgts',
           'sges', 'snes', 'frcs', 'truncs', 'floors', 'exp', 'logc', 'log', 'rcpc', 'rcpf', 'rcp',
           'rsqc', 'rsqf', 'rsq', 'maxas', 'maxasf', 'subs', 'subs_prev', 'setp_eq', 'setp_ne',
           'setp_gt', 'setp_ge', 'setp_inv', 'setp_pop', 'setp_clr', 'setp_rstr', 'kills_eq',
           'kills_gt', 'kills_ge', 'kills_ne', 'kills_one', 'sqrt', '?41', 'mulsc0', 'mulsc1',
           'addsc0', 'addsc1', 'subsc0', 'subsc1', 'sin', 'cos', 'retain_prev']
VERTEX_FORMATS = {6: '8_8_8_8', 7: '2_10_10_10', 16: '10_11_11', 17: '11_11_10', 25: '16_16',
                  26: '16_16_16_16', 31: '16_16_FLOAT', 32: '16_16_16_16_FLOAT', 33: '32',
                  34: '32_32', 35: '32_32_32_32', 36: '32_FLOAT', 37: '32_32_FLOAT',
                  38: '32_32_32_32_FLOAT', 57: '32_32_32_FLOAT'}
COMP = 'xyzw'


def bits(v, lo, n):
    return (v >> lo) & ((1 << n) - 1)


def signed(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


def fetch_dst_swizzle(sw):
    out = ''
    for i in range(4):
        c = bits(sw, i * 3, 3)
        out += 'xyzw01?_'[c]
    return out


def alu_swizzle(sw):
    # Each component is stored relative to its own position.
    s = ''.join(COMP[(bits(sw, i * 2, 2) + i) & 3] for i in range(4))
    return '' if s == 'xyzw' else '.' + s


def mask(m):
    return ''.join(COMP[i] for i in range(4) if m & (1 << i))


def dest_name(reg, export, shader_type, scalar=False):
    if not export:
        return f'r{reg}'
    if shader_type == 'vs':
        return {62: 'oPos', 63: 'oPts'}.get(reg, f'o{reg}')
    return {0: 'oC0', 1: 'oC1', 2: 'oC2', 3: 'oC3', 61: 'oDepth'}.get(reg, f'o{reg}')


def source(reg, sel, swiz, negate, abs_const):
    if sel:  # temporary register
        name = f'r{reg & 0x3F}'
        if reg & 0x80:
            name = f'|{name}|'
    else:
        name = f'c{reg}'
        if abs_const:
            name = f'|{name}|'
    return ('-' if negate else '') + name + alu_swizzle(swiz)


def vertex_fetch(w):
    d0, d1, d2 = w
    dst = bits(d0, 12, 6)
    src = bits(d0, 5, 6)
    const = bits(d0, 20, 5) * 3 + bits(d0, 25, 2)
    mini = bits(d1, 30, 1)
    fmt = bits(d1, 16, 6)
    stride = bits(d2, 0, 8)
    offset = signed(bits(d2, 8, 23), 23)
    num = 'signed' if bits(d1, 13, 1) else 'unsigned'
    norm = 'int' if bits(d1, 12, 1) else 'normalized'
    return (f"{'vfetch_mini' if mini else 'vfetch'} r{dst}.{fetch_dst_swizzle(bits(d1, 0, 12))}, "
            f"r{src}.{COMP[bits(d0, 30, 2)]}, vf{const} format={VERTEX_FORMATS.get(fmt, fmt)} "
            f"{num} {norm} stride={stride * 4} offset={offset * 4}")


def texture_fetch(w):
    d0, d1, d2 = w
    dst = bits(d0, 12, 6)
    src = bits(d0, 5, 6)
    const = bits(d0, 20, 5)
    src_sw = ''.join(COMP[bits(d0, 26 + i * 2, 2)] for i in range(3))
    dim = ['1D', '2D', '3D', 'Cube'][bits(d2, 14, 2)]
    return f'tfetch{dim} r{dst}.{fetch_dst_swizzle(bits(d1, 0, 12))}, r{src}.{src_sw}, tf{const}'


def alu(w, shader_type):
    d0, d1, d2 = w
    vdst, sdst = bits(d0, 0, 6), bits(d0, 8, 6)
    export = bits(d0, 15, 1)
    if export:  # an export's scalar result goes to the vector destination
        sdst = vdst
    vmask, smask = bits(d0, 16, 4), bits(d0, 20, 4)
    vclamp, sclamp = bits(d0, 24, 1), bits(d0, 25, 1)
    sop = bits(d0, 26, 6)
    abs_const = bits(d0, 7, 1)
    swz = [bits(d1, 16, 8), bits(d1, 8, 8), bits(d1, 0, 8)]  # src1, src2, src3
    neg = [bits(d1, 26, 1), bits(d1, 25, 1), bits(d1, 24, 1)]
    regs = [bits(d2, 16, 8), bits(d2, 8, 8), bits(d2, 0, 8)]
    vop = bits(d2, 24, 5)
    sel = [bits(d2, 31, 1), bits(d2, 30, 1), bits(d2, 29, 1)]
    pred = f"({'' if bits(d1, 28, 1) else '!'}p) " if bits(d1, 27, 1) else ''
    parts = []
    vname = VEC_OPS[vop] if vop < len(VEC_OPS) else f'v?{vop}'
    if vmask or vname.startswith(('kill', 'setp', 'maxa')):
        n = VEC_ARGS.get(vname, 2)
        srcs = ', '.join(source(regs[i], sel[i], swz[i], neg[i], abs_const) for i in range(n))
        sat = '_sat' if vclamp else ''
        parts.append(f'{vname}{sat} {dest_name(vdst, export, shader_type)}.{mask(vmask) or "_"}, {srcs}')
    sname = SCA_OPS[sop] if sop < len(SCA_OPS) else f's?{sop}'
    if smask or sname.startswith(('kill', 'setp', 'maxa')) or sname != 'retain_prev':
        if not (sname == 'retain_prev' and not smask):
            src3 = source(regs[2], sel[2], swz[2], neg[2], abs_const)
            sat = '_sat' if sclamp else ''
            sd = dest_name(sdst, export, shader_type, True)
            parts.append(f'{sname}{sat} {sd}.{mask(smask) or "_"}, {src3}')
    return pred + ('  +  '.join(parts) if parts else 'nop')


def disassemble(words, shader_type):
    out = []
    # Control flow: pairs of 48-bit instructions in 3 dwords, until the first
    # exec address (where instructions begin).
    cf = []
    first_exec = None
    i = 0
    while first_exec is None or i * 3 // 2 < first_exec * 3:
        if i * 3 // 2 + 2 >= len(words):
            break
        a, b, c = words[i // 2 * 3: i // 2 * 3 + 3]
        pair = [(a, b & 0xFFFF), ((b >> 16) | (c << 16) & 0xFFFFFFFF, c >> 16)]
        for d0, d1 in pair:
            op = bits(d1, 12, 4)
            cf.append((op, d0, d1))
            if op in (1, 2, 3, 4, 5, 6, 13, 14):
                addr = bits(d0, 0, 12)
                if first_exec is None or addr < first_exec:
                    first_exec = addr
        i += 2
        if len(cf) > 64:
            break
    for op, d0, d1 in cf:
        name = CF_OPS.get(op, f'cf?{op}')
        if op in (1, 2, 3, 4, 5, 6, 13, 14):
            addr, count, seq = bits(d0, 0, 12), bits(d0, 12, 3), bits(d0, 16, 12)
            out.append(f'{name} addr={addr} count={count}')
            for k in range(count):
                base = (addr + k) * 3
                inst = words[base:base + 3]
                if len(inst) < 3:
                    break
                is_fetch = bits(seq, k * 2, 1)
                if is_fetch:
                    fop = bits(inst[0], 0, 5)
                    text = vertex_fetch(inst) if fop == 0 else texture_fetch(inst) if fop == 1 else f'fetch?{fop}'
                else:
                    text = alu(inst, shader_type)
                out.append(f'  {addr + k:3d}: {text}')
            if op in (2, 4, 6, 14):
                break
        elif op == 12:
            size, kind = bits(d0, 0, 3), bits(d1, 9, 2)
            out.append(f"alloc {['?', 'position', 'interpolators/colors', 'memory'][kind]} size={size + 1}")
        elif op != 0:
            out.append(f'{name} {d0:08X} {d1:04X}')
    return out


def main():
    for path in sys.argv[1:]:
        data = open(path, 'rb').read()
        words = list(struct.unpack(f'>{len(data) // 4}I', data))
        kind = 'vs' if '_vs_' in path else 'ps'
        print(f'== {path} ({kind}, {len(words)} dwords)')
        print('\n'.join(disassemble(words, kind)))
        print()


if __name__ == '__main__':
    main()

"""Sampling profile of the game on a development Xbox, through its debug
monitor (xbdm, port 731).

  python xbox/tools/xbdm_profile.py <console address> [seconds] [--top N] [--function NAME] [--thread ID]

--thread counts only that thread (xbdm's id, as the by-thread table prints).
--inclusive also prints, for each function, the running samples with it
anywhere on the stack (2 KB): time including callees, to find subsystems.
--inside LO HI also counts the running samples with a recompiled function
sub_LO..sub_HI anywhere on the stack (2 KB from the stack pointer): the
share of time spent in that range including what it calls, e.g. the game's
Direct3D. Stale return addresses on the stack can inflate it slightly.

--function also prints the sampled addresses inside that function (as
offsets from its start, for llvm-objdump --disassemble-symbols).

Every round asks xbdm for each thread's context ('getcontext thread=N
control'). A thread that was switched out reports the kernel's switch code:
one waiting (it gave up the CPU itself) at KiSwapThread's return in the wait
path (kWaitEip, the same on every sample), one preempted elsewhere. For the
preempted ones the profile looks on the stack for the frame the interrupt
pushed when it stopped the thread (EIP in the XBE's code, CS = 8, EFLAGS
with interrupts on), else takes the first code address there. Those are
where the CPU's time goes; waiting threads are only counted.
Addresses are named from the link map (xbox/build/gw2.map): recompiled guest
functions appear as sub_XXXXXXXX, the runtime and renderer under their own
names.
"""
import bisect
import collections
import pathlib
import re
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'xbox' / 'tools'))
import xbdm_shot  # noqa: E402


import struct  # noqa: E402

CODE_START, CODE_END = 0x11000, 0x11000 + 0x85CBCB  # .text (gw2.map)
WAIT_EIP = 0x8002024C  # a thread switched out by a wait (this kernel)


def load_symbols():
    text = (ROOT / 'xbox' / 'build' / 'gw2.map').read_text(errors='replace')
    symbols = []
    for m in re.finditer(r'^\s*0001:[0-9a-fA-F]+\s+(\S+)\s+([0-9a-fA-F]{8,16})\s', text, re.M):
        symbols.append((int(m[2], 16), m[1]))
    symbols.sort()
    return [a for a, _ in symbols], [n for _, n in symbols]


def command(x, text):
    x.sock.sendall((text + '\r\n').encode())
    first = x.line()
    lines = [first]
    if first.startswith('202'):
        while True:
            line = x.line()
            if line == '.':
                break
            lines.append(line)
    return lines


def main():
    host = sys.argv[1]
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith('--') else 30
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 40
    addresses, names = load_symbols()
    x = xbdm_shot.Xbdm(host)
    threads = [int(t) for t in command(x, 'threads')[1:]]
    if '--thread' in sys.argv:
        threads = [int(sys.argv[sys.argv.index('--thread') + 1])]
    focus = sys.argv[sys.argv.index('--function') + 1] if '--function' in sys.argv else None
    inside = collections.Counter()
    by_function = collections.Counter()
    by_thread = collections.Counter()
    waiting = collections.Counter()
    rounds = 0
    end = time.time() + seconds
    span = None
    if '--inside' in sys.argv:
        k = sys.argv.index('--inside')
        span = (int(sys.argv[k + 1], 16), int(sys.argv[k + 2], 16))
    in_span = 0
    entries = collections.Counter()
    leaves = collections.defaultdict(collections.Counter)  # entry -> where the samples were

    def guest_of(address):
        i = bisect.bisect_right(addresses, address) - 1
        m = re.match(r'_sub_([0-9A-F]{8})$', names[i]) if i >= 0 else None
        return int(m[1], 16) if m else None

    inclusive_mode = '--inclusive' in sys.argv
    inclusive = collections.Counter()
    idle_rounds = 0  # rounds in which every thread was waiting: the CPU had nothing to do
    while time.time() < end:
        running_before = sum(by_thread.values())
        for tid in threads:
            reply = command(x, f'getcontext thread={tid} control')
            regs = {l.split('=')[0]: int(l.split('=')[1], 16) for l in reply[1:] if '=' in l}
            eip = regs.get('Eip')
            if eip is None:
                continue
            if eip == WAIT_EIP:
                waiting[tid] += 1
                continue
            if not CODE_START <= eip < CODE_END:
                # Preempted: the interrupt's frame, else the nearest code address.
                w = struct.unpack('<256I', x.getmem(regs['Esp'], 1024))
                eip = next((w[k] for k in range(254)
                            if CODE_START <= w[k] < CODE_END and w[k + 1] == 8 and w[k + 2] & 0x200), None)
                if eip is None:
                    eip = next((v for v in w if CODE_START <= v < CODE_END), None)
                if eip is None:
                    waiting[tid] += 1
                    continue
            i = bisect.bisect_right(addresses, eip) - 1
            name = names[i] if i >= 0 else f'{eip:08X}'
            by_function[name] += 1
            if inclusive_mode:
                words = struct.unpack('<512I', x.getmem(regs['Esp'], 2048))
                seen = set()
                for w in (eip,) + words:
                    if CODE_START <= w < CODE_END:
                        k = bisect.bisect_right(addresses, w) - 1
                        if k >= 0 and names[k] not in seen:
                            seen.add(names[k])
                            inclusive[names[k]] += 1
            if span:
                words = struct.unpack('<512I', x.getmem(regs['Esp'], 2048))
                # The outermost function of the range on the stack: the entry
                # the rest of the game called.
                entry = None
                for w in (eip,) + words:
                    if CODE_START <= w < CODE_END:
                        g = guest_of(w)
                        if g is not None and span[0] <= g <= span[1]:
                            entry = g
                if entry is not None:
                    in_span += 1
                    entries[entry] += 1
                    leaves[entry][name] += 1
            if focus and focus in name:
                inside[eip - addresses[i]] += 1
            by_thread[tid] += 1
        rounds += 1
        if sum(by_thread.values()) == running_before:
            idle_rounds += 1
    total = sum(by_function.values())
    print(f'{rounds} rounds, {total} samples in game code; every thread waiting in {idle_rounds} rounds '
          f'({100.0 * idle_rounds / max(rounds, 1):.0f}%)')
    if span:
        print(f'with sub_{span[0]:08X}..sub_{span[1]:08X} on the stack: {in_span} of {total} '
              f'({100.0 * in_span / max(total, 1):.0f}%)')
        for g, n in entries.most_common(25):
            print(f'  entry sub_{g:08X}: {n} ({100.0 * n / max(total, 1):.1f}%): ' +
                  ', '.join(f'{leaf} {c}' for leaf, c in leaves[g].most_common(6)))
    print('by thread (samples running / waiting):')
    for tid in threads:
        print(f'  {tid:4d}: {by_thread[tid]:6d} / {waiting[tid]:6d}')
    print(f'top {top} functions:')
    for name, n in by_function.most_common(top):
        print(f'  {100.0 * n / max(total, 1):5.1f}%  {n:6d}  {name}')
    if inclusive_mode:
        print('inclusive (with callees):')
        for name, n in inclusive.most_common(60):
            print(f'  {100.0 * n / max(total, 1):5.1f}%  {n:6d}  {name}')
    if focus:
        print(f'inside {focus} (offset: samples):')
        for offset, n in sorted(inside.items()):
            print(f'  +{offset:05X}: {n}')


if __name__ == '__main__':
    main()

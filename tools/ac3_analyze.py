#!/usr/bin/env python3
"""
ac3_analyze.py -- code discovery, function recovery and jump-table resolution
for the Ace Combat 3 PS-EXE images.

Both .text images interleave code with embedded data (CLUT art, sine tables,
strings), so a linear sweep would happily disassemble tables into garbage.
Instead this walks control flow recursively from a seed set and only trusts
what it can reach.

Seeds:
  * the PS-EXE entry point
  * every address the Psy-Q signature matcher identified
  * JAL targets discovered while walking

The genuinely hard case is `jr $rN` where rN is not $ra -- a switch dispatch
whose targets are in a table that no control-flow edge points at. resolve_jump_table()
backtracks from the jr through the block to recover the table address and
element count; see the docstring there for the pattern it matches.

Usage:
    python tools/ac3_analyze.py <exe.psx> [--symbols f.json] [--report]
    python tools/ac3_analyze.py <exe.psx> --disas 0x80125948 [--count N]
    python tools/ac3_analyze.py <exe.psx> --json out.json
"""
import sys, os, json, struct, collections
from mips import decode, Kind, R_RA, R_ZERO, REG_NAMES, LOADS

BACKTRACK_LIMIT = 256
MAX_TABLE_ENTRIES = 512
BIOS_VECTORS = {0xA0, 0xB0, 0xC0}
FUNC_WALK_LIMIT = 3000


class Program:
    def __init__(self, path):
        raw = open(path, 'rb').read()
        if raw[:8] != b'PS-X EXE':
            raise ValueError(f'{path}: not a PS-EXE')
        self.pc0, _gp = struct.unpack_from('<II', raw, 0x10)
        self.base, self.size = struct.unpack_from('<II', raw, 0x18)
        self.data = raw[0x800:0x800 + self.size]
        self.path = path
        n = self.size // 4
        self.words = struct.unpack('<%dI' % n, self.data[:n * 4])
        self._cache = {}

    def __contains__(self, addr):
        return self.base <= addr < self.base + self.size and addr % 4 == 0

    def word(self, addr):
        return self.words[(addr - self.base) // 4]

    def insn(self, addr):
        i = self._cache.get(addr)
        if i is None:
            i = decode(addr, self.word(addr))
            self._cache[addr] = i
        return i


class Analysis:
    def __init__(self, prog):
        self.prog = prog
        self.code = set()
        self.func_starts = set()
        self.call_sites = collections.defaultdict(set)
        self.jump_tables = {}
        self.bios_calls = {}
        self.unresolved_jr = []
        self.returns = set()
        self.seed_origin = {}


def bios_stub(prog, jr_addr):
    jr = prog.insn(jr_addr)
    vector = None
    for back in range(1, 4):
        a = jr_addr - back * 4
        if a not in prog:
            break
        ins = prog.insn(a)
        if (ins.valid and ins.mnemonic == 'addiu' and ins.rs == R_ZERO
                and ins.rt == jr.rs and ins.imm in BIOS_VECTORS):
            vector = ins.imm
            break
    if vector is None:
        return None

    funcnum = None
    delay = jr_addr + 4
    if delay in prog:
        d = prog.insn(delay)
        if d.valid and d.mnemonic == 'addiu' and d.rs == R_ZERO:
            funcnum = d.imm
    return {'vector': vector, 'funcnum': funcnum}


def looks_like_function(prog, addr, code=frozenset()):
    if addr not in prog:
        return False
    first = prog.insn(addr)
    if not first.valid:
        return False
    a = addr
    for _ in range(FUNC_WALK_LIMIT):
        if a not in prog:
            return False
        ins = prog.insn(a)
        if not ins.valid:
            return False
        if ins.is_return:
            return True
        if ins.kind in (Kind.JUMP, Kind.JR):
            return True
        a += 4
    return False


def harvest_pointers(prog, analysis):
    found = set()
    for i, w in enumerate(prog.words):
        if w % 4 or w not in prog:
            continue
        if w in analysis.code or w in analysis.func_starts:
            continue
        if not looks_like_function(prog, w):
            continue
        ins = prog.insn(w)
        if (ins.word & 0xFFFF0000) == 0x27BD0000 and (ins.word & 0x8000):
            found.add(w)
        elif prog.insn(w - 4).is_return if (w - 4) in prog else False:
            found.add(w)
    for f in found:
        analysis.seed_origin.setdefault(f, 'pointer')
    return found


def refine_start(prog, addr, limit=8):
    a = addr
    for _ in range(limit):
        p = a - 4
        if p not in prog:
            break
        pi = prog.insn(p)
        if not pi.valid or pi.is_nop or pi.has_delay_slot:
            break
        pp = p - 4
        if pp in prog and prog.insn(pp).valid and prog.insn(pp).has_delay_slot:
            break
        a = p
    return a


def scan_prologues(prog, analysis):
    found = set()
    for i, w in enumerate(prog.words):
        if (w & 0xFFFF0000) != 0x27BD0000 or not (w & 0x8000):
            continue
        a = prog.base + i * 4
        if a in analysis.code or a in analysis.func_starts:
            continue
        if looks_like_function(prog, a):
            found.add(refine_start(prog, a))
    for f in found:
        analysis.seed_origin.setdefault(f, 'prologue')
    return found


def scan_after_returns(prog, analysis, covered):
    found = set()
    for i, w in enumerate(prog.words):
        if w != 0x03E00008:
            continue
        a = prog.base + i * 4 + 8
        for _ in range(16):
            if a in prog and prog.insn(a).is_nop:
                a += 4
            else:
                break
        if a not in prog or a in analysis.code or a in analysis.func_starts:
            continue
        if a in covered:
            continue
        if looks_like_function(prog, a):
            found.add(a)
    for f in found:
        analysis.seed_origin.setdefault(f, 'after-return')
    return found


def covered_addresses(prog, funcs):
    out = set()
    for s, e in funcs.values():
        out.update(range(s, e, 4))
    return out


def resolve_jump_table(prog, jr_addr, block_start):
    start = max(prog.base, jr_addr - BACKTRACK_LIMIT * 4)
    regs = {}
    bounds = {}

    a = start
    while a <= jr_addr:
        ins = prog.insn(a)
        if not ins.valid:
            regs.clear(); bounds.clear(); a += 4; continue
        m = ins.mnemonic

        if m == 'lui':
            regs[ins.rt] = ('c', (ins.imm << 16) & 0xFFFFFFFF)
        elif m == 'addiu':
            src = ('c', 0) if ins.rs == R_ZERO else regs.get(ins.rs)
            regs[ins.rt] = (('c', (src[1] + ins.simm) & 0xFFFFFFFF)
                            if src and src[0] == 'c' else None)
        elif m == 'sll' and ins.shamt == 2:
            regs[ins.rd] = ('idx', ins.rt)
        elif m in ('addu', 'add'):
            x, y = regs.get(ins.rs), regs.get(ins.rt)
            out = None
            if x and y:
                if x[0] == 'c' and y[0] == 'c':
                    out = ('c', (x[1] + y[1]) & 0xFFFFFFFF)
                elif x[0] == 'c' and y[0] == 'idx':
                    out = ('tab', x[1], y[1])
                elif x[0] == 'idx' and y[0] == 'c':
                    out = ('tab', y[1], x[1])
            regs[ins.rd] = out
        elif m == 'sltiu':
            bounds[ins.rs] = ins.imm
            regs[ins.rt] = None
        elif m == 'lw':
            base = regs.get(ins.rs)
            if base and base[0] == 'tab':
                regs[ins.rt] = ('load',
                                (base[1] + ins.simm) & 0xFFFFFFFF, base[2])
            else:
                regs[ins.rt] = None
        else:
            d = ins.writes_reg()
            if d is not None:
                regs[d] = None
        a += 4

    val = regs.get(prog.insn(jr_addr).rs)
    if not val or val[0] != 'load':
        return None
    table, idx_reg = val[1], val[2]
    if table not in prog:
        return None

    count = bounds.get(idx_reg)
    targets, addr = [], table
    for _ in range(count if count else MAX_TABLE_ENTRIES):
        if addr not in prog:
            break
        t = prog.word(addr)
        if t not in prog or t % 4:
            break
        targets.append(t)
        addr += 4
    if not targets:
        return None
    return {'table': table, 'count': count, 'targets': targets,
            'bounded': count is not None}


def explore(prog, seeds, analysis):
    work = collections.deque(seeds)
    queued = set(seeds)
    block_start = {}

    while work:
        start = work.popleft()
        if start not in prog:
            continue
        addr = start
        cur_block = start
        while addr in prog:
            if addr in analysis.code:
                break
            ins = prog.insn(addr)
            if not ins.valid:
                break
            analysis.code.add(addr)
            block_start[addr] = cur_block

            if ins.has_delay_slot:
                delay = addr + 4
                if delay in prog and prog.insn(delay).valid:
                    analysis.code.add(delay)

                if ins.kind is Kind.CALL:
                    analysis.call_sites[ins.target].add(addr)
                    analysis.func_starts.add(ins.target)
                    if ins.target not in queued:
                        queued.add(ins.target); work.append(ins.target)
                    addr += 8
                    cur_block = addr
                    continue

                if ins.kind is Kind.BRANCH:
                    if ins.target not in queued and ins.target not in analysis.code:
                        queued.add(ins.target); work.append(ins.target)
                    addr += 8
                    cur_block = addr
                    continue

                if ins.kind is Kind.JUMP:
                    if ins.target not in queued and ins.target not in analysis.code:
                        queued.add(ins.target); work.append(ins.target)
                    break

                if ins.kind is Kind.JR:
                    if ins.is_return:
                        analysis.returns.add(addr)
                    else:
                        stub = bios_stub(prog, addr)
                        if stub:
                            analysis.bios_calls[addr] = stub
                        else:
                            jt = resolve_jump_table(prog, addr, cur_block)
                            if jt:
                                analysis.jump_tables[addr] = jt
                                for t in jt['targets']:
                                    if t not in queued and t not in analysis.code:
                                        queued.add(t); work.append(t)
                            else:
                                analysis.unresolved_jr.append(addr)
                    break

                if ins.kind is Kind.JALR:
                    addr += 8
                    cur_block = addr
                    continue

            if ins.kind is Kind.TRAP:
                addr += 4
                cur_block = addr
                continue
            addr += 4


def assign_functions(prog, analysis):
    starts = sorted(s for s in analysis.func_starts if s in prog)
    code = analysis.code
    funcs = {}
    for i, s in enumerate(starts):
        limit = starts[i + 1] if i + 1 < len(starts) else prog.base + prog.size
        end = s
        a = s
        while a < limit and a in code:
            end = a + 4
            a += 4
        funcs[s] = (s, end)
    return funcs


def merge_split_functions(prog, analysis, verbose=False):
    removed_total = 0
    for _ in range(8):
        funcs = assign_functions(prog, analysis)
        starts = sorted(funcs)
        owner = {}
        for s in starts:
            lo, hi = funcs[s]
            for a in range(lo, hi, 4):
                owner[a] = s

        remove = set()
        for s in starts:
            lo, hi = funcs[s]
            for a in range(lo, hi, 4):
                if a not in analysis.code:
                    continue
                ins = prog.insn(a)
                if not ins.valid or ins.kind not in (Kind.BRANCH, Kind.JUMP):
                    continue
                t = ins.target
                if t is None or t in analysis.func_starts:
                    continue
                other = owner.get(t)
                if other is not None and other != s:
                    remove.add(max(s, other))
        remove.discard(prog.pc0)
        remove -= set(analysis.call_sites)
        if not remove:
            break
        analysis.func_starts -= remove
        removed_total += len(remove)
        if verbose:
            print(f'   merge: dropped {len(remove)} false function splits')
    return removed_total


def analyze(prog, extra_seeds=(), verbose=False):
    a = Analysis(prog)
    seeds = {prog.pc0} | set(extra_seeds)
    for s in seeds:
        a.seed_origin.setdefault(s, 'entry/psyq')
    a.func_starts |= seeds
    explore(prog, seeds, a)

    for round_no in range(12):
        before = len(a.code)
        new = harvest_pointers(prog, a) | scan_prologues(prog, a)
        new |= {t for t in a.func_starts if t in prog and t not in a.code}
        if not new:
            break
        a.func_starts |= new
        explore(prog, new, a)
        if verbose:
            print(f'   stage1 round {round_no+1}: +{len(new)} seeds, '
                  f'code {before*4} -> {len(a.code)*4} bytes')
        if len(a.code) == before:
            break

    for round_no in range(4):
        covered = covered_addresses(prog, assign_functions(prog, a))
        new = scan_after_returns(prog, a, covered)
        if not new:
            break
        a.func_starts |= new
        explore(prog, new, a)
        if verbose:
            print(f'   stage2 round {round_no+1}: +{len(new)} leaf seeds, '
                  f'code now {len(a.code)*4} bytes')

    merge_split_functions(prog, a, verbose)
    return a


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = sys.argv[1]
    prog = Program(path)

    symbols = {}
    if '--symbols' in sys.argv:
        sp = sys.argv[sys.argv.index('--symbols') + 1]
        for e in json.load(open(sp)):
            symbols[e['addr']] = e['name']
    else:
        guess = os.path.splitext(os.path.basename(path))[0] + '_psyq_symbols.json'
        if os.path.exists(guess):
            for e in json.load(open(guess)):
                symbols[e['addr']] = e['name']

    if '--disas' in sys.argv:
        start = int(sys.argv[sys.argv.index('--disas') + 1], 0)
        count = 32
        if '--count' in sys.argv:
            count = int(sys.argv[sys.argv.index('--count') + 1], 0)
        for k in range(count):
            a = start + k * 4
            if a not in prog:
                break
            i = prog.insn(a)
            label = symbols.get(a, '')
            mark = f'  <- {label}' if label else ''
            print(f'  {a:08X}  {i.word:08X}  {i.text(symbols)}{mark}')
        return 0

    a = analyze(prog, symbols.keys(), verbose='--report' in sys.argv)
    funcs = assign_functions(prog, a)

    code_bytes = len(a.code) * 4
    print(f'{os.path.basename(path)}')
    print(f'  .text            0x{prog.base:08X} + {prog.size} bytes')
    print(f'  entry point      0x{prog.pc0:08X}')
    print(f'  seeds from psyq  {len(symbols)}')
    print()
    print(f'  code discovered  {code_bytes} bytes  ({100.0*code_bytes/prog.size:.1f}% of .text)')
    print(f'  functions        {len(funcs)}')
    print(f'  named by psyq    {sum(1 for s in funcs if s in symbols)}')
    print(f'  return sites     {len(a.returns)}')
    print()
    origins = collections.Counter(a.seed_origin.get(s, '?') for s in funcs)
    print(f'  discovered via   {dict(origins)}')
    print()
    print(f'  BIOS call stubs  {len(a.bios_calls)}')
    vecs = collections.Counter(v['vector'] for v in a.bios_calls.values())
    if vecs:
        print(f'    by vector      ' +
              ', '.join(f'0x{k:02X}: {c}' for k, c in sorted(vecs.items())))
    resolved = len(a.jump_tables)
    unres = len(a.unresolved_jr)
    print(f'  indirect jr $rN  {resolved + unres} total (excluding BIOS stubs)')
    print(f'    resolved       {resolved}  '
          f'({sum(len(j["targets"]) for j in a.jump_tables.values())} targets)')
    print(f'    unresolved     {unres}')
    bounded = sum(1 for j in a.jump_tables.values() if j['bounded'])
    print(f'    of resolved, bounded by sltiu: {bounded}')

    if a.jump_tables:
        print('\n  sample jump tables:')
        for jr in sorted(a.jump_tables)[:8]:
            j = a.jump_tables[jr]
            print(f'    jr @0x{jr:08X} -> table 0x{j["table"]:08X} '
                  f'count={j["count"]} targets={len(j["targets"])}')
    if a.unresolved_jr:
        print('\n  unresolved jr sites:')
        for x in a.unresolved_jr[:12]:
            print(f'    0x{x:08X}')

    if '--json' in sys.argv:
        out = sys.argv[sys.argv.index('--json') + 1]
        with open(out, 'w') as f:
            json.dump({
                'base': prog.base, 'size': prog.size, 'entry': prog.pc0,
                'functions': [{'start': s, 'end': e,
                               'name': symbols.get(s)} for s, e in sorted(funcs.values())],
                'jump_tables': {f'0x{k:08X}': v for k, v in a.jump_tables.items()},
                'bios_calls': {f'0x{k:08X}': v for k, v in a.bios_calls.items()},
                'unresolved_jr': [f'0x{x:08X}' for x in a.unresolved_jr],
                'code': sorted(a.code),
            }, f)
        print(f'\nwrote {out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""
recompile.py -- static recompiler: recovered MIPS functions -> C.

One C function per MIPS function, control flow inside a function expressed as
labels and gotos, calls as direct C calls. The CPU context is passed
explicitly so the host compiler can keep hot registers in host registers
across a block.

Delay slots
    A branch's delay slot executes before the branch is taken, and the delay
    slot may clobber a register the branch condition reads. So the condition
    is evaluated into a temporary first, then the delay slot is emitted, then
    the jump. Same shape for jr/jalr, where the *target* is captured first.

Load delay slots
    MIPS-I does not make a loaded value available to the next instruction.
    Psy-Q's compiler respects that when scheduling, so the generated code is
    correct if loads land immediately -- this holds for compiler output but
    would not for hand-written assembly that deliberately fills the slot.
    Any such code would need the load buffered; none is known here.

Usage:
    python tools/recompile.py <exe.psx> <analysis.json> <outdir>
                              [--symbols f.json] [--chunks N]
"""
import sys, os, json, collections
from mips import Kind, REG_NAMES, R_RA, R_ZERO
from ac3_analyze import Program

R_T1 = 9


def creg(n):
    return '0u' if n == R_ZERO else f'ctx->r[{n}]'


class FunctionEmitter:
    def __init__(self, rc, start, end):
        self.rc = rc
        self.prog = rc.prog
        self.start, self.end = start, end
        self.labels = set()
        self.consumed = set()
        self.notes = collections.Counter()

    def inside(self, a):
        return self.start <= a < self.end

    def collect_labels(self):
        a = self.start
        while a < self.end:
            ins = self.prog.insn(a)
            if ins.valid and ins.target is not None and self.inside(ins.target):
                if ins.kind in (Kind.BRANCH, Kind.JUMP):
                    self.labels.add(ins.target)
            jt = self.rc.jump_tables.get(a)
            if jt:
                for t in jt['targets']:
                    if self.inside(t):
                        self.labels.add(t)
            a += 4

    def goto_or_tail(self, target, out, indent='    '):
        if self.inside(target):
            out.append(f'{indent}goto L_{target:08X};')
        elif target in self.rc.func_names:
            out.append(f'{indent}{self.rc.func_names[target]}(ctx); return;')
            self.notes['tail_call'] += 1
        else:
            out.append(f'{indent}ac3_jump_indirect(ctx, 0x{target:08X}u); return;')
            self.notes['cross_function_jump'] += 1

    def data_insn(self, ins):
        m, rs, rt, rd = ins.mnemonic, ins.rs, ins.rt, ins.rd
        simm, imm, sh = ins.simm, ins.imm, ins.shamt
        S, T, D = creg(rs), creg(rt), rd

        def to(reg, expr):
            return '' if reg == R_ZERO else f'ctx->r[{reg}] = {expr};'

        if ins.is_nop:
            return ''
        if m in ('sll', 'srl', 'sra'):
            fn = {'sll': 'SHL', 'srl': 'SHR', 'sra': 'SAR'}[m]
            return to(D, f'{fn}({T}, {sh}u)')
        if m in ('sllv', 'srlv', 'srav'):
            fn = {'sllv': 'SHL', 'srlv': 'SHR', 'srav': 'SAR'}[m]
            return to(D, f'{fn}({T}, {S})')
        if m in ('add', 'addu'):
            return to(D, f'{S} + {T}')
        if m in ('sub', 'subu'):
            return to(D, f'{S} - {T}')
        if m == 'and':
            return to(D, f'{S} & {T}')
        if m == 'or':
            return to(D, f'{S} | {T}')
        if m == 'xor':
            return to(D, f'{S} ^ {T}')
        if m == 'nor':
            return to(D, f'~({S} | {T})')
        if m == 'slt':
            return to(D, f'SLT({S}, {T})')
        if m == 'sltu':
            return to(D, f'SLTU({S}, {T})')
        if m in ('addi', 'addiu'):
            return to(rt, f'{S} + 0x{simm & 0xFFFFFFFF:08X}u')
        if m == 'slti':
            return to(rt, f'SLT({S}, 0x{simm & 0xFFFFFFFF:08X}u)')
        if m == 'sltiu':
            return to(rt, f'SLTU({S}, 0x{simm & 0xFFFFFFFF:08X}u)')
        if m == 'andi':
            return to(rt, f'{S} & 0x{imm:04X}u')
        if m == 'ori':
            return to(rt, f'{S} | 0x{imm:04X}u')
        if m == 'xori':
            return to(rt, f'{S} ^ 0x{imm:04X}u')
        if m == 'lui':
            return to(rt, f'0x{(imm << 16) & 0xFFFFFFFF:08X}u')
        if m in ('mult', 'multu', 'div', 'divu'):
            return f'ac3_{m}(ctx, {S}, {T});'
        if m == 'mfhi':
            return to(D, 'ctx->hi')
        if m == 'mflo':
            return to(D, 'ctx->lo')
        if m == 'mthi':
            return f'ctx->hi = {S};'
        if m == 'mtlo':
            return f'ctx->lo = {S};'

        addr = f'{S} + 0x{simm & 0xFFFFFFFF:08X}u'
        LOADS = {'lb': 'MEM_R8S', 'lbu': 'MEM_R8U', 'lh': 'MEM_R16S',
                 'lhu': 'MEM_R16U', 'lw': 'MEM_R32'}
        if m in LOADS:
            return to(rt, f'{LOADS[m]}({addr})')
        if m == 'lwl':
            return to(rt, f'MEM_LWL({T}, {addr})')
        if m == 'lwr':
            return to(rt, f'MEM_LWR({T}, {addr})')
        STORES = {'sb': 'MEM_W8', 'sh': 'MEM_W16', 'sw': 'MEM_W32',
                  'swl': 'MEM_SWL', 'swr': 'MEM_SWR'}
        if m in STORES:
            return f'{STORES[m]}({addr}, {T});'

        if m == 'mfc0':
            return to(rt, f'ac3_mfc0(ctx, {rd}u)')
        if m == 'mtc0':
            return f'ac3_mtc0(ctx, {rd}u, {T});'
        if m == 'rfe':
            return 'ac3_rfe(ctx);'
        if m == 'mfc2':
            return to(rt, f'ac3_gte_mfc2(ctx, {rd}u)')
        if m == 'cfc2':
            return to(rt, f'ac3_gte_cfc2(ctx, {rd}u)')
        if m == 'mtc2':
            return f'ac3_gte_mtc2(ctx, {rd}u, {T});'
        if m == 'ctc2':
            return f'ac3_gte_ctc2(ctx, {rd}u, {T});'
        if m == 'lwc2':
            return f'ac3_gte_mtc2(ctx, {rt}u, MEM_R32({addr}));'
        if m == 'swc2':
            return f'MEM_W32({addr}, ac3_gte_mfc2(ctx, {rt}u));'
        if m == 'gte':
            return f'ac3_gte_op(ctx, 0x{ins.word & 0x01FFFFFF:07X}u);'
        if m in ('syscall', 'break'):
            return f'ac3_{m if m == "break" else "syscall"}(ctx);'
        return None

    def delay_lines(self, addr, indent, mark=True):
        out = []
        if addr not in self.prog or not self.inside(addr):
            return out
        d = self.prog.insn(addr)
        if mark:
            self.consumed.add(addr)
        if not d.valid:
            self.notes['bad_delay_slot'] += 1
            return out
        if d.has_delay_slot:
            out.append(f'{indent}ac3_unimplemented(ctx, 0x{addr:08X}u, '
                       f'"branch in delay slot");')
            self.notes['branch_in_delay_slot'] += 1
            return out
        stmt = self.data_insn(d)
        if stmt is None:
            out.append(f'{indent}ac3_unimplemented(ctx, 0x{addr:08X}u, '
                       f'"{d.mnemonic}");')
            self.notes['untranslated'] += 1
        elif stmt:
            out.append(f'{indent}{stmt}')
        return out

    def control_insn(self, ins, out):
        a, k = ins.addr, ins.kind
        delay = a + 4
        ind = '        '

        if k is Kind.BRANCH:
            m, S, T = ins.mnemonic, creg(ins.rs), creg(ins.rt)
            cond = {
                'beq':  f'{S} == {T}', 'bne': f'{S} != {T}',
                'blez': f'(s32){S} <= 0', 'bgtz': f'(s32){S} > 0',
                'bltz': f'(s32){S} < 0',  'bgez': f'(s32){S} >= 0',
                'bltzal': f'(s32){S} < 0', 'bgezal': f'(s32){S} >= 0',
            }[m]
            out.append(f'    {{ u32 _c = ({cond});')
            if m in ('bltzal', 'bgezal'):
                out.append(f'{ind}ctx->r[31] = 0x{a + 8:08X}u;')
            out += self.delay_lines(delay, ind)
            out.append(f'{ind}if (_c)')
            self.goto_or_tail(ins.target, out, ind + '    ')
            out.append('    }')
            return

        if k is Kind.JUMP:
            out.append(f'    {{')
            out += self.delay_lines(delay, ind)
            self.goto_or_tail(ins.target, out, ind)
            out.append('    }')
            return

        if k is Kind.CALL:
            name = self.rc.func_names.get(ins.target)
            out.append(f'    {{')
            out.append(f'{ind}ctx->r[31] = 0x{a + 8:08X}u;')
            out += self.delay_lines(delay, ind)
            if name == 'psyq_setjmp':
                out.append(f'{ind}{{ int _js = ac3_setjmp_save(ctx, ctx->r[4]);')
                out.append(f'{ind}  ctx->r[2] = (u32)setjmp(*ac3_setjmp_env(_js)); }}')
                self.notes['inline_setjmp'] += 1
            elif name:
                out.append(f'{ind}{name}(ctx);')
            else:
                out.append(f'{ind}ac3_call_indirect(ctx, 0x{ins.target:08X}u);')
                self.notes['call_unknown_target'] += 1
            if self.rc.profile:
                out.append(f'{ind}ac3_cur_func = 0x{self.start:08X}u;')
            out.append('    }')
            return

        if k is Kind.JALR:
            out.append(f'    {{ u32 _t = {creg(ins.rs)};')
            if ins.rd != R_ZERO:
                out.append(f'{ind}ctx->r[{ins.rd}] = 0x{a + 8:08X}u;')
            out += self.delay_lines(delay, ind)
            out.append(f'{ind}ac3_call_indirect(ctx, _t);')
            if self.rc.profile:
                out.append(f'{ind}ac3_cur_func = 0x{self.start:08X}u;')
            out.append('    }')
            self.notes['indirect_call'] += 1
            return

        if k is Kind.JR:
            if ins.is_return:
                out.append(f'    {{')
                out += self.delay_lines(delay, ind)
                out.append(f'{ind}return;')
                out.append('    }')
                return
            bios = self.rc.bios_calls.get(a)
            if bios:
                out.append(f'    {{ u32 _v = {creg(ins.rs)};')
                out += self.delay_lines(delay, ind)
                out.append(f'{ind}ac3_bios_call(ctx, _v, {creg(R_T1)});')
                out.append(f'{ind}return;')
                out.append('    }')
                self.notes['bios_call'] += 1
                return
            jt = self.rc.jump_tables.get(a)
            out.append(f'    {{ u32 _t = {creg(ins.rs)};')
            out += self.delay_lines(delay, ind)
            if jt:
                out.append(f'{ind}switch (_t) {{')
                for t in dict.fromkeys(jt['targets']):
                    out.append(f'{ind}case 0x{t:08X}u:')
                    self.goto_or_tail(t, out, ind + '    ')
                out.append(f'{ind}default: ac3_jump_indirect(ctx, _t); return;')
                out.append(f'{ind}}}')
                self.notes['switch'] += 1
            else:
                out.append(f'{ind}ac3_jump_indirect(ctx, _t); return;')
                self.notes['unresolved_jr'] += 1
            out.append('    }')
            return

        if k is Kind.TRAP:
            stmt = self.data_insn(ins)
            out.append(f'    {stmt}')
            return

    def emit(self):
        self.collect_labels()
        name = self.rc.func_names[self.start]
        out = [f'void {name}(ctx_t *ctx) {{']
        if self.start in self.rc.trace_funcs:
            out.append(f'    ac3_trace_call(0x{self.start:08X}u, ctx->r[4], '
                       f'ctx->r[5], ctx->r[6], ctx->r[7]);')
        if self.rc.profile:
            out.append(f'    ac3_prof[{self.rc.index[self.start]}]++;')
            out.append(f'    ac3_cur_func = 0x{self.start:08X}u;')
        a = self.start
        while a < self.end:
            if a in self.consumed:
                a += 4
                continue
            if a in self.labels:
                out.append(f'L_{a:08X}:;')
            ins = self.prog.insn(a)
            if not ins.valid:
                out.append(f'    ac3_unimplemented(ctx, 0x{a:08X}u, '
                           f'"undecodable 0x{ins.word:08X}");')
                self.notes['undecodable'] += 1
                a += 4
                continue
            if ins.has_delay_slot or ins.kind is Kind.TRAP:
                self.control_insn(ins, out)
                if ins.has_delay_slot and (a + 4) in self.labels:
                    d = a + 4
                    out.append(f'    goto D_{d:08X};')
                    out.append(f'L_{d:08X}:;')
                    out += self.delay_lines(d, '    ', mark=False)
                    out.append(f'D_{d:08X}:;')
                    self.notes['branch_into_delay_slot'] += 1
                a += 8 if ins.has_delay_slot else 4
                continue
            stmt = self.data_insn(ins)
            if stmt is None:
                out.append(f'    ac3_unimplemented(ctx, 0x{a:08X}u, '
                           f'"{ins.mnemonic}");')
                self.notes['untranslated'] += 1
            elif stmt:
                out.append(f'    {stmt}')
            a += 4
        if self.falls_off_end():
            if self.end in self.rc.func_names:
                out.append(f'    {self.rc.func_names[self.end]}(ctx);')
            else:
                out.append(f'    ac3_jump_indirect(ctx, 0x{self.end:08X}u);')
            self.notes['fall_through'] += 1
        out.append('}')
        return out

    @staticmethod
    def is_padding(ins):
        return ins.valid and (ins.word & 0xFC00F83F) == 0

    def falls_off_end(self):
        a, last = self.start, None
        while a < self.end:
            ins = self.prog.insn(a)
            last = ins
            a += 8 if (ins.valid and ins.has_delay_slot) else 4
        if last is None or not last.valid:
            return False
        if last.kind in (Kind.JUMP, Kind.JR):
            return False
        if last.kind is Kind.TRAP:
            return last.mnemonic != 'break'
        if last.kind is Kind.BRANCH and last.mnemonic in ('beq', 'bgez') \
                and last.rs == last.rt == R_ZERO:
            return False
        if last.kind is Kind.BRANCH and last.mnemonic == 'beq' and last.rs == last.rt:
            return False
        a = self.end - 4
        while a >= self.start and self.is_padding(self.prog.insn(a)):
            a -= 4
        if a >= self.start:
            t = self.prog.insn(a - 4) if a - 4 >= self.start else None
            if t is not None and t.valid and t.has_delay_slot and t.kind in (Kind.JUMP, Kind.JR):
                return False
            p = self.prog.insn(a)
            if p.valid and p.kind in (Kind.JUMP, Kind.JR, Kind.TRAP) and p.mnemonic != 'syscall':
                return False
        return True


class Recompiler:
    def __init__(self, prog, analysis, symbols, hle_names=(), profile=False):
        self.profile = profile
        self.trace_funcs = set()
        self.prog = prog
        self.symbols = symbols
        self.hle_names = set(hle_names)
        self.functions = [(f['start'], f['end']) for f in analysis['functions']]
        self.jump_tables = {int(k, 16): v
                            for k, v in analysis.get('jump_tables', {}).items()}
        self.bios_calls = {}
        for f in analysis.get('bios_calls', {}) or {}:
            self.bios_calls[int(f, 16)] = analysis['bios_calls'][f]
        self.index = {s: i for i, (s, _) in enumerate(sorted(self.functions))}
        self.func_names = {}
        for s, _ in self.functions:
            nm = symbols.get(s)
            self.func_names[s] = (f'psyq_{nm}' if nm else f'func_{s:08X}')

    def run(self, outdir, chunks=16):
        os.makedirs(outdir, exist_ok=True)
        notes = collections.Counter()
        bodies = []
        self.hle_used = []
        for start, end in sorted(self.functions):
            nm = self.symbols.get(start)
            if nm and nm in self.hle_names:
                self.hle_used.append((start, nm))
                notes['hle_native'] += 1
                continue
            fe = FunctionEmitter(self, start, end)
            bodies.append((start, fe.emit()))
            notes.update(fe.notes)

        header = [
            '#ifndef AC3_FUNCS_H', '#define AC3_FUNCS_H',
            '#include "ac3_runtime.h"', '',
        ]
        for s, _ in sorted(self.functions):
            header.append(f'void {self.func_names[s]}(ctx_t *ctx);')
        header += ['', 'extern unsigned long long ac3_prof[];',
                   'extern const unsigned ac3_prof_len;',
                   'extern u32 ac3_cur_func;',
                   'void ac3_trace_call(u32 fn, u32 a0, u32 a1, u32 a2, u32 a3);',
                   '', '#endif']
        with open(os.path.join(outdir, 'ac3_funcs.h'), 'w') as f:
            f.write('\n'.join(header) + '\n')

        per = (len(bodies) + chunks - 1) // chunks
        for i in range(chunks):
            part = bodies[i * per:(i + 1) * per]
            if not part:
                continue
            with open(os.path.join(outdir, f'ac3_code_{i:02d}.c'), 'w') as f:
                f.write('#include "ac3_funcs.h"\n\n')
                f.write('\n\n'.join('\n'.join(lines) for _, lines in part) + '\n')

        with open(os.path.join(outdir, 'ac3_table.c'), 'w') as f:
            f.write('#include "ac3_funcs.h"\n\n')
            f.write('const struct { u32 addr; ac3_func_t fn; } '
                    'ac3_func_table[] = {\n')
            for s, _ in sorted(self.functions):
                f.write(f'    {{ 0x{s:08X}u, {self.func_names[s]} }},\n')
            f.write('};\n')
            f.write('const unsigned ac3_func_table_len = '
                    'sizeof(ac3_func_table)/sizeof(ac3_func_table[0]);\n')
            f.write(f'unsigned long long ac3_prof'
                    f'[{len(self.functions) if self.profile else 1}];\n')
            f.write(f'const unsigned ac3_prof_len = '
                    f'{len(self.functions) if self.profile else 0};\n')
        return notes, len(bodies)


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 1
    exe, apath, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
    chunks = 16
    if '--chunks' in sys.argv:
        chunks = int(sys.argv[sys.argv.index('--chunks') + 1])

    prog = Program(exe)
    analysis = json.load(open(apath))
    symbols = {}
    spath = None
    if '--symbols' in sys.argv:
        spath = sys.argv[sys.argv.index('--symbols') + 1]
    else:
        guess = os.path.splitext(os.path.basename(exe))[0] + '_psyq_symbols.json'
        if os.path.exists(guess):
            spath = guess
    if spath:
        for e in json.load(open(spath)):
            symbols[e['addr']] = e['name']

    extra = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         'manual_symbols.txt')
    if os.path.exists(extra):
        n = 0
        for line in open(extra):
            line = line.split('#')[0].strip()
            if not line:
                continue
            addr, name = line.split()
            symbols[int(addr, 0)] = name
            n += 1
        if n:
            print(f'manual symbol overrides applied: {n}')

    hle_names = []
    if '--hle' in sys.argv:
        hpath = sys.argv[sys.argv.index('--hle') + 1]
        for line in open(hpath):
            line = line.split('#')[0].strip()
            if line:
                hle_names.append(line)

    rc = Recompiler(prog, analysis, symbols, hle_names,
                    profile='--profile' in sys.argv)
    if '--trace-func' in sys.argv:
        for a in sys.argv[sys.argv.index('--trace-func') + 1].split(','):
            rc.trace_funcs.add(int(a, 0))
    notes, n = rc.run(outdir, chunks)
    if hle_names:
        with open(os.path.join(outdir, 'hle_expected.txt'), 'w') as f:
            for a, nm in rc.hle_used:
                f.write(f'psyq_{nm} 0x{a:08X}\n')
        missing = sorted(set(hle_names) - {nm for _, nm in rc.hle_used})
        print(f'HLE: {len(rc.hle_used)} functions taken native, '
              f'{len(missing)} listed but not present in this image')
    print(f'recompiled {n} functions -> {outdir}')
    print(f'  named from psyq : {sum(1 for s,_ in rc.functions if s in symbols)}')
    for k, v in sorted(notes.items(), key=lambda x: -x[1]):
        print(f'  {k:<24} {v}')
    return 0


if __name__ == '__main__':
    sys.exit(main())

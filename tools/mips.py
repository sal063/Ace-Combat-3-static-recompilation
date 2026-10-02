#!/usr/bin/env python3
import struct
from enum import Enum

REG_NAMES = [
    '$zero', '$at', '$v0', '$v1', '$a0', '$a1', '$a2', '$a3',
    '$t0', '$t1', '$t2', '$t3', '$t4', '$t5', '$t6', '$t7',
    '$s0', '$s1', '$s2', '$s3', '$s4', '$s5', '$s6', '$s7',
    '$t8', '$t9', '$k0', '$k1', '$gp', '$sp', '$fp', '$ra',
]
R_ZERO, R_AT, R_V0, R_A0, R_SP, R_RA = 0, 1, 2, 4, 29, 31

COP0_NAMES = {
    3: 'BPC', 5: 'BDA', 6: 'JUMPDEST', 7: 'DCIC', 8: 'BadVaddr',
    9: 'BDAM', 11: 'BPCM', 12: 'SR', 13: 'Cause', 14: 'EPC', 15: 'PRID',
}

GTE_OPS = {
    0x01: 'RTPS',  0x06: 'NCLIP', 0x0C: 'OP',    0x10: 'DPCS',  0x11: 'INTPL',
    0x12: 'MVMVA', 0x13: 'NCDS',  0x14: 'CDP',   0x16: 'NCDT',  0x1B: 'NCCS',
    0x1C: 'CC',    0x1E: 'NCS',   0x20: 'NCT',   0x28: 'SQR',   0x29: 'DCPL',
    0x2A: 'DPCT',  0x2D: 'AVSZ3', 0x2E: 'AVSZ4', 0x30: 'RTPT',  0x3D: 'GPF',
    0x3E: 'GPL',   0x3F: 'NCCT',
}


class Kind(Enum):
    NORMAL = 0
    BRANCH = 1
    JUMP = 2
    CALL = 3
    JR = 4
    JALR = 5
    TRAP = 6
    INVALID = 7


SPECIAL = {
    0x00: 'sll',  0x02: 'srl',  0x03: 'sra',  0x04: 'sllv', 0x06: 'srlv',
    0x07: 'srav', 0x08: 'jr',   0x09: 'jalr', 0x0C: 'syscall', 0x0D: 'break',
    0x10: 'mfhi', 0x11: 'mthi', 0x12: 'mflo', 0x13: 'mtlo',
    0x18: 'mult', 0x19: 'multu', 0x1A: 'div', 0x1B: 'divu',
    0x20: 'add',  0x21: 'addu', 0x22: 'sub',  0x23: 'subu',
    0x24: 'and',  0x25: 'or',   0x26: 'xor',  0x27: 'nor',
    0x2A: 'slt',  0x2B: 'sltu',
}

REGIMM = {0x00: 'bltz', 0x01: 'bgez', 0x10: 'bltzal', 0x11: 'bgezal'}

OPCODES = {
    0x02: 'j',    0x03: 'jal',  0x04: 'beq',  0x05: 'bne',
    0x06: 'blez', 0x07: 'bgtz', 0x08: 'addi', 0x09: 'addiu',
    0x0A: 'slti', 0x0B: 'sltiu', 0x0C: 'andi', 0x0D: 'ori',
    0x0E: 'xori', 0x0F: 'lui',
    0x20: 'lb',   0x21: 'lh',   0x22: 'lwl',  0x23: 'lw',
    0x24: 'lbu',  0x25: 'lhu',  0x26: 'lwr',
    0x28: 'sb',   0x29: 'sh',   0x2A: 'swl',  0x2B: 'sw',  0x2E: 'swr',
    0x32: 'lwc2', 0x3A: 'swc2',
}

LOADS = {'lb', 'lh', 'lwl', 'lw', 'lbu', 'lhu', 'lwr', 'lwc2'}
STORES = {'sb', 'sh', 'swl', 'sw', 'swr', 'swc2'}
BRANCHES = {'beq', 'bne', 'blez', 'bgtz', 'bltz', 'bgez', 'bltzal', 'bgezal'}


class Insn:
    __slots__ = ('addr', 'word', 'mnemonic', 'kind', 'op', 'rs', 'rt', 'rd',
                 'shamt', 'funct', 'imm', 'target', 'gte')

    def __init__(self, addr, word):
        self.addr, self.word = addr, word
        self.op = word >> 26
        self.rs = (word >> 21) & 0x1F
        self.rt = (word >> 16) & 0x1F
        self.rd = (word >> 11) & 0x1F
        self.shamt = (word >> 6) & 0x1F
        self.funct = word & 0x3F
        self.imm = word & 0xFFFF
        self.target = None
        self.gte = None
        self.mnemonic = None
        self.kind = Kind.INVALID
        self._decode()

    def _decode(self):
        w, op = self.word, self.op
        if op == 0x00:
            m = SPECIAL.get(self.funct)
            if m is None:
                return
            self.mnemonic = m
            if m == 'jr':
                self.kind = Kind.JR
            elif m == 'jalr':
                self.kind = Kind.JALR
            elif m in ('syscall', 'break'):
                self.kind = Kind.TRAP
            else:
                self.kind = Kind.NORMAL
            return

        if op == 0x01:
            m = REGIMM.get(self.rt)
            if m is None:
                return
            self.mnemonic = m
            self.kind = Kind.BRANCH
            self.target = self.branch_target
            return

        if op in (0x02, 0x03):
            self.mnemonic = 'j' if op == 0x02 else 'jal'
            self.kind = Kind.JUMP if op == 0x02 else Kind.CALL
            self.target = ((self.addr + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            return

        if op == 0x10:
            if self.rs == 0x00:
                self.mnemonic, self.kind = 'mfc0', Kind.NORMAL
            elif self.rs == 0x04:
                self.mnemonic, self.kind = 'mtc0', Kind.NORMAL
            elif w & 0x02000000 and self.funct == 0x10:
                self.mnemonic, self.kind = 'rfe', Kind.NORMAL
            return

        if op == 0x12:
            if w & 0x02000000:
                self.mnemonic, self.kind = 'gte', Kind.NORMAL
                self.gte = GTE_OPS.get(self.funct, f'COP2_{self.funct:02X}')
                return
            self.mnemonic = {0x00: 'mfc2', 0x02: 'cfc2',
                             0x04: 'mtc2', 0x06: 'ctc2'}.get(self.rs)
            if self.mnemonic:
                self.kind = Kind.NORMAL
            return

        m = OPCODES.get(op)
        if m is None:
            return
        self.mnemonic = m
        if m in BRANCHES:
            self.kind = Kind.BRANCH
            self.target = self.branch_target
        else:
            self.kind = Kind.NORMAL

    @property
    def simm(self):
        return self.imm - 0x10000 if self.imm & 0x8000 else self.imm

    @property
    def branch_target(self):
        return (self.addr + 4 + (self.simm << 2)) & 0xFFFFFFFF

    @property
    def valid(self):
        return self.mnemonic is not None

    @property
    def has_delay_slot(self):
        return self.kind in (Kind.BRANCH, Kind.JUMP, Kind.CALL,
                             Kind.JR, Kind.JALR)

    @property
    def is_return(self):
        return self.kind is Kind.JR and self.rs == R_RA

    @property
    def is_nop(self):
        return self.word == 0

    @property
    def ends_block(self):
        return self.kind in (Kind.JUMP, Kind.JR, Kind.TRAP)

    def writes_reg(self):
        m = self.mnemonic
        if m is None or self.kind in (Kind.BRANCH, Kind.JUMP, Kind.TRAP):
            return None
        if m in ('jal',):
            return R_RA
        if m == 'jalr':
            return self.rd
        if self.op == 0x00:
            if m in ('mult', 'multu', 'div', 'divu', 'mthi', 'mtlo', 'jr'):
                return None
            return self.rd or None
        if m in ('addi', 'addiu', 'slti', 'sltiu', 'andi', 'ori', 'xori',
                 'lui') or m in LOADS:
            return self.rt or None
        if m in ('mfc0', 'mfc2', 'cfc2'):
            return self.rt or None
        return None

    def text(self, symbols=None):
        def sym(a):
            if symbols and a in symbols:
                return symbols[a]
            return f'0x{a:08X}'
        m, r = self.mnemonic, REG_NAMES
        if m is None:
            return f'.word 0x{self.word:08X}'
        if m == 'nop' or self.is_nop:
            return 'nop'
        if m == 'gte':
            return f'{self.gte}'
        if m in ('j', 'jal'):
            return f'{m} {sym(self.target)}'
        if m == 'jr':
            return f'jr {r[self.rs]}'
        if m == 'jalr':
            return f'jalr {r[self.rd]}, {r[self.rs]}'
        if m in ('beq', 'bne'):
            return f'{m} {r[self.rs]}, {r[self.rt]}, {sym(self.target)}'
        if m in ('blez', 'bgtz', 'bltz', 'bgez', 'bltzal', 'bgezal'):
            return f'{m} {r[self.rs]}, {sym(self.target)}'
        if m == 'lui':
            return f'lui {r[self.rt]}, 0x{self.imm:04X}'
        if m in LOADS or m in STORES:
            return f'{m} {r[self.rt]}, {self.simm}({r[self.rs]})'
        if m in ('addi', 'addiu', 'slti', 'sltiu'):
            return f'{m} {r[self.rt]}, {r[self.rs]}, {self.simm}'
        if m in ('andi', 'ori', 'xori'):
            return f'{m} {r[self.rt]}, {r[self.rs]}, 0x{self.imm:04X}'
        if m in ('sll', 'srl', 'sra'):
            return f'{m} {r[self.rd]}, {r[self.rt]}, {self.shamt}'
        if m in ('sllv', 'srlv', 'srav'):
            return f'{m} {r[self.rd]}, {r[self.rt]}, {r[self.rs]}'
        if m in ('mult', 'multu', 'div', 'divu'):
            return f'{m} {r[self.rs]}, {r[self.rt]}'
        if m in ('mfhi', 'mflo'):
            return f'{m} {r[self.rd]}'
        if m in ('mthi', 'mtlo'):
            return f'{m} {r[self.rs]}'
        if m in ('mfc0', 'mtc0'):
            return f'{m} {r[self.rt]}, {COP0_NAMES.get(self.rd, f"${self.rd}")}'
        if m in ('mfc2', 'mtc2', 'cfc2', 'ctc2'):
            return f'{m} {r[self.rt]}, ${self.rd}'
        if m == 'rfe':
            return 'rfe'
        if m in ('syscall', 'break'):
            return m
        return f'{m} {r[self.rd]}, {r[self.rs]}, {r[self.rt]}'

    def __repr__(self):
        return f'<{self.addr:08X}: {self.text()}>'


def decode(addr, word):
    return Insn(addr, word)


def decode_range(base, data):
    n = len(data) // 4
    words = struct.unpack('<%dI' % n, data[:n * 4])
    return [Insn(base + i * 4, w) for i, w in enumerate(words)]

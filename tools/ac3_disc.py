#!/usr/bin/env python3
"""
ac3_disc.py -- ISO9660 reader / extractor for Ace Combat 3 MODE2/2352 disc images.

Usage:
    python tools/ac3_disc.py list    <image.bin>
    python tools/ac3_disc.py extract <image.bin> <name> <outfile>
    python tools/ac3_disc.py dump    <image.bin> <outdir> [--all]
    python tools/ac3_disc.py exeinfo <file>

Without --all, `dump` skips ACE.BPB / ACE.SPB (the ~100MB / ~530MB asset packs).
"""
import sys, os, struct

RAW_SECTOR = 2352
USER_OFFSET = 24
USER_SIZE = 2048
BIG_FILES = {'ACE.BPB', 'ACE.SPB'}


class Disc:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.f.seek(0, os.SEEK_END)
        self.total_sectors = self.f.tell() // RAW_SECTOR

    def sector(self, lba):
        self.f.seek(lba * RAW_SECTOR + USER_OFFSET)
        return self.f.read(USER_SIZE)

    def read(self, lba, nbytes):
        buf = bytearray()
        while len(buf) < nbytes:
            buf += self.sector(lba)
            lba += 1
        return bytes(buf[:nbytes])

    def copy_to(self, lba, nbytes, fileobj, chunk_sectors=512):
        remaining = nbytes
        while remaining > 0:
            n = min(chunk_sectors, (remaining + USER_SIZE - 1) // USER_SIZE)
            buf = bytearray()
            for i in range(n):
                buf += self.sector(lba + i)
            take = min(remaining, len(buf))
            fileobj.write(buf[:take])
            remaining -= take
            lba += n

    def volume_id(self):
        return self.sector(16)[40:72].decode('latin-1').strip()


def parse_dir_records(data):
    recs, off = [], 0
    while off < len(data):
        length = data[off]
        if length == 0:
            off = (off // USER_SIZE + 1) * USER_SIZE
            if off >= len(data):
                break
            continue
        r = data[off:off + length]
        lba = struct.unpack_from('<I', r, 2)[0]
        size = struct.unpack_from('<I', r, 10)[0]
        flags = r[25]
        namelen = r[32]
        recs.append((r[33:33 + namelen], lba, size, flags))
        off += length
    return recs


def walk(disc, lba, size, path, out):
    for name, elba, esize, flags in parse_dir_records(disc.read(lba, size)):
        if name in (b'\x00', b'\x01'):
            continue
        nm = name.decode('latin-1').split(';')[0]
        full = f'{path}/{nm}'
        is_dir = bool(flags & 0x02)
        out.append((full, elba, esize, is_dir))
        if is_dir:
            walk(disc, elba, esize, full, out)
    return out


def listing(disc):
    pvd = disc.sector(16)
    root = pvd[156:190]
    root_lba = struct.unpack_from('<I', root, 2)[0]
    root_size = struct.unpack_from('<I', root, 10)[0]
    return walk(disc, root_lba, root_size, '', [])


PSX_EXE_HEADER = 0x800


def exeinfo(path):
    with open(path, 'rb') as f:
        head = f.read(PSX_EXE_HEADER)
    if head[:8] != b'PS-X EXE':
        return None
    pc0, gp0, t_addr, t_size, d_addr, d_size, b_addr, b_size, s_addr, s_size = \
        struct.unpack_from('<10I', head, 0x10)
    return dict(pc0=pc0, gp0=gp0, t_addr=t_addr, t_size=t_size,
                d_addr=d_addr, d_size=d_size, b_addr=b_addr, b_size=b_size,
                s_addr=s_addr, s_size=s_size,
                region=head[0x4C:0x80].split(b'\0')[0].decode('latin-1'))


def print_exeinfo(path):
    info = exeinfo(path)
    if not info:
        print(f'{path}: not a PS-EXE')
        return
    size = os.path.getsize(path)
    print(f'{path}')
    print(f'  entry point   0x{info["pc0"]:08X}   gp0 0x{info["gp0"]:08X}')
    print(f'  text          0x{info["t_addr"]:08X} .. 0x{info["t_addr"]+info["t_size"]:08X}'
          f'  ({info["t_size"]} bytes)')
    print(f'  stack         0x{info["s_addr"]:08X}   size 0x{info["s_size"]:X}')
    print(f'  region        {info["region"]}')
    print(f'  file size     {size}  (header 0x800 + {size - PSX_EXE_HEADER} body)')
    if info['t_size'] != size - PSX_EXE_HEADER:
        print(f'  !! t_size disagrees with file body length')


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    cmd = sys.argv[1]

    if cmd == 'exeinfo':
        for p in sys.argv[2:]:
            print_exeinfo(p)
        return 0

    disc = Disc(sys.argv[2])
    print(f'volume "{disc.volume_id()}"  ({disc.total_sectors} sectors)')
    entries = listing(disc)

    if cmd == 'extract':
        if len(sys.argv) < 5:
            print(__doc__)
            return 1
        name, dest = sys.argv[3].upper(), sys.argv[4]
        for full, lba, size, is_dir in entries:
            if is_dir or os.path.basename(full).upper() != name:
                continue
            os.makedirs(os.path.dirname(dest) or '.', exist_ok=True)
            with open(dest, 'wb') as fo:
                disc.copy_to(lba, size, fo)
            print(f'  wrote {name} ({size} bytes) -> {dest}')
            return 0
        print(f'  {name} is not on this disc')
        return 1

    if cmd == 'list':
        for full, lba, size, is_dir in entries:
            print(f'{"D" if is_dir else "F"} lba={lba:<8} size={size:<12} {full}')
        return 0

    if cmd == 'dump':
        outdir = sys.argv[3]
        want_all = '--all' in sys.argv
        for full, lba, size, is_dir in entries:
            dest = os.path.join(outdir, full.lstrip('/').replace('/', os.sep))
            if is_dir:
                os.makedirs(dest, exist_ok=True)
                continue
            base = os.path.basename(full)
            if base in BIG_FILES and not want_all:
                print(f'  skip  {base}  ({size} bytes, use --all)')
                continue
            os.makedirs(os.path.dirname(dest) or '.', exist_ok=True)
            with open(dest, 'wb') as fo:
                disc.copy_to(lba, size, fo)
            print(f'  wrote {base:<16} {size:>10} bytes -> {dest}')
        return 0

    print(__doc__)
    return 1


if __name__ == '__main__':
    sys.exit(main())

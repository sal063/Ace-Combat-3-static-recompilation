#!/usr/bin/env python3
"""
embed_spv.py -- turn compiled SPIR-V into a C header.

Keeps the shaders inside the executable so there is no runtime file lookup and
no way for the binary and its shaders to drift apart.

Usage:
    python tools/embed_spv.py <out.h> <name>=<file.spv> [<name>=<file.spv> ...]
"""
import sys, os


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    out = sys.argv[1]
    parts = []
    for arg in sys.argv[2:]:
        name, path = arg.split('=', 1)
        data = open(path, 'rb').read()
        if len(data) % 4:
            print(f'{path}: not a multiple of 4 bytes, not valid SPIR-V')
            return 1
        words = [int.from_bytes(data[i:i + 4], 'little')
                 for i in range(0, len(data), 4)]
        if words[0] != 0x07230203:
            print(f'{path}: bad SPIR-V magic 0x{words[0]:08X}')
            return 1
        body = []
        for i in range(0, len(words), 8):
            body.append('    ' + ' '.join(f'0x{w:08x},' for w in words[i:i + 8]))
        parts.append(f'static const uint32_t {name}[] = {{\n' +
                     '\n'.join(body) + '\n};\n')

    with open(out, 'w') as f:
        guard = os.path.basename(out).upper().replace('.', '_')
        f.write(f'#ifndef {guard}\n#define {guard}\n')
        f.write('#include <stdint.h>\n\n')
        f.write('\n'.join(parts))
        f.write('\n#endif\n')
    print(f'wrote {out} ({len(parts)} shaders)')
    return 0


if __name__ == '__main__':
    sys.exit(main())

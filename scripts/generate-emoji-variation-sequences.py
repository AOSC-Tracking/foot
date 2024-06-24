#!/usr/bin/env python3

import argparse
import sys


class Codepoint:
    def __init__(self, start: int, end: None|int = None):
        self.start = start
        self.end = start if end is None else end
        self.vs15 = False
        self.vs16 = False

    def __repr__(self) -> str:
        return f'{self.start:x}-{self.end:x}, vs15={self.vs15}, vs16={self.vs16}'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('input', type=argparse.FileType('r'))
    parser.add_argument('output', type=argparse.FileType('w'))
    opts = parser.parse_args()

    codepoints: list[Codepoint] = []

    for line in opts.input:
        line = line.rstrip()
        if not line:
            continue
        if line[0] == '#':
            continue

        cp, vs, _ = line.split(' ', maxsplit=2)
        cp = int(cp, 16)
        vs = int(vs, 16)

        assert vs == 0xfe0e or vs == 0xfe0f

        if len(codepoints) == 0 or codepoints[-1].start != cp:
            codepoints.append(Codepoint(cp))
        else:
            assert codepoints[-1].start == cp

        if vs == 0xfe0e:
            codepoints[-1].vs15 = True
        else:
            codepoints[-1].vs16 = True


    compacted_codepoints: list[Codepoint] = []
    for i, cp in enumerate(codepoints):
        assert cp.end == cp.start

        if i == 0:
            compacted_codepoints.append(cp)
            continue

        last_cp = compacted_codepoints[-1]
        if last_cp.end == cp.start - 1 and last_cp.vs15 == cp.vs15 and last_cp.vs16 == cp.vs16:
            compacted_codepoints[-1].end = cp.start
        else:
            compacted_codepoints.append(cp)

    opts.output.write('#pragma once\n')
    opts.output.write('#include <stdint.h>\n')
    opts.output.write('#include <stdbool.h>\n')
    opts.output.write('\n')
    opts.output.write('struct emoji_vs {\n')
    opts.output.write('    uint32_t start;\n')
    opts.output.write('    uint32_t end;\n')
    opts.output.write('    bool vs15:1;\n')
    opts.output.write('    bool vs16:1;\n')
    opts.output.write('} __attribute__((packed));\n')
    opts.output.write('_Static_assert(sizeof(struct emoji_vs) == 9, "unexpected struct size");\n')
    opts.output.write('\n')
    opts.output.write('#if defined(FOOT_GRAPHEME_CLUSTERING)\n')
    opts.output.write('\n')

    opts.output.write(f'static const struct emoji_vs emoji_vs[{len(compacted_codepoints)}] = {{\n')

    for cp in compacted_codepoints:
        opts.output.write('    {\n')
        opts.output.write(f'        .start = 0x{cp.start:X},\n')
        opts.output.write(f'        .end = 0x{cp.end:x},\n')
        opts.output.write(f'        .vs15 = {"true" if cp.vs15 else "false"},\n')
        opts.output.write(f'        .vs16 = {"true" if cp.vs16 else "false"},\n')
        opts.output.write('    },\n')

    opts.output.write('};\n')
    opts.output.write('\n')
    opts.output.write('#endif  /* FOOT_GRAPHEME_CLUSTERING */\n')


if __name__ == '__main__':
    sys.exit(main())

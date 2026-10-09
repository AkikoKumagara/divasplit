#!/usr/bin/env python3
"""Build tools/patterns.json from the EU dumps (maintainer tool).

    mkpatterns.py X=divax.elf F=divaf.elf F2ND=divaf2nd.elf

For every line of the vitagrafix/*.txt @FPS blocks, and every address the
plugin needs, it takes the instructions around the site, masks what differs
between builds (branch targets, literal loads, address immediates) and grows
the window until the pattern is unique in that dump. port.py then finds the
same code in other releases of the game.
"""
import json
import os
import re
import sys

from thumbelf import VitaElf, find_all, insn_mask

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FAMILIES = {
    'X':    {'patch': 'vitagrafix/divax.txt'},
    'F':    {'patch': 'vitagrafix/divaf.txt'},
    'F2ND': {'patch': 'vitagrafix/divaf2nd.txt'},
}

# plugin anchors (EU seg0 offsets) and what port.py reads at them
PLUGIN = {
    'X': {
        'render_stage': {'site': 0x271368, 'func': True},
        # movw/movt r0 = &fm_ptr; ldr r4, [r0] ... ldrb.w r0, [r4, #req]; strb.w r0, [r4, #flag]
        'fm_ptr': {'site': 0x42368, 'movw_at': 0, 'ldrb_at': 0x28},
        # RenderStage: movw/movt r1 = &stage_table_ptr
        'stages': {'site': 0x271380, 'movw_at': 0},
        'consts': {'fm_is_ptr': 1, 'stages_is_ptr': 1, 'has_profiler': 1,
                   'stage_3d': 3, 'stage_fx': 2, 'stage_setup': 0, 'prof_flag': 0x0C},
    },
    'F': {
        'render_stage': {'site': 0x28C54E, 'func': True},
        # GetDelta: movw/movt r0 = &fm
        'fm': {'site': 0x13D00, 'movw_at': 0, 'func': True},
        # fm update: ldrb.w r0, [r4, #req] ... strb r0, [r4, #0x18]
        'fm_req': {'site': 0x13E46, 'ldrb_at': 0},
        'stages': {'site': 0x28C552, 'movw_at': 0, 'movt_at': 6},
        'consts': {'fm_is_ptr': 0, 'stages_is_ptr': 0, 'has_profiler': 0,
                   'stage_3d': 8, 'stage_fx': 7, 'stage_setup': 5, 'prof_flag': 0x18},
    },
    'F2ND': {
        'render_stage': {'site': 0x265920, 'func': True},
        'fm': {'site': 0xAE7A, 'movw_at': 0, 'func': True},
        'fm_req': {'site': 0xAF5C, 'ldrb_at': 0},
        'stages': {'site': 0x265932, 'movw_at': 0},
        'consts': {'fm_is_ptr': 0, 'stages_is_ptr': 0, 'has_profiler': 0,
                   'stage_3d': 3, 'stage_fx': 2, 'stage_setup': 0, 'prof_flag': 0x0C},
    },
}

LINE_RX = re.compile(r'^(\d+):0x([0-9A-Fa-f]+)\s+(.*)$')


def patch_size(expr):
    name = expr.split('(')[0].strip()
    if name == 't1_mov':
        return 2
    if name in ('t2_mov', 't2_vmov', 't1_movt', 'uint32'):
        return 4
    if name == 'mov32':
        gap = int(expr.rstrip(')').split(',')[-1].strip())
        return 8 + gap
    if name == 'bytes':
        return len(re.search(r'bytes\(([0-9A-Fa-f]+)\)', expr).group(1)) // 2
    raise ValueError(f'unknown patch function in: {expr}')


def read_fps_block(path):
    """[(comment_lines, offset, expr)] of the @FPS section."""
    out, comments, in_fps = [], [], False
    for line in open(os.path.join(ROOT, path)):
        line = line.rstrip('\n')
        if line.startswith('@'):
            in_fps = line.strip() == '@FPS'
            continue
        if not in_fps:
            continue
        if line.startswith('#'):
            comments.append(line)
            continue
        m = LINE_RX.match(line)
        if m:
            out.append((comments, int(m.group(2), 16), m.group(3)))
            comments = []
    return out


def build_pattern(elf, site, size, func=False):
    """Masked pattern around site, grown until unique. Returns dict or None."""
    starts = elf.insn_starts()
    if not starts[site]:
        raise ValueError(f'0x{site:X} is not an instruction start')
    before, after = [], []
    # instructions after the site (including the patched ones)
    off = site
    while len(after) < 40 and off < len(elf.text):
        sz = starts[off]
        if not sz:
            break
        after.append((off, sz))
        off += sz
    off = site
    while len(before) < 24 and off > 0 and not func:
        prev = off - 2
        while prev >= off - 4 and not starts[prev]:
            prev -= 2
        if prev < off - 4 or starts[prev] != off - prev:
            break
        before.insert(0, (prev, starts[prev]))
        off = prev

    def window(nb, na):
        insns = before[len(before) - nb:] + after[:na]
        vals, masks = bytearray(), bytearray()
        for o, sz in insns:
            code = elf.text[o:o + sz]
            inside = site <= o < site + size
            vals += code
            masks += insn_mask(code, keep_imm=inside)
        return insns[0][0], bytes(vals), bytes(masks)

    # the window always covers the whole patched range
    min_after = 0
    while min_after < len(after) and after[min_after][0] < site + size:
        min_after += 1
    for total in range(max(4, min_after), 60):
        nb = 0 if func else min(len(before), total // 2)
        na = min(len(after), max(min_after, total - nb))
        start, vals, masks = window(nb, na)
        hits = find_all(elf.text, vals, masks)
        if hits == [start]:
            return {'bytes': vals.hex(), 'mask': masks.hex(), 'site_at': site - start}
    # identical code exists more than once (duplicated functions): remember
    # which copy it is; port.py takes the same copy if the count matches
    if start in hits:
        return {'bytes': vals.hex(), 'mask': masks.hex(), 'site_at': site - start,
                'nth': hits.index(start), 'count': len(hits)}
    return None


def main():
    elfs = dict(a.split('=', 1) for a in sys.argv[1:])
    result = {}
    for fam, cfg in FAMILIES.items():
        elf = VitaElf(elfs[fam])
        fam_out = {'eu_nid': f'0x{elf.module_nid:08X}', 'lines': [], 'plugin': {}}
        for comments, off, expr in read_fps_block(cfg['patch']):
            pat = build_pattern(elf, off, patch_size(expr))
            if pat is None:
                print(f'{fam} 0x{off:X}: no unique pattern', file=sys.stderr)
                continue
            fam_out['lines'].append({'comments': comments, 'eu_offset': off, 'expr': expr, **pat})
        for name, a in PLUGIN[fam].items():
            if name == 'consts':
                fam_out['plugin']['consts'] = a
                continue
            pat = build_pattern(elf, a['site'], 8, func=a.get('func', False))
            if pat is None:
                print(f'{fam} plugin {name}: no unique pattern', file=sys.stderr)
                continue
            fam_out['plugin'][name] = {**{k: v for k, v in a.items() if k != 'func'}, **pat}
        result[fam] = fam_out
        print(f'{fam}: {len(fam_out["lines"])} lines, plugin anchors {sorted(k for k in fam_out["plugin"] if k != "consts")}')
    json.dump(result, open(os.path.join(ROOT, 'tools', 'patterns.json'), 'w'), indent=1)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Port the DIVA 60 fps VitaGrafix patch and the divasplit addresses to another
release (US / JP / ASIA ...) of Project DIVA X, f or f 2nd.

    pip install capstone
    python3 port.py eboot.bin.elf --title PCSE00434

eboot.bin.elf is the decrypted eboot of YOUR copy (VitaShell / FAGDec dump).
Nothing from the game is uploaded or needed besides that file. The script finds
every EU patch site by the code around it (patterns.json) and prints:
  - the @FPS block for ux0:data/VitaGrafix/patchlist.txt
  - the GAMES[] line for src/main.c in divasplit
Sites it can't find for certain are listed and left out - test before sharing.
"""
import argparse
import json
import os
import sys

from thumbelf import VitaElf, decode_ldrb_imm, decode_movw_movt, find_all

HERE = os.path.dirname(os.path.abspath(__file__))
GAME_ENUM = {'X': 'GAME_X', 'F': 'GAME_F', 'F2ND': 'GAME_F2ND'}
GAME_NAME = {'X': 'Project Diva X', 'F': 'Project Diva f', 'F2ND': 'Project Diva f 2nd'}


def locate(elf, pat):
    """seg0 offset of the pattern's site, or (None, reason)."""
    vals, masks = bytes.fromhex(pat['bytes']), bytes.fromhex(pat['mask'])
    hits = find_all(elf.text, vals, masks)
    if 'nth' in pat:
        if len(hits) != pat['count']:
            return None, f'{len(hits)} copies, expected {pat["count"]}'
        return hits[pat['nth']] + pat['site_at'], None
    if len(hits) != 1:
        return None, 'not found' if not hits else f'{len(hits)} matches'
    return hits[0] + pat['site_at'], None


def port_family(elf, fam):
    lines, missing = [], []
    for l in fam['lines']:
        off, why = locate(elf, l)
        if off is None:
            missing.append((l, why))
        else:
            lines.append((l, off))
    return lines, missing


def plugin_entry(elf, fam_name, plugin):
    c = plugin['consts']
    found = {}
    for name, pat in plugin.items():
        if name == 'consts':
            continue
        off, why = locate(elf, pat)
        if off is None:
            return None, f'{name}: {why}'
        found[name] = (off, pat)
    t = elf.text

    def movw_movt(name):
        off, pat = found[name]
        w = off + pat.get('movw_at', 0)
        m = off + pat.get('movt_at', pat.get('movw_at', 0) + 4)
        return decode_movw_movt(t[w:w + 4] + t[m:m + 4])

    seg1 = elf.seg1_va
    if fam_name == 'X':
        fm = movw_movt('fm_ptr') - seg1
        off, pat = found['fm_ptr']
        req = decode_ldrb_imm(t[off + pat['ldrb_at']:off + pat['ldrb_at'] + 4])
    else:
        fm = movw_movt('fm') - seg1
        off, pat = found['fm_req']
        req = decode_ldrb_imm(t[off + pat['ldrb_at']:off + pat['ldrb_at'] + 4])
    stages = movw_movt('stages') - seg1
    rs = found['render_stage'][0]
    entry = (f'{{0x{elf.module_nid:08X}, {GAME_ENUM[fam_name]}, 0x{rs:X}, 0x{fm:X}, {c["fm_is_ptr"]}, '
             f'0x{req:X}, 0x{stages:X}, {c["stages_is_ptr"]}, {c["has_profiler"]}, '
             f'{c["stage_3d"]}, {c["stage_fx"]}, {c["stage_setup"]}, 0x{c["prof_flag"]:02X}}},')
    return entry, None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('elf', help='decrypted eboot.bin (ELF)')
    ap.add_argument('--title', default='TITLEID', help='title ID of this release, e.g. PCSE00434')
    ap.add_argument('--game', choices=['X', 'F', 'F2ND'], help='skip auto-detection')
    args = ap.parse_args()

    patterns = json.load(open(os.path.join(HERE, 'patterns.json')))
    elf = VitaElf(args.elf)
    print(f'# module {elf.module_name}, NID 0x{elf.module_nid:08X}', file=sys.stderr)

    results = {}
    for name in ([args.game] if args.game else patterns):
        results[name] = port_family(elf, patterns[name])
    best = max(results, key=lambda n: len(results[n][0]) / len(patterns[n]['lines']))
    lines, missing = results[best]
    total = len(patterns[best]['lines'])
    print(f'# detected {GAME_NAME[best]}: {len(lines)}/{total} patch sites found', file=sys.stderr)
    if len(lines) < total * 0.8:
        print('# fewer than 80% of the sites match: this is probably a different game or a very '
              'different build. Not printing a patch.', file=sys.stderr)
        sys.exit(1)

    print(f'# Hatsune Miku: {GAME_NAME[best]} [{args.title}], 60 fps ported from EU by divasplit/tools/port.py')
    print(f'# {len(lines)}/{total} sites found; untested until someone plays it')
    print(f'[{args.title}, eboot.bin, 0x{elf.module_nid:08X}]')
    print('@FPS')
    for l, off in lines:
        for c in l['comments']:
            print(c)
        print(f'0:0x{off:X} {l["expr"]}')
    for l, why in missing:
        print(f'# NOT PORTED (EU 0x{l["eu_offset"]:X} {l["expr"]}): {why}')

    entry, why = plugin_entry(elf, best, patterns[best]['plugin'])
    print()
    if entry:
        print('// divasplit src/main.c GAMES[]:')
        print('//   ' + entry)
    else:
        print(f'// divasplit: plugin addresses not found ({why})')


if __name__ == '__main__':
    main()

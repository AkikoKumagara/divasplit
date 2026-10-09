#!/usr/bin/env python3
"""Compare OPT on and off from an OPT A/B recording (log.csv).

    python3 abcompare.py log.csv [--rec N] [--cols cpu,3d,fps,...]

With OPT set to A/B, divasplit flips the optimizations every 1 s window while
recording, and log.csv's opt column says which state each window ran in. A
song's load changes from second to second, so this compares each window with
its neighbours (the mean of the window before and after, which ran in the
other state) and averages those paired differences; their spread gives the
error bar. Plain means per state are printed too.

log.csv holds every recording; --rec picks one (1 = first, -1 = last, the
default).
"""
import argparse
import math
import sys


def recordings(path):
    recs, cur, header = [], None, None
    for line in open(path, encoding='latin1'):
        line = line.strip()
        if not line:
            continue
        if line.startswith('#'):
            cur, header = None, None
            continue
        if header is None:
            header = line.split(',')
            cur = []
            recs.append((header, cur))
            continue
        cur.append(dict(zip(header, line.split(','))))
    return recs


def num(row, col):
    try:
        return float(row[col])
    except (KeyError, ValueError):
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log')
    ap.add_argument('--rec', type=int, default=-1, help='recording number (1 = first, -1 = last)')
    ap.add_argument('--cols', default='fps,cpu,3d,gpu,vs,late,main_run,main_ready,main_wait')
    ap.add_argument('--skip', type=int, default=2, help='windows to drop at the start (loading)')
    args = ap.parse_args()

    recs = [r for r in recordings(args.log) if 'opt' in r[0]]
    if not recs:
        sys.exit('no recording with an opt column')
    header, rows = recs[args.rec if args.rec < 0 else args.rec - 1]
    rows = rows[args.skip:]
    cols = [c for c in args.cols.split(',') if c in header]
    opts = [num(r, 'opt') for r in rows]
    n_on, n_off = opts.count(1.0), opts.count(0.0)
    print(f'{len(recs)} recordings with an opt column; this one: {len(rows)} windows '
          f'({n_on} on, {n_off} off), game {rows[0].get("game") if rows else "?"}')
    if not n_on or not n_off:
        sys.exit('needs windows with OPT both on and off (OPT A/B while recording)')

    print(f'\n{"":10} {"off":>8} {"on":>8} {"on-off":>8} {"paired":>8} {"+-":>6}  n')
    for c in cols:
        vals = [num(r, c) for r in rows]
        off = [v for v, o in zip(vals, opts) if o == 0 and v is not None]
        on = [v for v, o in zip(vals, opts) if o == 1 and v is not None]
        # each window against the mean of its two neighbours in the other state
        diffs = []
        for i in range(1, len(rows) - 1):
            a, b, x = vals[i - 1], vals[i + 1], vals[i]
            if None in (a, b, x) or opts[i] not in (0, 1) or opts[i - 1] == opts[i] or opts[i + 1] == opts[i]:
                continue
            d = x - (a + b) / 2
            diffs.append(d if opts[i] == 1 else -d)
        mo, mn = sum(off) / len(off), sum(on) / len(on)
        if len(diffs) > 1:
            m = sum(diffs) / len(diffs)
            sd = math.sqrt(sum((d - m) ** 2 for d in diffs) / (len(diffs) - 1))
            # neighbouring differences share windows: about half as many independent samples
            se = sd / math.sqrt(len(diffs) / 2)
            print(f'{c:10} {mo:8.2f} {mn:8.2f} {mn - mo:+8.2f} {m:+8.2f} {se:6.2f}  {len(diffs)}')
        else:
            print(f'{c:10} {mo:8.2f} {mn:8.2f} {mn - mo:+8.2f}')
    print('\npaired = mean of (on - neighbours off); +- is about one standard error.'
          ' cpu / 3d are ms per frame (lower is better).')


if __name__ == '__main__':
    main()

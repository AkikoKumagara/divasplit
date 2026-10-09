#!/usr/bin/env python3
"""Turn a divasplit recording into a timeline for https://ui.perfetto.dev
(or chrome://tracing).

    python3 perfetto.py prof.bin trace.bin eboot.bin.elf -o timeline.json [--from 20 --to 30]

prof.bin (profiler, every game thread) becomes a flame chart per thread: what
each thread was running, and where it waited, sampled ~500 times a second.
trace.bin (tracer) adds exact rows: every blocking call that took 20 us or
more (locks, waits, sleeps, file reads, GPU / vblank waits) under its thread,
and the main thread's frames and render stages. Both use the same clock.

Times are seconds from the start of the recording. A whole song makes a large
file; --from / --to cut a window, e.g. around a frame drop.
"""
import argparse
import bisect
import json
import struct
import sys

from profview import Code, load, F_USER, F_REGS, F_SYSCALL

TRACE_MAGIC = 0x43525444
REC = struct.Struct('<IIIIIHH')   # t_us, dur_us, thid, arg, caller, func, seq
MAX_DEPTH = 24
PID = 1


def wait_tid(thid):
    """Row id for a thread's traced calls, apart from all thread ids (0x4xxxxxxx)."""
    return (thid & 0x0FFFFFFF) | 0x10000000


def load_trace(path):
    data = open(path, 'rb').read()
    magic, version, header_size, rec_size, start_lo, start_hi, nfuncs = struct.unpack_from('<7I', data, 0)
    if magic != TRACE_MAGIC:
        sys.exit(f'{path}: not a divasplit trace')
    names = [data[64 + 32 * i:64 + 32 * (i + 1)].split(b'\0')[0].decode('latin1') for i in range(nfuncs)]
    recs = [REC.unpack_from(data, off) for off in range(header_size, len(data) - rec_size + 1, rec_size)]
    return (start_hi << 32) | start_lo, names, recs


def prof_header(path):
    data = open(path, 'rb').read(64 + 32 * 32 + 4 * 32)
    h = struct.unpack_from('<16I', data, 0)
    version, nthreads = h[1], h[9]
    start = (h[11] << 32) | h[10] if version >= 5 else 0
    thids = list(struct.unpack_from(f'<{nthreads}i', data, 64 + 32 * 32)) if version >= 5 else []
    return start, thids


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('prof')
    ap.add_argument('trace')
    ap.add_argument('elf')
    ap.add_argument('-o', '--out', default='timeline.json')
    ap.add_argument('--from', dest='t_from', type=float, default=0.0, help='seconds')
    ap.add_argument('--to', dest='t_to', type=float, default=1e9, help='seconds')
    args = ap.parse_args()

    hdr, samples = load(args.prof)
    prof_start, thids = prof_header(args.prof)
    trace_start, funcs, recs = load_trace(args.trace)
    code = Code(args.elf)
    base, size = hdr['text_base'], hdr['text_size']
    names = hdr['threads']
    if not thids:
        sys.exit('prof.bin is older than version 5 (no thread ids)')

    zero = min(prof_start, trace_start)
    lo, hi = args.t_from * 1e6, args.t_to * 1e6
    events = []

    def fname(f):
        return code.name(f) if not isinstance(f, str) else f

    def returns(lr, stack):
        out = []
        for r in (lr,) + tuple(stack):
            off = (r & ~1) - base
            if 0 <= off < size and r & 1 and code.is_return(off):
                out.append(off)
        return out

    def frames_of(s):
        """Call stack of a sample, outermost first."""
        t, frame, stage, flags, n, pc, lr, sp, cpsr, kpc, kcpsr, cost, stack, thread, weight = s
        if not flags & F_REGS:
            return ['[unknown]']
        rets = returns(lr, stack)
        callers = [fname(code.func(off - 2)) for off in rets]
        callers.reverse()
        if flags & F_SYSCALL:
            name = code.call_target(rets[0]) if rets else None
            leaf = f'[wait] {name}' if name else '[wait]'
        elif flags & F_USER:
            off = pc - base
            if 0 <= off < size:
                leaf = fname(code.func(off))
            else:
                name = code.call_target(rets[0]) if rets else None
                leaf = f'[lib] {name}' if name else '[lib]'
        else:
            leaf = '[kernel]'
        # drop a repeated innermost caller (the leaf's own function on the stack)
        stack_names = callers[-(MAX_DEPTH - 1):] + [leaf]
        dedup = []
        for x in stack_names:
            if not dedup or dedup[-1] != x:
                dedup.append(x)
        return dedup

    # thread rows
    for i, (name, thid) in enumerate(zip(names, thids)):
        events.append({'ph': 'M', 'pid': PID, 'tid': thid, 'name': 'thread_name', 'args': {'name': name}})
        events.append({'ph': 'M', 'pid': PID, 'tid': thid, 'name': 'thread_sort_index', 'args': {'sort_index': 10 + 2 * i}})
        events.append({'ph': 'M', 'pid': PID, 'tid': wait_tid(thid), 'name': 'thread_name', 'args': {'name': f'{name} - waits'}})
        events.append({'ph': 'M', 'pid': PID, 'tid': wait_tid(thid), 'name': 'thread_sort_index', 'args': {'sort_index': 11 + 2 * i}})
    events.append({'ph': 'M', 'pid': PID, 'tid': 1, 'name': 'thread_name', 'args': {'name': 'frames'}})
    events.append({'ph': 'M', 'pid': PID, 'tid': 1, 'name': 'thread_sort_index', 'args': {'sort_index': 0}})
    events.append({'ph': 'M', 'pid': PID, 'tid': 2, 'name': 'thread_name', 'args': {'name': 'render stages'}})
    events.append({'ph': 'M', 'pid': PID, 'tid': 2, 'name': 'thread_sort_index', 'args': {'sort_index': 1}})
    events.append({'ph': 'M', 'pid': PID, 'name': 'process_name', 'args': {'name': 'divasplit recording'}})

    # flame chart per thread from the samples
    period = (samples[-1][0] - samples[0][0]) / max(1, len({s[0] for s in samples}) - 1)
    by_thread = {}
    for s in samples:
        by_thread.setdefault(s[13], []).append(s)
    for t, ss in by_thread.items():
        tid = thids[t]
        open_ = []   # [name, start]
        last_ts = None
        for s in ss:
            ts = prof_start - zero + s[0]
            if ts > hi:
                break
            new = frames_of(s)
            k = 0
            while k < len(open_) and k < len(new) and open_[k][0] == new[k]:
                k += 1
            for name, start in reversed(open_[k:]):
                if ts > lo:
                    events.append({'ph': 'X', 'pid': PID, 'tid': tid, 'name': name,
                                   'ts': max(start, lo), 'dur': ts - max(start, lo)})
            open_ = open_[:k] + [[n, ts] for n in new[k:]]
            last_ts = ts
        if last_ts is not None:
            end = min(last_ts + period, hi)
            for name, start in reversed(open_):
                if end > lo:
                    events.append({'ph': 'X', 'pid': PID, 'tid': tid, 'name': name,
                                   'ts': max(start, lo), 'dur': end - max(start, lo)})

    # exact rows from the tracer
    unknown = set()
    known = set(thids)
    for t_us, dur, thid, arg, caller, func, seq in recs:
        ts = trace_start - zero + t_us
        if ts + dur < lo or ts > hi:
            continue
        name = funcs[func] if func < len(funcs) else f'func {func}'
        if func == 0:
            events.append({'ph': 'X', 'pid': PID, 'tid': 1, 'name': f'frame {arg}', 'ts': ts, 'dur': dur,
                           'args': {'ms': round(dur / 1000, 2)}})
            continue
        if func == 1:
            events.append({'ph': 'X', 'pid': PID, 'tid': 2, 'name': f'stage {arg}', 'ts': ts, 'dur': dur})
            continue
        off = (caller & ~1) - base
        where = fname(code.func(off)) if 0 <= off < size else f'0x{caller:08X}'
        if thid not in known:
            unknown.add(thid)
        events.append({'ph': 'X', 'pid': PID, 'tid': wait_tid(thid), 'name': name, 'ts': ts, 'dur': dur,
                       'args': {'arg': f'0x{arg:08X}', 'called from': where}})
    for thid in unknown:
        events.append({'ph': 'M', 'pid': PID, 'tid': wait_tid(thid), 'name': 'thread_name',
                       'args': {'name': f'thread {thid:08X} - waits'}})

    events.sort(key=lambda e: (e.get('ts', -1), -e.get('dur', 0)))
    json.dump({'traceEvents': events, 'displayTimeUnit': 'ms'}, open(args.out, 'w'))
    spans = [e for e in events if e['ph'] == 'X']
    print(f'{args.out}: {len(spans)} slices, {len(by_thread)} sampled threads, {len(recs)} traced calls'
          f' ({len(unknown)} threads only in the trace)')


if __name__ == '__main__':
    main()

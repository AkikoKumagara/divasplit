# Engine notes (Project DIVA f 2nd EU 1.00)

What reverse engineering and profiling found so far, for the next round of
optimization work. Addresses are seg0 offsets of the decrypted eboot
(module NID `0xDB737975`) unless they start with `0x81`, which are absolute
addresses in the game's data segment as the code loads them with `movw/movt`.
Functions are named by their start address (from the eboot's `.ARM.exidx`).

## Frame

The frame function `0x00AC2E` (called from the main loop `0x00C2B4`, inside
`0x00C4D8` / `0x4D34E4` / module start `0x4D361C`) runs, in order:

| Call site | Callee | What |
|---|---|---|
| `0xAC4E`..`0xAC7C` | `0x4BE6D2`, `0x44A7DA`, `0x243AF2`, `0x247198`, `0x275D5A`, `0x219AA8`, ... | setup (`0x4BE6BA` / `0x4BE6E0` look like the game's own profiler marks) |
| `0xAC84` | `0x014B06` | **task fork-join** (below), ~1.9 ms, nearly all waiting |
| `0xAC8A`..`0xACDC` | `0x44ABC8`, `0x434B4E`, `0x410868`, `0x2B14BA`, `0x2B1488`, `0x275DC4`, `0x275DB0` ×2 | small updates, cheap |
| `0xACF8`..`0xAD00` | `0x26579E`, `0x265D16` (stage loop, calls `RenderStage` `0x265920`), `0x2657A0` | render stages, ~11 ms CPU |
| `0xAD04`, `0xAD08` | `0x4E0810`, `0x28A8DC` | |
| `0xAD32` | `0x014B26` | (task module, 94 bytes, no calls) |
| `0xAD36` | `0x00A89A` | swap + vsync wait (`sceDisplayWaitVblankStartMulti` at `0xA994`) |

About 3–4.5 ms per frame of the main thread's time is spent outside the frame
function (game update, called from the main loop), including the character
join below.

Measured per frame, heavy PV, normal frames → slow frames (> 18 ms): stage 3
(3D world) CPU 5.4 → 7.2 ms (the only growth), stage 5 (2D) 2.4 → 2.5,
outside rendering 3.1 → 3.2, waits 4.8 → 13.9 (the missed vblank).

## Task system (`diva_task_disp`)

- Task manager: global pointer at `0x81750810`. `+0x10` dispatching flag,
  `+0x14` dispatcher object.
- `0x014B06`: `mgr = *0x81750810; mgr->flag = 1; 0x014DAA(mgr->disp, mgr); mgr->flag = 0`.
- Dispatcher `0x014DAA(disp, list)`, `pool = *disp`:
  1. `pool+0x70 = list`
  2. `0x4DB8F8(i, 0)` for i = 1, 2: copy render context 0 into contexts 1, 2
  3. `pool+0x74 = *(*(list+4))` (first task node)
  4. for each worker `w = pool+0x78+4*i` (i < 3) not busy: `w+0x78 = 1`, unlock `w+0x38`
  5. unlock then lock `pool+0x38` (the workers' gate)
  6. for each busy worker: lock `w+0x38` (waits for it to finish), `w+0x78 = 0`
  7. `pool+0x70 = 0`
- Lock / unlock wrappers: `0x002AFA(obj, 0)` / `0x002B36(obj)`, an LwMutex at
  `obj+0x18`, flags at `obj+8`.
- Workers are created in `0x014C88`: three threads `diva_task_disp`,
  priority 160 (`0x10000100`), affinity `0x70000`, stack `0x40000`. Worker
  struct (0x80 bytes): `+0x38` mutex object, `+0x70` pool, `+0x74` index
  (0, 1, 2), `+0x78` busy.
- Worker entry `0x014F6E(w)`: set the thread's context index to `w+0x74` in
  three subsystems (`0x4DB8CA`, `0x243A42`, `0x275EEC`), then loop: pass the
  gate, lock own mutex, run `0x014E92(w)` if busy, clear busy, unlock.
- Job loop `0x014E92(w)` uses only `w+0x70` (the pool): under the pool mutex
  it pops task nodes from `pool+0x74` (node: `+0` next, `+8` task; list end
  at `list+4`), runs each with `0x0145C0(task)` (checks `task+0x10`,
  `task+0xC` states, calls the task's virtual method at vtable `+0x14`), and
  stores the game profiler's time with `0x0145EE` (`task+0x4C`).

### Per-thread contexts

| Setter | TLS variable | Accepts |
|---|---|---|
| `0x4DB8CA` (render) | `0x81772644` | 0..2 |
| `0x243A42` | `0x81772634` | 0..2 |
| `0x275EEC` | `0x8177263C` | 0..2 |

The main thread is index 0 and so is worker 0: while the main thread waits in
the dispatcher, worker 0 uses its context. The render context array is at
`0x81D1E2A8 + i * 0x24C` and has 29 slots (`0x4DB8F8` checks `< 0x1D`); the
sizes of the other two subsystems' arrays are not known yet.

## Characters (`RobThread`)

- Threads `RobThread::ctrl_thread_launcher` are created in `0x366424` (four,
  two busy with two singers, ~1.6 ms each per frame).
- The main thread waits for them in `0x366654` (`sceKernelWaitLwCond` via
  `0x4A1CAC`), reached from `0x01488A` → `0x014702` → `0x34C5FC` / `0x354704`,
  outside the frame function: ~1 ms per frame.

## Rendering hot spots

- Shader binding `0x4DE828` (vertex) and `0x4DE706` (fragment): set the
  program, reserve the default uniform buffer, copy every parameter from the
  engine's uniform table (`0x81D1E2A8 + 0x2EF24` vertex, `+ 0x30274`
  fragment) with `memcpy` (or `sceGxmSetUniformDataF` for flagged ones; not
  used in the measured PV). Program entries: `sb + idx * 0x368` with the
  parameter count at `+0x1B4` and 12-byte entries `{dst offset, src offset,
  size | flag bit 31}` from `+0x34`.
  - The vertex `memcpy` at `0x4DE906` is the biggest single cost: ~850 copies,
    ~850 KB a frame into the GXM ring (`0x7018xxxx`–`0x7077xxxx`), ~1.5 ms
    across stages 2–5. Blocks are 4880 bytes (bone palettes); 54% repeat the
    previous draw's block.
  - A second vertex binding path is `0x4E0514` (reserve `0x4E0572`).
- Draw wrappers: `0x4D8CCA` (3 vertex streams + `sceGxmDraw`), `0x4E1190`
  (polygon mode ×3), `0x4E064A`, `0x4E0674`.
- 4×4 matrix multiply `0x4D0FDE`: `out[i][j] = A[i][1]*B[1][j] + A[i][0]*B[0][j]
  + A[i][2]*B[2][j] + A[i][3]*B[3][j]`, scalar VFP, all loads before stores.
  The same bytes are at `0x3CCB02` in f; X has only transposed variants
  (`0x441F14` = Aᵀ·B, `0x4420E0` = A·Bᵀ).

## Experiments that didn't help

| Experiment | Code | Result |
|---|---|---|
| NEON matrix multiply | `src/opt.c`, `opt_matmul` | slower (1.26 vs 0.8 ms per frame): load-after-store stalls, tiny function |
| Uniform block reuse (stage in cached memory, `sceGxmSetVertexDefaultUniformBuffer` for repeats) | `src/opt.c`, `probe_ret` | works, but ~0.8 ms slower: the compare and staging copy cost about what the GPU writes saved |
| Core pinning (main thread alone on core 1) | `src/threadview.c` `tv_pin` | slightly slower; ready time was only ~2.5% |

## Ideas still open

1. **A fourth task worker context.** Let the main thread run tasks in the
   dispatcher (after step 5) with its own context index 3:
   - Find the arrays indexed by the TLS variables `0x81772634` and
     `0x8177263C` (search for code loading them through `__tls_get_addr` and
     indexing an array) and their sizes. If they have room for index 3, like
     the render contexts do, raise the three setters' `cmp r4, #3` to 4.
   - In a hook of `0x014DAA`: after the gate, `0x4DB8F8(3, 0)`, set the
     three indices to 3, call `0x014E92` with a stand-in worker whose
     `+0x70` is the pool, set the indices back to 0, then wait as before.
   - Expected: four executors instead of three for ~3.7 ms of task work,
     maybe ~0.5 ms a frame. Check for tasks that assume they are not on the
     main thread (locks the main thread holds there).
2. **Overlap the two worker hand-offs.** Start the characters' work before
   the task fork-join if the tasks don't consume the characters' results
   (likely they do: tasks probably prepare draws). Needs the order of
   `0x01488A` (characters) relative to the frame function, and where the
   `RobThread` work is kicked.

## Things that bite

- `TAI_CONTINUE` declares locals named `cur` and `next`; a variable of either
  name passed to it is silently replaced (this caused a black screen). Build
  with `-Wshadow`.
- Hooking an import the game calls thousands of times a frame while it runs
  can crash it (taiHEN patches the stub in place). Install such hooks in
  `module_start`.
- `sceKernelGetThreadRunStatus` crashed the game (its real signature isn't the
  SDK's).
- Kernel `ksceKernelGetThreadCpuRegisters`: inside a system call, register set
  0 is the kernel context and set 1 the user registers saved on entry.
- User mode can't load kernel modules (taiHEN allows it only for the shell),
  so the profiler loads at boot and is started by system calls.

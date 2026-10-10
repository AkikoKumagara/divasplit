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
| `0xAC84` | `0x014B06` | **task fork-join** (below), ~1.9 ms, nearly all waiting (`0x44ABC8` replaces it when `0x44A80E()` is non-zero) |
| `0xAC8A`..`0xACDC` | `0x44ABC8`, `0x434B4E`, `0x410868`, `0x2B14BA`, `0x2B1488`, `0x275DC4`, `0x275DB0` ×2 | small updates, cheap |
| `0xACF8`..`0xAD00` | `0x26579E`, `0x265D16` (stage loop, calls `RenderStage` `0x265920`), `0x2657A0` | render stages, ~11 ms CPU |
| `0xAD04`, `0xAD08` | `0x4E0810`, `0x28A8DC` | |
| `0xAD32` | `0x014B26` | runs every task's Basic method (vtable `+0x18`) on the main thread |
| `0xAD36` | `0x00A89A` | swap + vsync wait (`sceDisplayWaitVblankStartMulti` at `0xA994`) |

About 3–4.5 ms per frame of the main thread's time is spent outside the frame
function: the task ctrl pass (`0x01488A`, called from `0xAEB0` at `0xB0BE`,
before the frame function), which includes both character joins below.

The frame step: `0xAFA2..0xAFD0` scales it by `max(1, target / measured)` when
the catch-up flag `0x81787B30` is set (divasplit's SYNC sets the byte it is
copied from, FM `+0xDC`); the number of ctrl ticks is its integer part. In an
extra tick only tasks with `+0x21` set run (PREP, ROB_BASE, MOTION_MODIFIER,
COLLISION), so ROB_BASE kicks and joins at once without PVGAME's early kick:
roughly 1.8 ms more on a 2-tick frame (not measured; every profile so far ran
with SYNC off).

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
the dispatcher, worker 0 uses its context. There are exactly 3 contexts in each
subsystem (the `< 0x1D` check in `0x4DB8F8` is a stack count reused as a bound):
index 3 would land on other live data in all three (subsystem A: past the
0x1A78-byte object into its 14 shared lists; B: the sprite lock's vtable at
obj+0xF48; render: base+0xC44, a live table).

## Characters (`RobThread`)

- Threads `RobThread::ctrl_thread_launcher` are created in `0x366424` (four,
  two busy with two singers, ~1.6 ms each per frame).
- The main thread waits for them in `0x366654` (`sceKernelWaitLwCond` via
  `0x4A1CAC`), reached from `0x01488A` → `0x014702` → `0x34C5FC` / `0x354704`,
  outside the frame function: ~1 ms per frame.
- ROB_BASE (job `0x34DBCA`, ~1.3–1.5 ms on the busier RobThread) is kicked early
  from PVGAME ctrl (`0x5F1B40` → `0x35C71E` → `0x34C9A4`, kick at `0x34CA70`)
  and overlaps AUTH_3D ctrl; the join at `0x34C6CE` still waits 0.5–0.7 ms and
  its results are used right after (`0x34C6F8`).
- ROB_DISP (`0x354704`) kicks job `0x354C0D` at `0x354850` and joins at once at
  `0x35485E` (0.5 ms); the job's only later consumer is the disp task
  `0x354A98` inside the fork-join. Deferring the join is possible but needs a
  snapshot of the wind (`0x360796` returns `wind+0x38`, rewritten every frame
  by CHARA WIND `0x362788`), a join before stage changes (`0x41F3FC`) and the
  cleanup tail `0x354862..0x3548C2` moved along.

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
    across stages 2–5. Blocks are 4880 bytes; 54% repeat the previous draw's
    block, almost all of them 2D sprites in stage 5 (≥95% of stage-5 blocks
    repeat, ≤27% of stage 3's). Bone palettes never repeat: each draw item gets
    its own from a bump pool (`0x2448C8`).
  - What fills `T` (the vertex table, 0x1350 bytes; name table `0x816DA9D8`):
    `program_lcl` 0 (128 B), `program_env` 0x80 (3584 B),
    `state_matrix_program` 0xE80 (576 B), `mvp` 0x10C0, `state_matrix_texture`
    0x1100 (512 B), five materials 0x1300..0x1350. The fragment table `F` is
    `T+0x1350`. Writers during play: the setters `0x4DFBE4` / `0x4DFC72` (env,
    flags `T+0x1A2A+i`; fragment env `F+0x200+16i`, i < 32, flags `T+0x1B0A+i`)
    and `0x4DFD12` / `0x4DFDA2` (local, flags `T+0x1B2A`), the per-draw setup
    `0x4DF052` (MVP), and its flush `0x4DEF04`: material copies `0x4DE314`,
    matrices `0x4DE954` (P, MV, MVP always; program / texture matrices only
    when their dirty byte `mc+0x6EC+0x14k` / `mc+0x5AC+0x14j` is set).
  - The game already sorts and merges each program's parameter entries at
    init (`0x4DF6F2` vertex, `0x4DF2C8` fragment): about 5 copies per block.
  - Copies cost about 1.03 ns/B into cached memory and 2.24 ns/B into the
    ring (v10 A/B, clean windows).
  - A second vertex binding path is `0x4E0514` (reserve `0x4E0572`).
- Draw wrappers: `0x4D8CCA` (3 vertex streams + `sceGxmDraw`; can return
  before drawing), `0x4E1190` (polygon mode ×3; 2D, calls the pre-draw
  callback with `blx r3` at `0x4E11A8` and always draws), `0x4E064A`,
  `0x4E0674`.
- Per-draw uniform setup `0x4DF052`: 3D calls it at `0x247ED6`, 2D through
  the draw descriptor (`0x4D8626`). MVP, flush `0x4DEF04`, vertex binding
  `0x4DE828`, which calls the fragment binding `0x4DE706`.
- 4×4 matrix multiply `0x4D0FDE`: `out[i][j] = A[i][1]*B[1][j] + A[i][0]*B[0][j]
  + A[i][2]*B[2][j] + A[i][3]*B[3][j]`, scalar VFP, all loads before stores.
  The same bytes are at `0x3CCB02` in f; X has only transposed variants
  (`0x441F14` = Aᵀ·B, `0x4420E0` = A·Bᵀ).

## Main-thread cuts (OPT, `src/opt.c`)

Found by static analysis, checked against the profiles; not yet measured on
the console. Each is a key in `ux0:data/divasplit/opt.txt` (e.g. `centre=1
dirty=1 lsort=1 nosort0=0`); OPT and A/B switch the set. `test/test_cuts.c`
checks `centre` and `lsort` on the host against the game's versions.

| Cut | Where | What | Expected (light / heavy frames) |
|---|---|---|---|
| `centre` | hook `0x245CDC` | The 3D sort keys average a skinned object's centre over its bones: per bone `xform(inverse(W) * B_i * (W * X), c)`, two 4x4 products and a transform. Linear in `B_i`: sum the bones first, then one product chain. Same up to float rounding (host test: 8e-6 relative). Called only from `0x246050` (stage 3, main thread). | 0.45 / 0.8 ms |
| `dirty` | patch `0x4DF012` (32 bytes) | `0x4DEF04` (once per draw flush, gated by `[T+0x1B50]`) clears 224 dirty-flag bytes at `T+0x1A2A` one by one (`if (b) b = 0`); patched once at game start to call the plugin, which does one `sceClibMemset` (cut on; timed on the console: 0.15 µs against the loop's 2.8 µs) or the same loop (off). Same memory afterwards. Patching it at run time instead (for A/B) risks a core still holding the old bytes. | 0.4–0.5 / 0.55–0.65 ms |
| `lsort` | hook `0x5ABB9C` | `std::list::sort` of the draw lists allocates 27 list heads per call from a locked heap. Bottom-up merge sort over static arrays: stable, the same order. Callers `0x246A68/90/B0` only. | 0.08 / 0.15 ms |
| `nosort0` | stage table byte `0x81B628A1` | Skip sorting the opaque list 0 front to back (`0x265ACE`). Changes draw order (coplanar flicker possible); off by default. | +0.2 ms on top of the others |

Measured (2026-10-10, f 2nd heavy PV, A/B, 180 paired seconds): stage 3
−0.65 ms per frame in every 20 s block, but the rest of the main thread
+0.35 ms, net cpu −0.29 ms, fps unchanged within noise. Not `sceClibMemset`:
timed at start on the console, 224 bytes take 0.15 µs with it, 0.8 µs with
an inline word loop and 2.8 µs with the game's byte loop. Still unexplained;
log.csv now has each stage's CPU time (`st0`..`st7`) to locate it.

Round 2 (on top of the above; profile ms, about 8% inflated by the sampler).
`test/test_r2.c` checks `keyset` and `h2f` on the host against vectors made by
emulating the game's machine code.

| Cut | Where | What | Expected (light / heavy) |
|---|---|---|---|
| `ureuse` | hook `0x4DF052` + import hook `sceGxmReserveVertexDefaultUniformBuffer` | 2D draws only: when no setter flag or matrix dirty byte is set and MV, P and the materials are bitwise unchanged, `T` after the flush is byte-identical to the last flush's, so the block uploaded then (same program record, same `SceGxmVertexProgram`, same scene) is the one the game would write. Skip the flush, MVP and copies; `sceGxmSetVertexDefaultUniformBuffer` to the old block. Never compares block data except the check (`check=1`, else 1 in 1024: reused block against `T`, entry by entry; a difference turns it off for the session). | 0.75 / 0.8 ms (by hit rate) |
| `matdup` | patches `0x4DEF28` (16 B), `0x4DE994` (22 B) | The flush's ten 16-byte material getters are two 80-byte copies of `G+0x30F14`; `0x4DE954` recomputes the MVP `0x4DF052` just stored at `T+0x10C0`. | 0.14 / 0.18 ms |
| `palette` | hook `0x2472A0` | Bone palette: three `0x4DFBE4` calls per bone → plain copies to `T+0x280+48i` and one flag `memset`. | 0.07 ms |
| `texwrap` | hook `0x4E1B30` | `texParameteri` WRAP_S/T: the same stores and `sceGxmTextureSetU/VAddrMode` call without the 3-level dispatch. | 0.06 / 0.08 ms |
| `keyset` | hook `0x20462A` (RobThreads) | ROB_BASE key-set evaluator `0x204334` inlined, the game's exact VFP sequence. Shortens the ROB_BASE join wait. | 0.06–0.1 ms |
| `h2f` | 90-byte patch `0x4B1A48` | half → float without the stack-canary prologue; all 65536 inputs checked against the original at start, before and after patching. | ~0.03 ms (with `keyset` off) |
| `nolog` | 2-byte patch `0x3367D2` | The DSC player formats a trace line per command into a ring nothing reads (`0x81773808`). | 0.01 / 0.02 ms |
| `earlykick` | hooks `0x336402`, `0x35C71E` | Kick ROB_BASE right after the DSC interpreter instead of after the PV update tail, when no PvOsageManager slot is busy or ready. Not proven exact (indirect calls in the tail, late task requests): off by default; `ekcheck` hashes the robot state at both points first. | 0.15–0.2 ms × the share of frames it applies to |
| `ucopy` | hooks `0x4DE828`, `0x4DE706` | Timing experiment: the bindings with aligned NEON copies instead of SceLibc `memcpy`. Off by default. | −0.05 … +0.35 ms |

`h2f` and `nolog` are patched at start if set then; `earlykick`, `ekcheck`
and `ucopy` are hooked only if set at start.

Validation run (2026-10-10, 3 songs, `check=1`, first 3 minutes in diag.txt):
`ureuse` reused 255568 of 311603 2D pre-draws, every one compared with `T`,
0 different. Hit rate by stage: s2 97%, s3 31%, s4 0% (37628 calls), s5 96%.
`centre` checked on every call: largest difference 9e-6. `ekcheck` never ran
(its gate, no PvOsageManager slot busy or ready, apparently never opened), so
`earlykick` is parked. `h2f` didn't go in: the check of its original bytes
included the movw/movt of the stack-canary address, which the loader fills in
(not seg0-relative either); now only their shape is checked, and with that
it goes in (2026-10-10: patch id set, self-test passed after patching).

Timing run (2026-10-10, f 2nd heavy PV, AVS off, A/B, 200 windows): frame time
−2.32 ± 0.34 ms, cpu −1.57; stage 5 2.26 → 1.14 ms, stage 3 −0.77, stage 4
+0.08, outside the stages +0.26 ± 0.22, GPU wait +0.20. fps 51.3 → 55.9
(worst 10% of seconds 31.4 → 49.9; seconds at 60: 25% → 40%).

Stage 3 is not the only thing that grows in heavy frames: PVGAME ctrl
(`0x5F1B40`) grows 0.23 → 0.48 ms, in the PV script `0x3256EC` → `0x325374` →
`0x31B346`, which runs before the ROB_BASE kick.

## Experiments that didn't help

| Experiment | Code | Result |
|---|---|---|
| NEON matrix multiply | `src/opt.c`, `opt_matmul` | slower (1.26 vs 0.8 ms per frame): load-after-store stalls, tiny function |
| Uniform block reuse (stage in cached memory, `sceGxmSetVertexDefaultUniformBuffer` for repeats) | `src/opt.c`, `probe_ret` | works, but ~0.8 ms slower: the compare and staging copy cost about what the GPU writes saved |
| Core pinning (main thread alone on core 1) | `src/threadview.c` `tv_pin` | slightly slower; ready time was only ~2.5% |

## Ideas that don't pan out

- **A fourth task worker context** (the main thread as a 4th executor in the
  fork-join): the game has 3 cores and the main thread shares the workers'
  priority, and the 3 workers already keep all 3 cores busy for most of the
  fork-join (v4: all 3 running in 55–57% of its samples; 4.2 ms of task work
  is a 1.4 ms floor on 3 cores). The idle time is the drained tail: 85% of idle
  worker samples sit at the gate after the queue ran empty. Realistic gain
  ≤ 0.1 ms, and index 3 doesn't exist (above).
- **Overlapping the joins:** ROB_BASE is already kicked as early as its inputs
  allow and consumed right after its join; reordering the task list gives the
  same makespan in a list-scheduling simulation with the measured task sizes.
- **Splitting 2D or AUTH_3D disp work:** 2D sprites are appended in time order
  under one lock and drawn unsorted; a split changes overlap order.
- **Recording stage 3 draws on another core:** the draw path is an
  immediate-mode state machine on global state (uniform table, TLS matrix
  stacks); it would mean replacing the engine's GL layer.

- **3D uniform reuse:** stage-3 blocks repeat at most 20–27%; checking the
  flagged palette and env entries costs about what reuse saves.
- **Splitting one ROB_BASE job across the RobThreads:** all 3 cores are busy
  from kick to join; at most 0.27 / 0.18 ms even with perfect sharing.
  `mult_0` and `mult_1` aren't redundant (IK runs between them).
- **PV script "doubling" in heavy frames** is mostly the 18 ms sampling
  artifact; no scan in it grows with script length.
- **Fragment uniform reuse:** stage 5's fragment binding is 0.03 ms.

## Things that bite

- `TAI_CONTINUE` calls through an unprototyped pointer: a `float` argument is
  promoted to `double` and passed in `d0`, not `s0`, so the game's function
  gets garbage. Hooks with float arguments call on through a prototyped
  pointer (`TAI_NEXT` in `src/opt.c`).
- Profiles split at 18 ms are biased (every function gains about one sample
  in the 17–19 ms buckets). Classify frames by vblank count instead.

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

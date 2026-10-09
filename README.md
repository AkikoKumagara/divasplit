# divasplit

Frame pacing and performance plugin for **Hatsune Miku: Project DIVA X, f and f 2nd** on PS Vita, made to go with the 60 fps VitaGrafix patches.

| Game | Title ID | Version |
|---|---|---|
| Project DIVA X | PCSB01007 / PCSE00867 | EU / US 1.00 |
| Project DIVA f | PCSB00419 | EU 1.00 |
| Project DIVA f 2nd | PCSB00554 | EU 1.00 |

In any other game or version the plugin stays idle.

## What it does

- **AVS (adaptive vsync).** The games wait for the next screen refresh after every frame. A frame that takes even slightly longer than 16.7 ms then waits for the refresh after that, so a small slowdown becomes a drop straight from 60 to 30 fps. AVS skips that wait only when the frame is already late. A 17 ms frame stays 17 ms (about 58 fps instead of 30). It never goes above 60 fps, and frames are still flipped on vblank, so there is no tearing.
- **SYNC (frame-drop catch-up).** The engines have a built-in mode that scales game time by target/measured fps, so notes and animation keep real-time speed when the frame rate drops. The games never turn it on; this plugin can.
- **Split render presets.** Redraw the 3D world (PV) less often while notes and UI still redraw every frame. This trades PV smoothness for CPU and GPU time.
- **Status bar** with fps and a per-frame timing breakdown: CPU work, waiting on the GPU, waiting for vblank, and 3D submit time.
- **Recording.** One line per second goes to `ux0:data/divasplit/log.csv`.
- **Settings per game**, remembered across launches and changeable from the **Quick Menu** (hold PS).

## Install

You need taiHEN (HENkaku / Ensō) and VitaShell.

1. Copy `divasplit.suprx` to `ur0:tai/`.
2. In `ur0:tai/config.txt`, add a section for each game you have:
   ```
   *PCSB01007
   ur0:tai/divasplit.suprx
   *PCSB00419
   ur0:tai/divasplit.suprx
   *PCSB00554
   ur0:tai/divasplit.suprx
   ```
   For the US release of X, use `*PCSE00867`.
3. Reboot.

### Quick Menu toggles (optional)

This needs [QuickMenuReborn](https://github.com/Ibrahim778/QuickMenuReborn/releases) v2.5.

1. Copy `QuickMenuReborn.suprx` to `ur0:tai/` and add it under `*main` in `config.txt`.
2. Copy `qmr_plugin.rco` (from the QuickMenuReborn release) to **`ur0:QuickMenuReborn/`**. Version 2.5 always loads it from `ur0:`, even if you keep its plugins on `ux0:`.
3. Copy `divasplit_qm.suprx` to `ux0:QuickMenuReborn/` (or `ur0:QuickMenuReborn/` if there is no `ux0:` folder).
4. Reboot.

Holding PS shows a *divasplit* section with these toggles:

- AVS
- SYNC
- Status bar
- Recording
- 3D preset button

They apply to the DIVA game that started last, and take effect in the running game within half a second.

## Controls in game

Hold **L + R**, then press:

| Button | Action |
|---|---|
| RIGHT | next 3D preset |
| LEFT | AVS on / off |
| UP | SYNC on / off |
| DOWN | recording on / off (in X, also shows the game's own profiler) |

## Presets

| Preset | Effect |
|---|---|
| OFF | normal game |
| 3D 1/2 | 3D world drawn every 2nd frame |
| 3D+FX 1/2 | also the extra off-screen pass |
| ALL3D 1/2 | also the 3D setup / clear stage |
| 3D 1/3 | 3D world drawn every 3rd frame |
| NO 3D test | 3D world never redrawn. Diagnostic only: shows the fps with zero 3D drawing. |

## Reading the status bar

```
SPLIT OFF  SYNC OFF  AVS ON  58fps cpu 13.9 gpu 0.4 vs 2.4 3d 5.6
```

All numbers are milliseconds per frame, averaged over one second.

| Field | Meaning |
|---|---|
| cpu | the game's main thread is working |
| gpu | blocked waiting for the GPU (`sceGxmBeginScene` / `EndScene` / `DisplayQueueAddEntry`) |
| vs | waiting for vblank |
| 3d | CPU time to submit the 3D world |

`cpu + gpu + vs` equals the frame time. When fps drops:

- **gpu grows:** the GPU is the bottleneck.
- **cpu grows:** the CPU is. In practice, X is mostly GPU-bound and f 2nd is CPU-bound in its 3D stage.

## Recommended setup

- **AVS on.** This alone removes the 60 → 30 drops.
- **SYNC on**, if you want game speed to stay exact during the remaining dips. The trade-off is a small stutter.
- **A split preset** only if a PV still drops a lot. 3D 1/2 + AVS held f 2nd's heaviest PV at 59.7 fps on average.

## VitaGrafix 60 fps patch

The 60 fps patches for the EU releases (and X US) are in [`vitagrafix/`](vitagrafix). They are also submitted upstream as [VitaGrafixPatchlist PR #162](https://github.com/Electry/VitaGrafixPatchlist/pull/162).

Until that PR is merged, install them yourself:
1. In `ux0:data/VitaGrafix/patchlist.txt`, replace the game's existing block with the one from `vitagrafix/`.
2. Set `FPS=60` for that title in `ux0:data/VitaGrafix/config.txt`.

They include an input fix that matters with or without this plugin.

The games only accept a controller sample newer than the last one they used. At 60 fps there is about one sample per frame, so sometimes a frame gets none. The game then reads every button as released, which breaks hold notes and causes double presses. The fix makes the game always use the newest sample.

| Game | Patch lines |
|---|---|
| X | `0x283352` / `0x28335A` |
| f | `0x12E24` / `0x12E28` |
| f 2nd | `0x2896F6` / `0x2896FE` |

## Other releases (US / JP / ASIA)

Only the EU releases (plus X US, which is the same build) are supported so far, because every release puts the code at different addresses. `tools/port.py` ports both the 60 fps patch and the plugin addresses to another release of X, f or f 2nd. It finds each EU patch site by the code around it.

1. Dump the decrypted `eboot.bin` of **your own copy** (e.g. with FAGDec) and copy it to a PC.
2. Run the script:
   ```
   pip install capstone
   python3 tools/port.py eboot.bin.elf --title PCSE00434
   ```
3. Use what it prints:
   - The `@FPS` block goes into that title's block in `ux0:data/VitaGrafix/patchlist.txt`. Keep its existing `@IB` lines and replace any old `@FPS` lines.
   - The `GAMES[]` line goes into `src/main.c`.

The script lists any site it couldn't find for certain instead of guessing. A ported patch still needs a test run in game before sharing. If it works, please open an issue or PR with the output. Never share the eboot itself.

`tools/mkpatterns.py` rebuilds `tools/patterns.json` from the EU dumps (maintainer use).

## Files

| Path | Contents |
|---|---|
| `ux0:data/divasplit/settings.txt` | settings per game |
| `ux0:data/divasplit/log.csv` | recordings (each run starts with a header line) |
| `ux0:data/divasplit/diag.txt` | load diagnostics for the current session |

## How it works

These are the engine notes (EU 1.00 offsets in seg0) behind the plugin.

- **Render stages.** Every frame, the engine calls `RenderStage(stage)` in a loop.
  - **X and f 2nd** have 8 stages, with the same layout in both games. RenderStage is at `0x271368` in X and `0x265920` in f 2nd.
    - 0: setup / clear
    - 2: extra pass
    - 3: 3D world
    - 4: post effects + composite
    - 5: 2D layers (notes, UI)
  - **f** has 14 stages, with RenderStage at `0x28C54E`.
    - 5: setup
    - 7: extra pass
    - 8: 3D world
    - 10: post effects
    - 11 / 12: 2D layers
  - The plugin hooks RenderStage by offset and skips stages according to the preset.
- **Frame manager.**
  - `GetDelta()` returns `step × max(1, target/measured)` when the catch-up flag is set.
  - Every frame, the game copies a "requested" byte into that flag:

    | Game | Requested byte → flag | Frame manager |
    |---|---|---|
    | X | `fm+0x2D` → `+0x2C` | pointer at `0x814F756C` |
    | f | `fm+0xEC` → `+0x18` | `0x816E7228` |
    | f 2nd | `fm+0xDC` → `+0x18` | `0x81787B18` |

  - SYNC writes the requested byte.
- **Vblank.** The main loop calls `sceDisplayWaitVblankStartMulti(vblank)` on the main thread.
  - AVS hooks that import and skips the wait when `vcount - vcount_at_last_frame >= vblank`, only on the thread that runs RenderStage.
  - The flip itself is `sceDisplaySetFrameBuf(..., NEXTFRAME)` from the GXM display queue callback.
- **Profiler.** X keeps its own CPU profiler in the frame manager, in microseconds:

  | Field | Offset |
  |---|---|
  | update | `+0x60` |
  | draw | `+0x70` |
  | swap | `+0xA0` |
  | sub-phases | `+0x118`–`+0x178` |

  The recording's detail columns come from these.

## Building

You need [VitaSDK](https://vitasdk.org) with the taiHEN stubs (`vdpm install taihen`).

```
mkdir build && cd build
cmake .. && make
```

This produces `divasplit.suprx` (game plugin) and `divasplit_qm.suprx` (Quick Menu plugin).

## Credits

- Font: Terminus Font 8×14 bold, by Dimitar Toshkov Zhekov (SIL Open Font License 1.1). The bitmap header comes from Electry's PSVshell.
- Quick Menu: [QuickMenuReborn](https://github.com/Ibrahim778/QuickMenuReborn) by Ibrahim778 (MIT). Its headers and stub library are included in `qm/`.
- VitaGrafix and its patchlist, by Electry and contributors.

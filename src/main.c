/*
 * divasplit - frame pacing / split render plugin for Hatsune Miku: Project DIVA
 *   X     EU/US 1.00 (PCSB01007 / PCSE00867, module NID 0x0A3FF027)
 *   f     EU 1.00    (PCSB00419,             module NID 0x544807B3)
 *   f 2nd EU 1.00    (PCSB00554,             module NID 0xDB737975)
 *
 * Every frame the games call RenderStage(stage) in a loop. X and f 2nd have
 * 8 stages (X: seg0+0x271368, f 2nd: seg0+0x265920, same layout):
 *   0 viewport / 3D buffer setup + clear     3 3D world (characters, stage)
 *   1 (empty)                                4 post effects + composite, starts the
 *   2 extra off-screen pass + draw layer 2     framebuffer scene
 *   6, 7 (empty)                             5 2D layers (notes, UI) into framebuffer
 * f has 14 (seg0+0x28C54E): 5 setup, 7 extra pass + layer 1, 8 3D world,
 * 10 post effects, 11/12 2D layers, the rest empty or flush only.
 *
 * The plugin skips the selected 3D stages on some frames, so the last 3D
 * image is reused while notes and UI still redraw every frame.
 *
 * Frame-drop catch-up ("SYNC"): GetDelta() scales the frame step by
 * target/measured fps when the frame manager's active flag is set. Once per
 * frame the game copies a "requested" byte into that flag (X: fm+0x2D -> +0x2C,
 * FM pointer at seg1+0x956C; f 2nd: fm+0xDC -> +0x18, FM at 0x81787B18;
 * f: fm+0xEC -> +0x18, FM at 0x816E7228).
 * The game never sets the requested byte; this plugin can.
 *
 * Perf readout (updated once per second, ms per frame):
 *   fps   frames per second (stage 0 to stage 0)
 *   cpu   time the main thread is busy
 *   gpu   time blocked in sceGxmBeginScene / EndScene / DisplayQueueAddEntry,
 *         i.e. waiting for the GPU to catch up
 *   vs    time in sceDisplayWaitVblankStartMulti: the game waits for vblank
 *         on the main thread every frame (idle time at a solid 60)
 *   3d    CPU time of the 3D world stage when it runs (waits excluded)
 *
 * Detail readout (X only): the game's own CPU profiler (uint32 microseconds of
 * the last frame, kept in the frame manager), averaged per second:
 *   upd   +0x60   whole update (seg0+0x42358)
 *     a   +0x118  game logic subsystems (seg0+0x42400..0x424C0)
 *     b   +0x138  seg0+0x2FB282 (conditional)
 *     c   +0x148  seg0+0x33858E / 0x33851C
 *   draw  +0x70   whole render function (seg0+0x41CC0, before the swap)
 *     p   +0x158  render prep (seg0+0x338532 / 0x2FB2AA)
 *     l   +0x178  layer setup (seg0+0x27468C / 0x274676)
 *     r   +0x168  render stages loop + scene end
 *   swap  +0xA0   display swap incl. waiting
 *
 * Adaptive vsync ("AVS"): the game waits for the next vblank on its main
 * thread every frame. A frame that runs even slightly over 16.7 ms then waits
 * for the vblank after that, so it costs 33 ms (60 -> 30 fps). With AVS on,
 * the wait is skipped when a vblank already passed since the last frame
 * started. Every frame still spans at least one vblank (never above 60 fps),
 * and the flip itself stays on vblank (SetFrameBuf NEXTFRAME), so no tearing.
 *
 * AVS only acts on the game's main thread (the one running RenderStage);
 * other threads that wait for vblank are left alone.
 *
 * Hotkeys (hold L + R, then press):
 *   RIGHT         next preset
 *   LEFT          toggle AVS (adaptive vsync)
 *   UP            toggle SYNC (frame-drop catch-up)
 *   DOWN          toggle recording (+ detail readout on X)
 *   SELECT        engine optimizations: off -> STAGE -> on -> A/B -> off.
 *                 A/B switches them every second while recording (log.csv's
 *                 opt column says which way each second ran). STAGE is a
 *                 test mode: uniform staging without reuse.
 *
 * Settings are remembered per game in ux0:data/divasplit/settings.txt (see
 * settings.h). The input thread rereads it twice a second, so the Quick Menu
 * plugin (divasplit_qm.suprx) can change them while the game runs.
 *
 * While recording (REC), every 1 s line is appended to
 * ux0:data/divasplit/log.csv. The render thread only formats the line into a
 * small ring buffer; the low-priority input thread does the file writes.
 *
 * Recording also turns on the thread view (threadview.c): the main thread's
 * running / ready / waiting split, what it waits on, per-core load and every
 * game thread's CPU time, into ux0:data/divasplit/threads.csv and a second
 * status bar line. sceKernelCreateThread is hooked to find the game threads.
 *
 * Engine optimizations (opt.c, L+R+SELECT or the Quick Menu switch them off):
 * faster versions of hot game functions, found with the profiler.
 *
 * With the optional kernel plugin divasplit_prof.skprx installed, recording
 * also runs the sampling profiler (prof.bin, see prof.h and tools/profview.py).
 *
 * Font: Terminus 8x14 Bold (SIL OFL 1.1), taken from Electry's PSVshell.
 */
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <taihen.h>
#include <stdarg.h>

#include "font_ter_u14b.h"
#include "settings.h"
#include "threadview.h"
#include "prof.h"
#include "opt.h"
#include "trace.h"

#define STAGE_MAX 32

#define NID_LIB_GXM            0xF76B66BD
#define NID_GXM_BEGIN_SCENE    0x8734FF4E
#define NID_GXM_END_SCENE      0xFE300E2F
#define NID_GXM_DQ_ADD_ENTRY   0xEC5C26B5
#define NID_LIB_DISPLAY_USER   0x4FAACD11
#define NID_SET_FRAME_BUF      0x7A410B64
#define NID_LIB_DISPLAY        0x5ED8F994
#define NID_WAIT_VBLANK_MULTI  0xDD0A13B8
#define NID_LIB_KERNEL         0xCAE9ACE6
#define NID_CREATE_THREAD      0xC5C11EE7

#define FONT_W 8
#define FONT_H 14
#define LINE_CHARS 100

#define LOG_DIR   SETTINGS_DIR
#define LOG_FILE  LOG_DIR "/log.csv"
#define DIAG_FILE LOG_DIR "/diag.txt"
#define DIAG_HEARTBEAT_S 180   // heartbeat lines for the first 3 minutes
#define LOG_SLOTS 8
#define LOG_LINE  384

typedef struct {
    uint32_t module_nid;
    int settings_idx;        // GAME_* in settings.h
    uint32_t render_stage;   // seg0 offset of RenderStage(stage)
    uint32_t fm;             // seg1 offset: the frame manager, or a pointer to it
    int fm_is_ptr;
    uint32_t fm_sync_req;    // FM byte the game copies into the catch-up flag
    uint32_t stages;         // seg1 offset: the stage table, or a pointer to it
    int stages_is_ptr;
    int has_profiler;        // FM keeps the X profiler slots
    unsigned char stage_3d, stage_fx, stage_setup;   // stage numbers the presets skip
    unsigned char prof_flag; // stage table byte: GPU profiler (sceGxmFinish around stages)
    uint32_t opt_matmul;     // seg0 offset of the 4x4 matrix multiply (0: off, see opt.c)
    uint32_t probe_ret[3];   // uniform reuse (opt.c): seg0 return offsets of the shader
                             // binding's memcpy, reserve, SetUniformDataF calls (0: unknown)
} game_t;

static const game_t GAMES[] = {
    // X: FM pointer 0x814F756C, stage table pointer 0x814F0208 (seg1 0x814EE000)
    {0x0A3FF027, GAME_X,    0x271368, 0x956C,  1, 0x2D, 0x2208,   1, 1, 3, 2, 0, 0x0C, 0, {0}},
    // f: FM 0x816E7228, stage table 0x818F8B58 (seg1 0x81689000)
    {0x544807B3, GAME_F,    0x28C54E, 0x5E228, 0, 0xEC, 0x26FB58, 0, 0, 8, 7, 5, 0x18, 0, {0}},
    // f 2nd: FM 0x81787B18, stage table 0x81B62800 (seg1 0x8171B000)
    // uniform reuse off (see opt.c); f 2nd's offsets: {0x4DE90A, 0x4DE8B8, 0x4DE8EA}
    {0xDB737975, GAME_F2ND, 0x265920, 0x6CB18, 0, 0xDC, 0x447800, 0, 0, 3, 2, 0, 0x0C, 0, {0}},
};
#define GAME_COUNT (int)(sizeof(GAMES) / sizeof(GAMES[0]))

enum { PROF_UPD, PROF_A, PROF_B, PROF_C, PROF_DRAW, PROF_P, PROF_L, PROF_R, PROF_SWAP, PROF_COUNT };
static const unsigned short PROF_OFF[PROF_COUNT] = {0x60, 0x118, 0x138, 0x148, 0x70, 0x158, 0x178, 0x168, 0xA0};
static const char *const PROF_LABEL[PROF_COUNT] = {
    " upd ", " (a ", " b ", " c ", ") draw ", " (p ", " l ", " r ", ") swap "};

enum { HOOK_STAGE, HOOK_FB, HOOK_BEGIN, HOOK_END, HOOK_DQ, HOOK_VBLANK, HOOK_THREAD, HOOK_COUNT };
static tai_hook_ref_t g_ref[HOOK_COUNT];
static SceUID g_hook[HOOK_COUNT] = {-1, -1, -1, -1, -1, -1, -1};

static const game_t *g_game = NULL;
static uintptr_t g_seg1 = 0;

static SceUID g_thread = -1;
static volatile int g_run = 1;

static volatile unsigned int g_frame = 0;
static volatile int g_preset = 0;
static volatile int g_sync = 0;
static volatile int g_dirty = 0;   // settings changed: restart the perf window
static volatile int g_rec = 0;
static volatile int g_avs = 0;
static volatile int g_bar = 1;
static volatile int g_opt = 1;       // setting: 0 off, 1 on, 2 A/B, 3 STAGE (test)
static volatile int g_opt_on = 1;    // what the hooks use: 0 off, 1 on, 3 STAGE
static int g_window_opt = 1;         // g_opt_on during the last log window
static volatile SceUID g_main_thid = -1;   // the thread that runs RenderStage
static volatile unsigned int g_vb_main = 0, g_vb_other = 0;   // vblank waits per thread kind (diag)
static settings_t g_settings;              // input thread only
static unsigned int g_vc_last = 0;     // vblank count when the last frame started
static unsigned int g_late_total = 0;  // frames whose vblank wait AVS skipped

// log ring: render thread produces at head, input thread writes out at tail
static char g_log_ring[LOG_SLOTS][LOG_LINE];
static int g_log_len[LOG_SLOTS];
static volatile unsigned int g_log_head = 0, g_log_tail = 0;
static uint64_t g_rec_start = 0;
static SceUID g_log_fd = -1;

// diagnostics: hooks only set flags, the input thread writes diag.txt
static volatile int g_cur_stage = -1;   // also read by the profiler
static volatile int g_seen_stage = 0, g_seen_fb = 0;
static SceDisplayFrameBuf g_fb_info;
static uint64_t g_start_time = 0;
static int g_diag_started = 0;

// profiler (divasplit_prof.skprx), input thread only
static uint32_t g_module_nid, g_text_base, g_text_size;
static int g_prof_state = 0;   // 0 off, 1 running, -1 not available this session
static volatile int g_prof_on = 0;

// diag.txt only covers the current game session: the first line truncates it
static void diag(const char *fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    int n = sceClibVsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(buf) - 2)
        n = sizeof(buf) - 2;
    buf[n++] = '\n';
    sceIoMkdir(LOG_DIR, 0777);
    SceUID fd = sceIoOpen(DIAG_FILE, SCE_O_WRONLY | SCE_O_CREAT |
            (g_diag_started ? SCE_O_APPEND : SCE_O_TRUNC), 0666);
    g_diag_started = 1;
    if (fd >= 0) {
        sceIoWrite(fd, buf, n);
        sceIoClose(fd);
    }
}

// perf counters, touched from the game's main thread only
static uint64_t g_gpu_total;      // us spent in the hooked gxm calls, ever
static uint64_t g_vs_total;       // us spent waiting for vblank, ever
static uint64_t g_last_frame;     // time of the last stage 0
static uint64_t g_win_start, g_win_gpu_start, g_win_vs_start;
static unsigned int g_win_late_start;
static uint64_t g_win_3d;
static unsigned int g_win_frames, g_win_3d_n;
static uint32_t g_win_prof[PROF_COUNT];

typedef struct {
    int valid;
    int fps10, cpu10, gpu10, vs10, s3d10;  // tenths
    int late;                              // AVS-skipped waits in the last second
    int prof10[PROF_COUNT];
} perf_t;
static volatile perf_t g_perf;
static probe_stats_t g_probe;          // last window, render thread
static unsigned int g_probe_frames;

// status line, double buffered: the display thread draws g_text[g_text_idx]
static char g_text[2][LINE_CHARS + 1];
static int g_text_len[2];
static volatile int g_text_idx = 0;

static volatile uint8_t *seg1_object(uint32_t off, int is_ptr) {
    if (!g_seg1)
        return NULL;
    if (is_ptr)
        return *(volatile uint8_t **)(g_seg1 + off);
    return (volatile uint8_t *)(g_seg1 + off);
}

static volatile uint8_t *fm_base(void) {
    return seg1_object(g_game->fm, g_game->fm_is_ptr);
}

#define MS(v) (v) / 10, (v) % 10

static const char *game_tag(void) {
    return SETTINGS_GAME_TAG[g_game->settings_idx];
}

static unsigned int preset_mask(const preset_t *p) {
    return (p->skip_3d ? 1u << g_game->stage_3d : 0)
         | (p->skip_fx ? 1u << g_game->stage_fx : 0)
         | (p->skip_setup ? 1u << g_game->stage_setup : 0);
}

static void update_text(void) {
    int i = g_text_idx ^ 1;
    char *t = g_text[i];
    const int size = sizeof(g_text[i]);
    const char *prof = "";
    volatile uint8_t *stages = seg1_object(g_game->stages, g_game->stages_is_ptr);
    if (stages && stages[g_game->prof_flag])
        prof = " PROF";
    int n;
    if (!g_perf.valid) {
        n = sceClibSnprintf(t, size, "%sSPLIT %s  SYNC %s  AVS %s%s", g_rec ? (g_prof_on ? "REC+PROF " : "REC ") : "",
                PRESETS[g_preset].name, g_sync ? "ON" : "OFF", g_avs ? "ON" : "OFF",
                g_opt == 3 ? "  OPT STAGE" : g_opt == 2 ? "  OPT A/B" : g_opt ? "  OPT" : "");
    } else if (g_rec && g_game->has_profiler) {
        // gpu is left out (swap covers it) and l is always 0.0: both still go to the log
        n = sceClibSnprintf(t, size, "REC %s%s%s %2dfps vs %d.%d",
                PRESETS[g_preset].name, g_sync ? " SYNC" : "", g_avs ? " AVS" : "",
                (g_perf.fps10 + 5) / 10, MS(g_perf.vs10));
        for (int k = 0; k < PROF_COUNT && n < LINE_CHARS; k++)
            if (k != PROF_L)
                n += sceClibSnprintf(t + n, size - n, "%s%d.%d", PROF_LABEL[k], MS(g_perf.prof10[k]));
    } else {
        n = sceClibSnprintf(t, size,
                "%sSPLIT %s  SYNC %s  AVS %s%s  %2dfps cpu %d.%d gpu %d.%d vs %d.%d 3d %d.%d%s",
                g_rec ? (g_prof_on ? "REC+PROF " : "REC ") : "", PRESETS[g_preset].name, g_sync ? "ON" : "OFF",
                g_avs ? "ON" : "OFF", g_opt == 3 ? "  OPT STAGE" : g_opt == 2 ? "  OPT A/B" : g_opt ? "  OPT" : "",
                (g_perf.fps10 + 5) / 10, MS(g_perf.cpu10), MS(g_perf.gpu10),
                MS(g_perf.vs10), MS(g_perf.s3d10), prof);
    }
    g_text_len[i] = n > LINE_CHARS ? LINE_CHARS : n;
    g_text_idx = i;
}

static void log_line(uint64_t now) {
    unsigned int head = g_log_head;
    if (head - g_log_tail >= LOG_SLOTS)
        return;   // writer is behind: drop this second
    if (g_rec_start == 0)
        g_rec_start = now;
    unsigned int t10 = (unsigned int)((now - g_rec_start) / 100000);
    char *line = g_log_ring[head % LOG_SLOTS];
    int n = sceClibSnprintf(line, LOG_LINE, "%u.%u,%s,%s,%d,%d,%d,%d,%d.%d,%d.%d,%d.%d,%d.%d,%d.%d",
            t10 / 10, t10 % 10, game_tag(), PRESETS[g_preset].name, g_sync, g_avs, g_window_opt, g_perf.late,
            MS(g_perf.fps10), MS(g_perf.cpu10), MS(g_perf.gpu10), MS(g_perf.vs10), MS(g_perf.s3d10));
    for (int k = 0; k < PROF_COUNT && n < LOG_LINE - 16; k++)
        n += sceClibSnprintf(line + n, LOG_LINE - n, ",%d.%d", MS(g_perf.prof10[k]));
    tv_summary_t tv;
    tv_get(&tv);
    if (tv.valid) {
        n += sceClibSnprintf(line + n, LOG_LINE - n, ",%d.%d,%d.%d,%d.%d",
                MS(tv.run10), MS(tv.ready10), MS(tv.wait10));
        for (int c = 0; c < TV_CORES && n < LOG_LINE - 16; c++)
            n += tv.core10[c] < 0 ? sceClibSnprintf(line + n, LOG_LINE - n, ",")
                                  : sceClibSnprintf(line + n, LOG_LINE - n, ",%d.%d", MS(tv.core10[c]));
    } else {
        n += sceClibSnprintf(line + n, LOG_LINE - n, ",,,,,,,");
    }
    if (g_probe_frames && n < LOG_LINE - 64) {
        const probe_stats_t *p = &g_probe;
        n += sceClibSnprintf(line + n, LOG_LINE - n, ",%u,%u,%u,%u,%u,%u,%u,%u",
                p->calls / g_probe_frames, p->bytes / g_probe_frames,
                p->groups / g_probe_frames, p->group_bytes / g_probe_frames,
                p->same / g_probe_frames, p->same_bytes / g_probe_frames,
                p->setdata, p->overflow);
    }
    line[n++] = '\n';
    g_log_len[head % LOG_SLOTS] = n;
    __sync_synchronize();
    g_log_head = head + 1;
}

static void log_flush(void) {
    if (g_rec && g_log_fd < 0) {
        sceIoMkdir(LOG_DIR, 0777);
        g_log_fd = sceIoOpen(LOG_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
        if (g_log_fd >= 0) {
            static const char hdr[] = "# divasplit 1.9 recording\n"
                "t,game,preset,sync,avs,opt,late,fps,cpu,gpu,vs,3d,upd,a,b,c,draw,p,l,r,swap,"
                "main_run,main_ready,main_wait,core0,core1,core2,core3,"
                "up_copies,up_bytes,up_blocks,up_block_bytes,up_reused,up_reused_bytes,up_setdata,up_overflow\n";
            sceIoWrite(g_log_fd, hdr, sizeof(hdr) - 1);
        }
    }
    while (g_log_tail != g_log_head) {
        __sync_synchronize();
        unsigned int i = g_log_tail % LOG_SLOTS;
        if (g_log_fd >= 0)
            sceIoWrite(g_log_fd, g_log_ring[i], g_log_len[i]);
        g_log_tail++;
    }
    if (!g_rec && g_log_fd >= 0) {
        sceIoClose(g_log_fd);
        g_log_fd = -1;
    }
}

static void window_reset(uint64_t now) {
    g_win_start = now;
    g_win_gpu_start = g_gpu_total;
    g_win_vs_start = g_vs_total;
    g_win_late_start = g_late_total;
    g_win_frames = g_win_3d_n = 0;
    g_win_3d = 0;
    sceClibMemset(g_win_prof, 0, sizeof(g_win_prof));
}

static void frame_boundary(uint64_t now) {
    g_frame++;
    volatile uint8_t *fm = fm_base();
    // the game copies this byte into the active catch-up flag every frame
    if (fm)
        fm[g_game->fm_sync_req] = g_sync ? 1 : 0;

    if (g_last_frame) {
        g_win_frames++;
        // the profiler slots hold the last frame's times
        if (fm && g_game->has_profiler) {
            for (int k = 0; k < PROF_COUNT; k++) {
                uint32_t us = *(const volatile uint32_t *)(fm + PROF_OFF[k]);
                g_win_prof[k] += us < 100000 ? us : 100000;
            }
        }
    }
    g_last_frame = now;

    int dirty = g_dirty;
    if (dirty) {
        probe_stats_t drop;
        probe_take(&drop);   // the new window starts clean
        g_dirty = 0;
        g_perf.valid = 0;   // old numbers belong to the old setting
        update_text();
    }
    uint64_t span = now - g_win_start;
    if (dirty || g_win_start == 0 || span > 5000000) {   // new setting, first frame or long stall
        window_reset(now);
        return;
    }
    if (span < 1000000 || g_win_frames == 0)
        return;

    unsigned int frame_us = (unsigned int)(span / g_win_frames);
    unsigned int gpu_us = (unsigned int)((g_gpu_total - g_win_gpu_start) / g_win_frames);
    unsigned int vs_us = (unsigned int)((g_vs_total - g_win_vs_start) / g_win_frames);
    unsigned int idle_us = gpu_us + vs_us;
    g_perf.fps10 = (int)((uint64_t)g_win_frames * 10000000ull / span);
    g_perf.gpu10 = (int)(gpu_us / 100);
    g_perf.vs10 = (int)(vs_us / 100);
    g_perf.cpu10 = (int)((frame_us > idle_us ? frame_us - idle_us : 0) / 100);
    g_perf.s3d10 = g_win_3d_n ? (int)(g_win_3d / g_win_3d_n / 100) : 0;
    g_perf.late = (int)(g_late_total - g_win_late_start);
    for (int k = 0; k < PROF_COUNT; k++)
        g_perf.prof10[k] = (int)(g_win_prof[k] / g_win_frames / 100);
    probe_take(&g_probe);
    g_probe_frames = g_win_frames;
    // the window that ended ran with g_opt_on; A/B flips it for the next one
    g_window_opt = g_opt_on;
    g_opt_on = g_opt == 2 ? (g_rec ? !g_opt_on : 1) : g_opt;
    g_perf.valid = 1;
    update_text();
    if (g_rec)
        log_line(now);
    else
        g_rec_start = 0;

    window_reset(now);
}

static int render_stage_hook(int stage) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    uint64_t tr0 = trace_on() ? trace_now() : 0;
    g_seen_stage = 1;
    if (stage == 0) {
        static uint64_t frame_start = 0;
        if (tr0 && frame_start)
            trace_add(TR_FRAME, frame_start, tr0, g_frame, 0);
        frame_start = tr0;
        g_main_thid = sceKernelGetThreadId();
        frame_boundary(t0);
    }

    const preset_t *p = &PRESETS[g_preset];
    if (stage >= 0 && stage < STAGE_MAX && (preset_mask(p) & (1u << stage))
            && (p->period == 0 || (g_frame % p->period) != 0))
        return 0;

    uint64_t w0 = g_gpu_total;
    g_cur_stage = stage;
    int ret = TAI_CONTINUE(int, g_ref[HOOK_STAGE], stage);
    g_cur_stage = -1;
    if (tr0)
        trace_add(TR_STAGE, tr0, trace_now(), (uint32_t)stage, 0);
    if (stage == g_game->stage_3d) {
        uint64_t busy = sceKernelGetProcessTimeWide() - t0 - (g_gpu_total - w0);
        g_win_3d += busy;
        g_win_3d_n++;
    }
    return ret;
}

static int begin_scene_hook(void *ctx, unsigned int flags, const void *rt, const void *region,
        void *vsync, void *fsync, const void *color, const void *depth) {
    probe_scene();
    uint64_t t0 = sceKernelGetProcessTimeWide();
    uint64_t tr0 = trace_on() ? trace_now() : 0;
    int ret = TAI_CONTINUE(int, g_ref[HOOK_BEGIN], ctx, flags, rt, region, vsync, fsync, color, depth);
    g_gpu_total += sceKernelGetProcessTimeWide() - t0;
    if (tr0)
        trace_add(TR_GXM_BEGIN, tr0, trace_now(), 0, (uint32_t)__builtin_return_address(0));
    return ret;
}

static int end_scene_hook(void *ctx, const void *vnotif, const void *fnotif) {
    probe_scene();
    uint64_t t0 = sceKernelGetProcessTimeWide();
    uint64_t tr0 = trace_on() ? trace_now() : 0;
    int ret = TAI_CONTINUE(int, g_ref[HOOK_END], ctx, vnotif, fnotif);
    g_gpu_total += sceKernelGetProcessTimeWide() - t0;
    if (tr0)
        trace_add(TR_GXM_END, tr0, trace_now(), 0, (uint32_t)__builtin_return_address(0));
    return ret;
}

static int dq_add_entry_hook(void *old_buf, void *new_buf, const void *data) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    uint64_t tr0 = trace_on() ? trace_now() : 0;
    int ret = TAI_CONTINUE(int, g_ref[HOOK_DQ], old_buf, new_buf, data);
    g_gpu_total += sceKernelGetProcessTimeWide() - t0;
    if (tr0)
        trace_add(TR_GXM_DQ, tr0, trace_now(), 0, (uint32_t)__builtin_return_address(0));
    return ret;
}

static int wait_vblank_multi_hook(unsigned int vcount) {
    SceUID thid = sceKernelGetThreadId();
    if (thid != g_main_thid) {   // e.g. f's loading loop waits 2 vblanks
        static SceUID last_other = -1;
        if (thid != last_other) {
            last_other = thid;
            tv_thread_seen(thid);
        }
        g_vb_other++;
        return TAI_CONTINUE(int, g_ref[HOOK_VBLANK], vcount);
    }
    g_vb_main++;
    if (g_avs && g_vc_last && vcount > 0) {
        unsigned int vc = (unsigned int)sceDisplayGetVcount();
        if (vc - g_vc_last >= vcount) {   // the vblank we would wait for already passed: late
            g_vc_last = vc;
            g_late_total++;
            return 0;
        }
    }
    uint64_t t0 = sceKernelGetProcessTimeWide();
    uint64_t tr0 = trace_on() ? trace_now() : 0;
    int ret = TAI_CONTINUE(int, g_ref[HOOK_VBLANK], vcount);
    g_vs_total += sceKernelGetProcessTimeWide() - t0;
    g_vc_last = (unsigned int)sceDisplayGetVcount();
    if (tr0)
        trace_add(TR_VBLANK, tr0, trace_now(), vcount, (uint32_t)__builtin_return_address(0));
    return ret;
}

static SceUID create_thread_hook(const char *name, SceKernelThreadEntry entry, int prio,
        SceSize stack, SceUInt32 attr, int mask, const SceKernelThreadOptParam *opt) {
    SceUID ret = TAI_CONTINUE(SceUID, g_ref[HOOK_THREAD], name, entry, prio, stack, attr, mask, opt);
    if (ret >= 0)
        tv_thread_seen(ret);
    return ret;
}

// line 0 is the bottom line
static void draw_line(const SceDisplayFrameBuf *fb, const char *text, int len, int line) {
    if (len <= 0 || fb->pixelformat != SCE_DISPLAY_PIXELFORMAT_A8B8G8R8)
        return;
    if ((int)fb->width < len * FONT_W + 8 || (int)fb->height < (line + 1) * (FONT_H + 2) + 8)
        return;
    uint32_t *base = (uint32_t *)fb->base;
    int y0 = fb->height - FONT_H - 4 - line * (FONT_H + 2);
    for (int y = 0; y < FONT_H; y++) {
        uint32_t *row = base + (y0 + y) * fb->pitch + 4;
        for (int c = 0; c < len; c++) {
            unsigned char bits = FONT_TER_U14B[(unsigned char)text[c] * FONT_H + y];
            for (int x = 0; x < FONT_W; x++)
                row[c * FONT_W + x] = ((bits >> (7 - x)) & 1) ? 0xFF00FFFF : 0xFF000000;
        }
    }
}

static void draw_text(const SceDisplayFrameBuf *fb) {
    int idx = g_text_idx;
    draw_line(fb, g_text[idx], g_text_len[idx], 0);
    int len;
    const char *tv = tv_text(&len);
    if (g_rec)
        draw_line(fb, tv, len, 1);
}

static int set_frame_buf_hook(const SceDisplayFrameBuf *pParam, SceDisplaySetBufSync sync) {
    static SceUID last_thid = -1;   // the GXM display queue thread
    SceUID thid = sceKernelGetThreadId();
    if (thid != last_thid) {
        last_thid = thid;
        tv_thread_seen(thid);
    }
    if (!g_seen_fb && pParam) {
        g_fb_info = *pParam;
        g_seen_fb = 1;
    }
    if (g_bar && pParam && pParam->base)
        draw_text(pParam);
    return TAI_CONTINUE(int, g_ref[HOOK_FB], pParam, sync);
}

static void settings_current(game_settings_t *gs) {
    gs->preset = g_preset;
    gs->sync = g_sync;
    gs->avs = g_avs;
    gs->bar = g_bar;
    gs->rec = g_rec;
    gs->opt = g_opt;
}

static void settings_apply(const game_settings_t *gs) {
    g_preset = gs->preset;
    g_sync = gs->sync;
    g_avs = gs->avs;
    g_bar = gs->bar;
    g_rec = gs->rec;
    g_opt = gs->opt;
    g_dirty = 1;   // the render thread redraws the status line
}

// write our current values, keep the other games' lines
static void settings_store(void) {
    settings_load(&g_settings);
    settings_current(&g_settings.game[g_game->settings_idx]);
    g_settings.active = g_game->settings_idx;
    settings_save(&g_settings);
}

// pick up changes made by the Quick Menu plugin
static void settings_poll(void) {
    game_settings_t cur;
    settings_load(&g_settings);
    settings_current(&cur);
    if (!game_settings_equal(&g_settings.game[g_game->settings_idx], &cur))
        settings_apply(&g_settings.game[g_game->settings_idx]);
}

// the profiler runs while recording, when its kernel plugin is installed
// development tools run while recording only when their switch file exists
#define PROF_ON_FILE  LOG_DIR "/prof.on"
#define TRACE_ON_FILE LOG_DIR "/trace.on"

static int file_exists(const char *path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

static void prof_update(void) {
    probe_enable(g_rec);
    if (g_rec && g_prof_state == 0 && g_main_thid > 0 && !file_exists(PROF_ON_FILE))
        g_prof_state = -1;   // not this time
    if (g_rec && g_prof_state == 0 && g_main_thid > 0) {
        prof_args_t a;
        sceClibMemset(&a, 0, sizeof(a));
        a.magic = PROF_MAGIC;
        a.version = PROF_VERSION;
        a.size = sizeof(a);
        // the main thread and the workers it waits for
        a.thids[0] = g_main_thid;
        sceClibStrncpy(a.names[0], "main", PROF_NAME_LEN);
        a.nthreads = 1;
        // every game thread divasplit knows of
        static SceUID ids[PROF_MAX_THREADS];
        static char names[PROF_MAX_THREADS][32];
        int n = tv_threads_named("", ids, names, PROF_MAX_THREADS);
        for (int i = 0; i < n && a.nthreads < PROF_MAX_THREADS; i++) {
            if (ids[i] == g_main_thid)
                continue;
            a.thids[a.nthreads] = ids[i];
            sceClibStrncpy(a.names[a.nthreads], names[i], PROF_NAME_LEN - 1);
            a.nthreads++;
        }
        a.module_nid = g_module_nid;
        a.text_base = g_text_base;
        a.text_size = g_text_size;
        a.frame_addr = (uint32_t)&g_frame;
        a.stage_addr = (uint32_t)&g_cur_stage;
        a.rate_hz = 500;
        for (unsigned int t = 0; t < a.nthreads; t++) {
            SceKernelThreadInfo ti;
            sceClibMemset(&ti, 0, sizeof(ti));
            ti.size = sizeof(ti);
            if (sceKernelGetThreadInfo(a.thids[t], &ti) >= 0)
                a.stack_top[t] = (uint32_t)ti.stack + (uint32_t)ti.stackSize;
        }
        int r = divasplit_prof_start(&a);
        diag("profiler start: %08X (%u threads)", r, (unsigned)a.nthreads);
        g_prof_state = r >= 0 ? 1 : -1;   // not installed or failed: don't retry this session
        g_prof_on = r >= 0;
        g_dirty = 1;
    } else if (!g_rec && g_prof_state == -1) {
        g_prof_state = 0;   // check the switch file again next recording
    } else if (!g_rec && g_prof_state == 1) {
        int n = divasplit_prof_stop();
        diag("profiler stop: %d samples", n);
        g_prof_state = 0;
        g_prof_on = 0;
        g_dirty = 1;
    }
}

static int input_thread(SceSize args, void *argp) {
    unsigned int prev = 0;
    int logged_stage = 0, logged_fb = 0;
    unsigned int next_beat = 5;
    unsigned int tick = 0;
    while (g_run) {
        unsigned int secs = (unsigned int)((sceKernelGetProcessTimeWide() - g_start_time) / 1000000);
        if (g_seen_stage && !logged_stage) {
            logged_stage = 1;
            diag("%us first RenderStage call", secs);
        }
        if (g_seen_fb && !logged_fb) {
            logged_fb = 1;
            diag("%us first SetFrameBuf %ux%u pitch %u fmt %08X base %p", secs,
                    g_fb_info.width, g_fb_info.height, g_fb_info.pitch,
                    (unsigned)g_fb_info.pixelformat, g_fb_info.base);
        }
        if (secs >= next_beat && next_beat <= DIAG_HEARTBEAT_S) {
            next_beat += 5;
            diag("%us frames %u in-stage %d fps %d vblank waits main %u other %u", secs, g_frame,
                    g_cur_stage, g_perf.valid ? (g_perf.fps10 + 5) / 10 : -1, g_vb_main, g_vb_other);
            if (g_game->probe_ret[0]) {
                char line[160];
                probe_diag(line, sizeof(line));
                diag("%us opt %d (%d) %s", secs, g_opt, g_opt_on, line);
            }
        }

        SceCtrlData pad;
        sceClibMemset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~prev;
        prev = pad.buttons;

        if ((pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) == (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) {
            int changed = 0;
            if (pressed & SCE_CTRL_RIGHT) {
                g_preset = (g_preset + 1) % PRESET_COUNT;
                changed = 1;
            }
            if (pressed & SCE_CTRL_UP) {
                g_sync = !g_sync;
                changed = 1;
            }
            if (pressed & SCE_CTRL_LEFT) {
                g_avs = !g_avs;
                changed = 1;
            }
            if (pressed & SCE_CTRL_DOWN) {
                g_rec = !g_rec;
                changed = 1;
            }
            if (pressed & SCE_CTRL_SELECT) {
                static const int next[4] = {3, 2, 0, 1};   // off -> STAGE -> on -> A/B -> off
                g_opt = next[g_opt & 3];
                changed = 1;
            }
            if (changed) {
                g_dirty = 1;   // the render thread redraws the status line
                settings_store();
            }
        } else if (++tick % 10 == 0) {   // every 0.5 s
            settings_poll();
        }
        log_flush();
        tv_set(g_rec, g_main_thid);
        tv_flush();
        prof_update();
        {
            static int trace_rec = -1;   // the switch file, checked when recording starts
            if (g_rec && trace_rec < 0)
                trace_rec = file_exists(TRACE_ON_FILE);
            else if (!g_rec)
                trace_rec = -1;
            trace_set(g_rec && trace_rec == 1);
        }
        {
            int moved = tv_pin(g_opt_on == 1, g_main_thid);
            static int logged = 0;
            if (moved && logged < 6) {
                logged++;
                diag("core pinning %s: %d threads", g_opt_on == 1 ? "on" : "off", moved);
            }
        }
        trace_flush();
        sceKernelDelayThread(50 * 1000);
    }
    return 0;
}

void _start() __attribute__ ((weak, alias("module_start")));
int module_start(SceSize argc, const void *args) {
    tai_module_info_t info;
    info.size = sizeof(info);
    g_start_time = sceKernelGetProcessTimeWide();
    if (taiGetModuleInfo(TAI_MAIN_MODULE, &info) < 0) {
        diag("== divasplit 1.9: taiGetModuleInfo failed");
        return SCE_KERNEL_START_SUCCESS;
    }
    for (int i = 0; i < GAME_COUNT; i++)
        if (GAMES[i].module_nid == info.module_nid)
            g_game = &GAMES[i];
    if (!g_game)
        return SCE_KERNEL_START_SUCCESS;   // not a build we know: do nothing
    diag("== divasplit 1.9: %s nid %08X -> %s", info.name, (unsigned)info.module_nid, game_tag());

    // remembered settings; recording never carries over to a new session
    settings_load(&g_settings);
    g_settings.game[g_game->settings_idx].rec = 0;
    settings_apply(&g_settings.game[g_game->settings_idx]);
    g_settings.active = g_game->settings_idx;
    settings_save(&g_settings);

    SceKernelModuleInfo minfo;
    sceClibMemset(&minfo, 0, sizeof(minfo));
    minfo.size = sizeof(minfo);
    if (sceKernelGetModuleInfo(info.modid, &minfo) >= 0) {
        g_seg1 = (uintptr_t)minfo.segments[1].vaddr;
        g_text_base = (uint32_t)minfo.segments[0].vaddr;
        g_text_size = minfo.segments[0].memsz;
    }
    g_module_nid = info.module_nid;

    g_hook[HOOK_STAGE] = taiHookFunctionOffset(&g_ref[HOOK_STAGE], info.modid, 0, g_game->render_stage, 1, render_stage_hook);
    g_hook[HOOK_FB] = taiHookFunctionImport(&g_ref[HOOK_FB], TAI_MAIN_MODULE, NID_LIB_DISPLAY_USER, NID_SET_FRAME_BUF, set_frame_buf_hook);
    g_hook[HOOK_BEGIN] = taiHookFunctionImport(&g_ref[HOOK_BEGIN], TAI_MAIN_MODULE, NID_LIB_GXM, NID_GXM_BEGIN_SCENE, begin_scene_hook);
    g_hook[HOOK_END] = taiHookFunctionImport(&g_ref[HOOK_END], TAI_MAIN_MODULE, NID_LIB_GXM, NID_GXM_END_SCENE, end_scene_hook);
    g_hook[HOOK_DQ] = taiHookFunctionImport(&g_ref[HOOK_DQ], TAI_MAIN_MODULE, NID_LIB_GXM, NID_GXM_DQ_ADD_ENTRY, dq_add_entry_hook);
    g_hook[HOOK_VBLANK] = taiHookFunctionImport(&g_ref[HOOK_VBLANK], TAI_MAIN_MODULE, NID_LIB_DISPLAY, NID_WAIT_VBLANK_MULTI, wait_vblank_multi_hook);
    g_hook[HOOK_THREAD] = taiHookFunctionImport(&g_ref[HOOK_THREAD], TAI_MAIN_MODULE, NID_LIB_KERNEL, NID_CREATE_THREAD, create_thread_hook);
    diag("seg1 %08X hooks stage %08X fb %08X begin %08X end %08X dq %08X vblank %08X thread %08X",
            (unsigned)g_seg1, g_hook[HOOK_STAGE], g_hook[HOOK_FB], g_hook[HOOK_BEGIN],
            g_hook[HOOK_END], g_hook[HOOK_DQ], g_hook[HOOK_VBLANK], g_hook[HOOK_THREAD]);
    g_opt_on = g_opt == 2 ? 1 : g_opt;
    int opt = opt_init(info.modid, g_game->opt_matmul, &g_opt_on);
    diag("optimizations: matmul %08X", opt);
    // dev tools, with the profiler kernel plugin installed (an unresolved import returns -1)
    if (divasplit_prof_stop() >= 0)
        diag("profiler present, tracer hooks: %d", trace_install());
    if (g_game->probe_ret[0])
        diag("uniform reuse hooks: %08X", probe_install(g_text_base,
                g_game->probe_ret[0], g_game->probe_ret[1], g_game->probe_ret[2]));
    tv_init(diag);

    update_text();
    g_thread = sceKernelCreateThread("divasplit_input", input_thread, 0x10000100, 0x4000, 0, 0, NULL);
    if (g_thread >= 0)
        sceKernelStartThread(g_thread, 0, NULL);
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
    if (g_prof_state == 1)
        divasplit_prof_stop();
    tv_shutdown();
    opt_shutdown();
    trace_shutdown();
    if (g_thread >= 0) {
        g_run = 0;
        sceKernelWaitThreadEnd(g_thread, NULL, NULL);
        sceKernelDeleteThread(g_thread);
    }
    if (g_log_fd >= 0)
        sceIoClose(g_log_fd);
    for (int i = 0; i < HOOK_COUNT; i++)
        if (g_hook[i] >= 0)
            taiHookRelease(g_hook[i], g_ref[i]);
    return SCE_KERNEL_STOP_SUCCESS;
}

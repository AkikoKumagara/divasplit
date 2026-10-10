/*
 * Dynamic resolution (f 2nd): the 3D world's resolution follows the GPU load,
 * full size while the GPU keeps up, lower only while a heavy PV would drop.
 *
 * The engine allocates its 3D render targets at 1024x544 once; the internal
 * resolution is only a size in the render manager (mgr = *0x81B62808), used
 * for viewports and texture coordinates:
 *   +0x38/+0x3C     written every frame by the stage-0 setup seg0+0x25E32E
 *                   (720x408 in a PV, VitaGrafix's IB patch changes those
 *                   constants; 896x512 when byte +0x34 asks for it)
 *   +0x18F4/+0x18F8 FBO 0 (3D world), +0x194C/+0x1950 FBO 11 (tonemap output,
 *   +0x1954/+0x1958 FBO 12 (stage-2 pass): logical sizes, copied from the
 *                   above only when the mode (+0x2C) changes; mode 0 is the PV
 * The setup uses neither after writing them (the rest of it refreshes movie
 * textures with their own sizes), so writing our size right after it returns
 * makes the whole frame use it. Nothing is reallocated.
 *
 * Valid regions (OPT region): the engine begins every scene with none
 * (0x266536 -> sceGxmBeginScene at 0x266584), so the GPU processes and stores
 * all 1024x544 tiles of FBO 0 (4x MSAA F16), 11 and 12 though the image is
 * 720x408. For those three we pass the image's area plus one texel (the
 * stretch's bilinear edge reads it, as stock). The scene object is
 * mgr+0x1074+0x80*i, colour surface at +0x24.
 *
 * Controller, per frame, on the main thread's own timings: a frame is late
 * past 16.7 ms; GPU-bound when it also waited for the GPU (BeginScene /
 * EndScene / display queue) clearly more than the game's floor (its driver
 * work when the GPU keeps up: f ~1 ms, f 2nd ~0.3 ms). Half of 8 frames late
 * and GPU-bound, twice in a row and without a hitch (loading): one step down,
 * more when the GPU wait is long (fewer size changes). A second with little
 * GPU wait over the floor: one step up, whatever the frame rate (a lower
 * resolution doesn't help a frame the CPU makes late), but only after a hold
 * that doubles each time a step up is undone within two seconds (and resets
 * after ten quiet seconds).
 */
#include <psp2/kernel/clib.h>
#include <taihen.h>

#include "drs.h"
#include "opt.h"

#define STEP 32          // width per level
#define LATE_US 17200
// GPU waits are measured above the game's floor: the time its main thread spends in
// the GXM calls when the GPU keeps up (driver work; f 2nd ~0.3 ms, f ~1 ms, 7 scenes)
#define GPU_BOUND_US 2000    // a step down: late frames that also waited this much more for the GPU
#define GPU_FREE_US 800      // a step up: a second averaging less than this over the floor
#define FLOOR_MAX_US 2000    // a wait above this is the GPU's, never the floor
#define FLOOR_RISE_US 25     // per second: the floor follows a wait that grew (scene change)
#define HITCH_US 50000       // a window with a frame this long is a hitch (loading), not GPU load

static tai_hook_ref_t s_ref;
static const drs_game_t *s_game;
static uintptr_t s_seg1;
static uint32_t s_scene_ret, s_d_mgr;
static volatile unsigned int d_regions;
static SceUID s_hook = -1;
static int s_min = 480, s_max = 720;
static int s_req_min, s_req_max;   // opt.txt's (0: the game's default)
static int s_level;              // 0 = s_max, each level STEP narrower
static int s_w;                  // width this frame (0: not applied)
static int s_applied;            // this frame used our size: its timings count
static volatile int s_enabled;   // the setting

static unsigned int f_n, f_late, f_gpu, f_vs, f_max;   // down window (8 frames)
static unsigned int s_floor, s_bad;                  // the GPU wait floor; down windows in a row
static unsigned int u_n, u_late, u_gpu, u_vs;        // up window (60 frames)
static unsigned int s_up_hold = 120, s_since_change, s_since_up = 100000;
static volatile unsigned int d_down, d_up, d_undone, d_minw = 9999, d_maxw;

static int level_w(int level) {
    int w = s_max - level * STEP;
    return w < s_min ? s_min : w;
}

static int levels(void) {
    return (s_max - s_min + STEP - 1) / STEP;   // the last level is s_min
}

static int height_of(int w) {   // the game's shape (f 2nd 30:17 = 720:408), even
    return (w * s_game->aspect_h / s_game->aspect_w + 1) & ~1;
}

// after the game's per-frame setup wrote its size: ours (alt: the special size was asked for)
static void apply(uint8_t *mgr, int alt) {
    const drs_game_t *g = s_game;
    s_applied = 0;
    s_w = 0;
    if (alt || (g->mode && *(volatile int *)(mgr + g->mode) != 0))   // not a PV, or the special size
        return;
    int bw = *(volatile int *)(mgr + g->size), bh = *(volatile int *)(mgr + g->size + 4);   // the engine's size
    int w = bw, h = bh;
    if (s_enabled && opt_cut_on(OPT_DRS)) {
        w = level_w(s_level);
        h = height_of(w);
        if (h > g->max_h) {   // the render targets' height
            h = g->max_h & ~1;
            w = h * g->aspect_w / g->aspect_h;
        }
        s_applied = 1;
    }
    if (w < 16 || h < 16 || w > 1024 || h > g->max_h)
        return;
    volatile int *p = (volatile int *)(mgr + g->size);
    p[0] = w;
    p[1] = h;
    for (int k = 0; g->fbo_size && k < 3; k++) {   // 3D world, tonemap output, stage-2 background
        p = (volatile int *)(mgr + g->fbo_size + 8 * g->fbo[k]);
        p[0] = w;
        p[1] = h;
    }
    s_w = w;
    if (s_applied) {
        if ((unsigned)w < d_minw)
            d_minw = (unsigned)w;
        if ((unsigned)w > d_maxw)
            d_maxw = (unsigned)w;
    }
}

// f 2nd: seg0+0x25E32E(mgr), the stage-0 setup
static void stage0_hook(uint8_t *mgr) {
    int alt = s_game->request ? mgr[s_game->request] : 0;   // the special size, cleared by the setup
    TAI_CONTINUE(void, s_ref, mgr);
    apply(mgr, alt);
}

// f: the setup (0x2828BE) runs in RenderStage(5) and VitaGrafix's IB patch is
// inside it, so main.c calls this before and after that stage instead of a hook
void drs_stage(int stage, int after) {
    static int alt;
    if (!s_game || s_game->stage0 || stage != s_game->stage)
        return;
    uint8_t *mgr = *(uint8_t *const volatile *)(s_seg1 + s_d_mgr);
    if (!mgr)
        return;
    if (!after)
        alt = s_game->request ? mgr[s_game->request] : 0;
    else
        apply(mgr, alt);
}

void drs_frame(unsigned int frame_us, unsigned int gpu_us, unsigned int vs_us) {
    if (!s_applied || frame_us > 200000) {   // not ours, or a stall (loading)
        f_n = u_n = 0;
        f_late = u_late = f_gpu = u_gpu = f_vs = u_vs = f_max = s_bad = 0;
        return;
    }
    s_since_change++;
    s_since_up++;
    int late = frame_us > LATE_US;
    f_n++, f_late += late, f_gpu += gpu_us, f_vs += vs_us;
    u_n++, u_late += late, u_gpu += gpu_us, u_vs += vs_us;
    if (frame_us > f_max)
        f_max = frame_us;
    if (f_n >= 8) {
        unsigned int gpu = f_gpu / f_n, over = gpu > s_floor ? gpu - s_floor : 0;
        // GPU-bound for two windows in a row, without a hitch in them
        s_bad = f_late >= 4 && over > GPU_BOUND_US && f_max < HITCH_US ? s_bad + 1 : 0;
        if (s_bad >= 2 && s_level < levels()) {
            // a step per 4 ms of GPU wait (32 px of width is ~9% of the pixels at 720)
            int steps = 1 + (int)(over / 4000);
            s_bad = 0;
            s_level = s_level + steps > levels() ? levels() : s_level + steps;
            d_down++;
            if (s_since_up < 120) {   // the last step up didn't hold
                d_undone++;
                if (s_up_hold < 1920)
                    s_up_hold *= 2;
            }
            s_since_change = 0;
            s_since_up = 100000;
            u_n = u_late = u_gpu = u_vs = 0;
        }
        f_n = f_late = f_gpu = f_vs = f_max = 0;
    }
    if (u_n >= 60) {
        unsigned int avg = u_gpu / u_n;
        // the floor: the lowest second seen, rising slowly to follow the game
        s_floor = !s_floor || avg < s_floor + FLOOR_RISE_US ? avg : s_floor + FLOOR_RISE_US;
        if (s_floor > FLOOR_MAX_US)
            s_floor = FLOOR_MAX_US;
        if (avg < s_floor + GPU_FREE_US && s_level > 0 && s_since_change >= s_up_hold) {
            s_level--;
            d_up++;
            s_since_change = 0;
            s_since_up = 0;
        } else if (s_since_change > 600 && s_up_hold > 120) {
            s_up_hold = 120;   // ten quiet seconds: try again sooner
        }
        u_n = u_late = u_gpu = u_vs = 0;
    }
}

void drs_set_enabled(int on) {
    s_enabled = on;
}

// defaults: stock width at the top, two thirds of it at the bottom (f 2nd 480..720, f 448..640)
static void resolve_range(void) {
    int stock = s_game ? s_game->stock_w : 720, cap = s_game ? s_game->max_w : 960;
    int min_w = s_req_min > 0 ? s_req_min : (stock * 2 / 3 + 31) & ~31;
    int max_w = s_req_max > 0 ? s_req_max : stock;
    if (min_w < 320)
        min_w = 320;
    if (max_w > cap)
        max_w = cap;
    if (max_w < min_w)
        max_w = min_w;
    s_min = min_w;
    s_max = max_w;
    if (s_level > levels())
        s_level = levels();
}

void drs_set_range(int min_w, int max_w) {
    s_req_min = min_w;
    s_req_max = max_w;
    resolve_range();
}

int drs_width(void) {
    return s_w;
}

int drs_scene_region(uint32_t ret_addr, const void *color, unsigned int *xmax, unsigned int *ymax) {
    if (ret_addr != s_scene_ret || !color || !opt_cut_on(OPT_REGION))
        return 0;
    const uint8_t *mgr = *(const uint8_t *const volatile *)(s_seg1 + s_d_mgr);
    if (!mgr)
        return 0;
    const drs_game_t *g = s_game;
    int off = (int)((const uint8_t *)color - g->color - (mgr + g->scene));
    if (off < 0 || off % (int)g->stride != 0)
        return 0;
    int i = off / (int)g->stride;
    if (i != g->fbo[0] && i != g->fbo[1] && i != g->fbo[2])
        return 0;
    uint32_t so = g->fbo_size ? g->fbo_size + 8 * i : g->size;   // f: one size for all three
    int w = *(const volatile int *)(mgr + so), h = *(const volatile int *)(mgr + so + 4);
    if (w < 16 || h < 16 || w >= 1024 || h >= g->max_h)
        return 0;   // the whole texture anyway
    *xmax = (unsigned int)w;   // inclusive: the image and one texel past it
    *ymax = (unsigned int)h;
    d_regions++;
    return 1;
}

int drs_init(SceUID modid, uintptr_t seg0, uintptr_t seg1, const drs_game_t *g) {
    s_game = g;
    s_seg1 = seg1;
    s_scene_ret = (uint32_t)(seg0 + g->scene_ret) | 1;
    s_d_mgr = g->d_mgr;
    resolve_range();
    if (!g->stage0)
        return 0;   // driven from main.c's RenderStage hook (drs_stage)
    s_hook = taiHookFunctionOffset(&s_ref, modid, 0, g->stage0, 1, stage0_hook);
    return s_hook;
}

void drs_shutdown(void) {
    if (s_hook >= 0)
        taiHookRelease(s_hook, s_ref);
    s_hook = -1;
}

int drs_diag(char *buf, int size) {
    return sceClibSnprintf(buf, size, "drs: hook %08X range %d..%d now %dx%d level %d; down %u up %u undone %u"
            " (up hold %u frames), GPU wait floor %u us, widths seen %u..%u; regions %u",
            s_hook, s_min, s_max, s_w, s_w ? height_of(s_w) : 0, s_level, d_down, d_up, d_undone, s_up_hold,
            s_floor, d_minw == 9999 ? 0 : d_minw, d_maxw, d_regions);
}

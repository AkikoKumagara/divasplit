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
 * EndScene / display queue). Half of 8 frames late and GPU-bound: one step
 * down at once, more when the GPU wait is long (fewer size changes). A second
 * with little GPU wait: one step up, whatever the frame rate (a lower
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
#define GPU_BOUND_US 2000    // a step down: late frames that also waited this long for the GPU
#define GPU_FREE_US 800      // a step up: a second averaging less GPU wait than this

static tai_hook_ref_t s_ref;
static const drs_game_t *s_game;
static uintptr_t s_seg1;
static uint32_t s_scene_ret, s_d_mgr;
static volatile unsigned int d_regions;
static SceUID s_hook = -1;
static int s_min = 480, s_max = 720;
static int s_level;              // 0 = s_max, each level STEP narrower
static int s_w;                  // width this frame (0: not applied)
static int s_applied;            // this frame used our size: its timings count
static volatile int s_enabled;   // the setting

static unsigned int f_n, f_late, f_gpu, f_vs;        // down window (8 frames)
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

static int height_of(int w) {
    return (w * 17 / 30 + 1) & ~1;   // 960:544 = 720:408 = 30:17, even
}

// seg0+0x25E32E(mgr), the stage-0 setup
static void stage0_hook(uint8_t *mgr) {
    const drs_game_t *g = s_game;
    int alt = g->request ? mgr[g->request] : 0;   // the special size (f 2nd 896x512), cleared by the setup
    TAI_CONTINUE(void, s_ref, mgr);
    s_applied = 0;
    s_w = 0;
    if (alt || *(volatile int *)(mgr + g->mode) != 0)   // not a PV, or the special size
        return;
    int bw = *(volatile int *)(mgr + g->size), bh = *(volatile int *)(mgr + g->size + 4);   // the engine's size
    int w = bw, h = bh;
    if (s_enabled && opt_cut_on(OPT_DRS)) {
        w = level_w(s_level);
        h = height_of(w);
        s_applied = 1;
    }
    if (w < 16 || h < 16 || w > 1024 || h > 544)
        return;
    volatile int *p = (volatile int *)(mgr + g->size);
    p[0] = w;
    p[1] = h;
    for (int k = 0; k < 3; k++) {   // 3D world, tonemap output, stage-2 background
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

void drs_frame(unsigned int frame_us, unsigned int gpu_us, unsigned int vs_us) {
    if (!s_applied || frame_us > 200000) {   // not ours, or a stall (loading)
        f_n = u_n = 0;
        f_late = u_late = f_gpu = u_gpu = f_vs = u_vs = 0;
        return;
    }
    s_since_change++;
    s_since_up++;
    int late = frame_us > LATE_US;
    f_n++, f_late += late, f_gpu += gpu_us, f_vs += vs_us;
    u_n++, u_late += late, u_gpu += gpu_us, u_vs += vs_us;
    if (f_n >= 8) {
        unsigned int gpu = f_gpu / f_n;
        if (f_late >= 4 && gpu > GPU_BOUND_US && s_level < levels()) {
            // a step per 4 ms of GPU wait (32 px of width is ~9% of the pixels at 720)
            int steps = 1 + (int)(gpu / 4000);
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
        f_n = f_late = f_gpu = f_vs = 0;
    }
    if (u_n >= 60) {
        if (u_gpu / u_n < GPU_FREE_US && s_level > 0 && s_since_change >= s_up_hold) {
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

void drs_set_range(int min_w, int max_w) {
    if (min_w < 320)
        min_w = 320;
    if (max_w > 960)
        max_w = 960;
    if (max_w < min_w)
        max_w = min_w;
    s_min = min_w;
    s_max = max_w;
    if (s_level > levels())
        s_level = levels();
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
    if (off < 0 || (off & 0x7F) != 0)
        return 0;
    int i = off >> 7;
    if (i != g->fbo[0] && i != g->fbo[1] && i != g->fbo[2])
        return 0;
    int w = *(const volatile int *)(mgr + g->fbo_size + 8 * i), h = *(const volatile int *)(mgr + g->fbo_size + 4 + 8 * i);
    if (w < 16 || h < 16 || w >= 1024 || h >= 544)
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
            " (up hold %u frames), widths seen %u..%u; regions %u",
            s_hook, s_min, s_max, s_w, s_w ? height_of(s_w) : 0, s_level, d_down, d_up, d_undone, s_up_hold,
            d_minw == 9999 ? 0 : d_minw, d_maxw, d_regions);
}

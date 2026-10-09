/*
 * Engine optimizations: faster replacements for hot game functions found with
 * the profiler (tools/profview.py). Each gives the same results as the game's
 * own code. The "opt" setting (L+R+SELECT, Quick Menu) switches them off, to
 * compare.
 *
 *   matmul  4x4 float matrix multiply, out = A * B (row major). The game does
 *           it with 64 scalar VFP multiply-adds; NEON does it with 16. Each
 *           element is summed in the game's order (A[i][1]*B[1][j] first, then
 *           columns 0, 2, 3) with the same non-fused multiply-add, so the
 *           results are bit-identical. All inputs are loaded before any store,
 *           as in the game, so out may be A or B.
 *           f: seg0+0x3CCB02, f 2nd: seg0+0x4D0FDE. About 0.4-0.6 ms per frame
 *           of the f 2nd 3D stage.
 *
 *           Measured on f 2nd: SLOWER, 1.26 ms per frame against the game's
 *           ~0.8. The function is tiny and called thousands of times a frame,
 *           so memory traffic dominates: the 128-bit NEON load of the input
 *           waits for the game's 32-bit VFP stores that just wrote it, and the
 *           compiler's stack copy of the result stalls the same way. Not
 *           enabled for any game (opt_matmul = 0 in GAMES[]).
 */
#include <arm_neon.h>
#include <taihen.h>

#include "opt.h"

static tai_hook_ref_t s_matmul_ref;
static SceUID s_matmul_hook = -1;
static volatile int *s_on;

/*
 * probe: counts the memcpy calls the shader binding makes (f 2nd seg0+0x4DE906,
 * the uniform upload; 1.5 ms per frame in the profile). Hooks the eboot's
 * memcpy import only once recording starts; every other call goes straight on.
 */
#define NID_LIB_LIBC  0xBE43BB07
#define NID_MEMCPY    0x7205BFDB

static tai_hook_ref_t s_memcpy_ref;
static SceUID s_memcpy_hook = -1;
static uint32_t s_probe_ret;
static volatile probe_stats_t s_probe;

static void *memcpy_hook(void *dst, const void *src, unsigned int n) {
    if ((uint32_t)__builtin_return_address(0) == s_probe_ret) {
        uint32_t d = (uint32_t)dst;
        s_probe.calls++;
        s_probe.bytes += n;
        s_probe.hist[n <= 16 ? 0 : n <= 64 ? 1 : n <= 256 ? 2 : n <= 1024 ? 3 : 4]++;
        if (d < s_probe.dst_min)
            s_probe.dst_min = d;
        if (d > s_probe.dst_max)
            s_probe.dst_max = d;
        if (!(d & 15))
            s_probe.dst_aligned16++;
    }
    return TAI_CONTINUE(void *, s_memcpy_ref, dst, src, n);
}

int probe_start(uint32_t text_base, uint32_t ret_off) {
    if (s_memcpy_hook >= 0 || !ret_off)
        return s_memcpy_hook;
    s_probe.dst_min = 0xFFFFFFFF;
    s_probe_ret = (text_base + ret_off) | 1;   // Thumb return address
    s_memcpy_hook = taiHookFunctionImport(&s_memcpy_ref, TAI_MAIN_MODULE, NID_LIB_LIBC, NID_MEMCPY, memcpy_hook);
    return s_memcpy_hook;
}

void probe_take(probe_stats_t *out) {
    out->calls = s_probe.calls;
    out->bytes = s_probe.bytes;
    for (int i = 0; i < 5; i++) {
        out->hist[i] = s_probe.hist[i];
        s_probe.hist[i] = 0;
    }
    out->dst_min = s_probe.dst_min;
    out->dst_max = s_probe.dst_max;
    out->dst_aligned16 = s_probe.dst_aligned16;
    s_probe.calls = s_probe.bytes = s_probe.dst_aligned16 = 0;
    s_probe.dst_min = 0xFFFFFFFF;
    s_probe.dst_max = 0;
}

static float *matmul_neon(float *o, const float *a, const float *b) {
    float32x4_t b0 = vld1q_f32(b), b1 = vld1q_f32(b + 4), b2 = vld1q_f32(b + 8), b3 = vld1q_f32(b + 12);
    float32x4_t r[4];
    for (int i = 0; i < 4; i++) {
        float32x4_t row = vld1q_f32(a + 4 * i);
        float32x2_t lo = vget_low_f32(row), hi = vget_high_f32(row);
        float32x4_t s = vmulq_lane_f32(b1, lo, 1);
        s = vmlaq_lane_f32(s, b0, lo, 0);
        s = vmlaq_lane_f32(s, b2, hi, 0);
        r[i] = vmlaq_lane_f32(s, b3, hi, 1);
    }
    for (int i = 0; i < 4; i++)
        vst1q_f32(o + 4 * i, r[i]);
    return o;
}

static float *matmul_hook(float *o, const float *a, const float *b) {
    if (!*s_on)
        return TAI_CONTINUE(float *, s_matmul_ref, o, a, b);
    return matmul_neon(o, a, b);
}

int opt_init(SceUID modid, uint32_t matmul_off, volatile int *on) {
    s_on = on;
    if (matmul_off)
        s_matmul_hook = taiHookFunctionOffset(&s_matmul_ref, modid, 0, matmul_off, 1, matmul_hook);
    return s_matmul_hook;
}

void opt_shutdown(void) {
    if (s_matmul_hook >= 0)
        taiHookRelease(s_matmul_hook, s_matmul_ref);
    s_matmul_hook = -1;
    if (s_memcpy_hook >= 0)
        taiHookRelease(s_memcpy_hook, s_memcpy_ref);
    s_memcpy_hook = -1;
}

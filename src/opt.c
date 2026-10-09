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
 */
#include <arm_neon.h>
#include <taihen.h>

#include "opt.h"

static tai_hook_ref_t s_matmul_ref;
static SceUID s_matmul_hook = -1;
static volatile int *s_on;

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
}

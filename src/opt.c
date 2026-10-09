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
 * probe: what the shader binding (f 2nd seg0+0x4DE828) uploads per draw. It
 * reserves the vertex default uniform buffer, then fills it parameter by
 * parameter with memcpy (or sceGxmSetUniformDataF). The profile puts the
 * memcpys at 1.5 ms per frame: about 850 draws and 850 KB a frame, written to
 * uncached GPU memory.
 *
 * Every block is hashed; "same" counts blocks identical to the previous one in
 * the same scene, which could point at the previous buffer
 * (sceGxmSetVertexDefaultUniformBuffer) instead of being copied again.
 * Hooks go in once recording starts; other callers go straight on.
 */
#define NID_LIB_LIBC       0xBE43BB07
#define NID_MEMCPY         0x7205BFDB
#define NID_LIB_GXM        0xF76B66BD
#define NID_RESERVE_VDUB   0x97118913
#define NID_SET_DATA_F     0x65DD0C84

enum { PH_MEMCPY, PH_RESERVE, PH_SETDATA, PH_COUNT };
static tai_hook_ref_t s_probe_ref[PH_COUNT];
static SceUID s_probe_hook[PH_COUNT] = {-1, -1, -1};
static uint32_t s_ret[PH_COUNT];
static volatile probe_stats_t s_probe;

// block being filled, and the previous one
static uint32_t s_cur_hash, s_cur_bytes, s_prev_hash, s_prev_bytes;
static uint32_t s_cur_buf;   // the reserved buffer
static int s_cur_open;

static uint32_t hash_bytes(uint32_t h, const void *p, unsigned int n) {
    if (!((uint32_t)p & 3)) {
        const uint32_t *w = p;
        for (; n >= 4; n -= 4)
            h = (h ^ *w++) * 0x01000193;
        p = w;
    }
    const uint8_t *b = p;
    while (n--)
        h = (h ^ *b++) * 0x01000193;
    return h;
}

static void block_close(void) {
    if (!s_cur_open)
        return;
    s_cur_open = 0;
    if (!s_cur_bytes)
        return;
    s_probe.groups++;
    s_probe.group_bytes += s_cur_bytes;
    if (s_cur_hash == s_prev_hash && s_cur_bytes == s_prev_bytes) {
        s_probe.same++;
        s_probe.same_bytes += s_cur_bytes;
    }
    s_prev_hash = s_cur_hash;
    s_prev_bytes = s_cur_bytes;
}

void probe_scene(void) {
    block_close();
    s_prev_hash = s_prev_bytes = 0;
}

static void *memcpy_hook(void *dst, const void *src, unsigned int n) {
    if ((uint32_t)__builtin_return_address(0) == s_ret[PH_MEMCPY]) {
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
        if (s_cur_open) {
            s_cur_hash = hash_bytes(s_cur_hash ^ (d - s_cur_buf), src, n);   // offset in the block
            s_cur_bytes += n;
        }
    }
    return TAI_CONTINUE(void *, s_probe_ref[PH_MEMCPY], dst, src, n);
}

static int reserve_hook(void *ctx, void **buf) {
    int ours = (uint32_t)__builtin_return_address(0) == s_ret[PH_RESERVE];
    if (ours)
        block_close();
    int ret = TAI_CONTINUE(int, s_probe_ref[PH_RESERVE], ctx, buf);
    if (ours && ret >= 0) {
        s_cur_open = 1;
        s_cur_hash = 0x811C9DC5;
        s_cur_bytes = 0;
        s_cur_buf = (uint32_t)*buf;
    }
    return ret;
}

static int setdata_hook(void *buf, const void *param, unsigned int offset, unsigned int count, const float *src) {
    if ((uint32_t)__builtin_return_address(0) == s_ret[PH_SETDATA] && s_cur_open) {
        s_cur_hash = hash_bytes(s_cur_hash ^ (uint32_t)param ^ (offset << 16), src, count * 4);
        s_cur_bytes += count * 4;
    }
    return TAI_CONTINUE(int, s_probe_ref[PH_SETDATA], buf, param, offset, count, src);
}

int probe_start(uint32_t text_base, uint32_t memcpy_ret, uint32_t reserve_ret, uint32_t setdata_ret) {
    if (s_probe_hook[PH_MEMCPY] >= 0 || !memcpy_ret)
        return s_probe_hook[PH_MEMCPY];
    s_probe.dst_min = 0xFFFFFFFF;
    s_ret[PH_MEMCPY] = (text_base + memcpy_ret) | 1;   // Thumb return addresses
    s_ret[PH_RESERVE] = (text_base + reserve_ret) | 1;
    s_ret[PH_SETDATA] = (text_base + setdata_ret) | 1;
    s_probe_hook[PH_RESERVE] = taiHookFunctionImport(&s_probe_ref[PH_RESERVE], TAI_MAIN_MODULE,
            NID_LIB_GXM, NID_RESERVE_VDUB, reserve_hook);
    s_probe_hook[PH_SETDATA] = taiHookFunctionImport(&s_probe_ref[PH_SETDATA], TAI_MAIN_MODULE,
            NID_LIB_GXM, NID_SET_DATA_F, setdata_hook);
    s_probe_hook[PH_MEMCPY] = taiHookFunctionImport(&s_probe_ref[PH_MEMCPY], TAI_MAIN_MODULE,
            NID_LIB_LIBC, NID_MEMCPY, memcpy_hook);
    return s_probe_hook[PH_MEMCPY];
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
    out->groups = s_probe.groups;
    out->group_bytes = s_probe.group_bytes;
    out->same = s_probe.same;
    out->same_bytes = s_probe.same_bytes;
    s_probe.calls = s_probe.bytes = s_probe.dst_aligned16 = 0;
    s_probe.groups = s_probe.group_bytes = s_probe.same = s_probe.same_bytes = 0;
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
    for (int i = 0; i < PH_COUNT; i++) {
        if (s_probe_hook[i] >= 0)
            taiHookRelease(s_probe_hook[i], s_probe_ref[i]);
        s_probe_hook[i] = -1;
    }
}

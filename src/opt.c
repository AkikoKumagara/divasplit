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
 *
 *   uniform reuse  The shader binding (f 2nd seg0+0x4DE828) reserves the vertex
 *           default uniform buffer and fills it with memcpys from the engine's
 *           uniform table: ~850 copies, ~850 KB a frame into uncached GPU
 *           memory, 1.5 ms of main thread time. 54% of those blocks are
 *           byte-identical to the previous draw's (bone palettes uploaded again
 *           for every part of a character).
 *           With opt on, that reservation hands the game a cached staging
 *           buffer instead. At the draw, the block is compared with the
 *           previous one: identical blocks point the draw at the previous GPU
 *           copy (sceGxmSetVertexDefaultUniformBuffer), others are reserved and
 *           copied once. Reuse never crosses a scene (BeginScene / EndScene),
 *           whose end lets GXM recycle the ring buffer.
 *           The hooks go in at game start: patching these imports while the
 *           game calls them ~2000 times a frame crashed it.
 *
 *           Measured on f 2nd (A/B, same run): it works (93 of 175 blocks and
 *           457 KB a frame reused, no glitches) but is ~0.8 ms a frame SLOWER.
 *           The game's memcpys got 0.84 ms cheaper, the draw hook (compare +
 *           one copy into GPU memory) cost 1.68 ms. On the Vita a copy into
 *           cached memory (1.48 ms for 850 KB) is barely cheaper than into the
 *           uncached ring, so checking that data is unchanged costs about as
 *           much as writing it. Off for every game (probe_ret = {0}).
 */
#include <arm_neon.h>
#include <psp2/gxm.h>
#include <psp2/kernel/clib.h>
#include <taihen.h>

#include "opt.h"

static tai_hook_ref_t s_matmul_ref;
static SceUID s_matmul_hook = -1;
static volatile int *s_on;

/* uniform reuse */

#define NID_LIB_LIBC       0xBE43BB07
#define NID_MEMCPY         0x7205BFDB
#define NID_LIB_GXM        0xF76B66BD
#define NID_RESERVE_VDUB   0x97118913
#define NID_SET_DATA_F     0x65DD0C84
#define NID_DRAW           0xBC059AFC

#define STAGE_BYTES  0x8000   // largest block seen is a few KB

enum { UH_MEMCPY, UH_RESERVE, UH_SETDATA, UH_DRAW, UH_COUNT };
static tai_hook_ref_t s_uref[UH_COUNT];
static SceUID s_uhook[UH_COUNT] = {-1, -1, -1, -1};
static uint32_t s_ret[3];                  // memcpy, reserve, SetUniformDataF return addresses
static volatile probe_stats_t s_stats;
static volatile int s_count;               // keep stats (recording)

// main thread only
static uint8_t s_stage[2][STAGE_BYTES] __attribute__((aligned(64)));
static int s_cur;                          // s_stage index being filled
static int s_staging;                      // a block is in s_stage[s_cur], not uploaded yet
static void *s_staging_ctx;
static uint32_t s_used;                    // bytes of the block written so far
static void *s_prev_gpu;                   // GPU copy of s_stage[!s_cur], this scene
static uint32_t s_prev_used;

// diagnostics (cumulative, for diag.txt)
static volatile uint32_t d_commits, d_reused, d_reserve_fail, d_discards, d_max_used;
static volatile uint32_t d_overflow;
static volatile int d_reserve_err;

static int same_bytes(const uint8_t *a, const uint8_t *b, uint32_t n) {
    const uint32_t *x = (const uint32_t *)a, *y = (const uint32_t *)b;
    for (uint32_t i = 0; i < n / 4; i++)
        if (x[i] != y[i])
            return 0;
    for (uint32_t i = n & ~3u; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

// no reuse across scenes, and a reservation we didn't stage replaces ours
static void reuse_reset(void) {
    if (s_staging)
        d_discards++;
    s_staging = 0;
    s_prev_gpu = NULL;
}

void probe_scene(void) {
    reuse_reset();
}

static void *memcpy_hook(void *dst, const void *src, unsigned int n) {
    if ((uint32_t)__builtin_return_address(0) == s_ret[0]) {
        if (s_count) {
            s_stats.calls++;
            s_stats.bytes += n;
        }
        if (s_staging) {
            uint32_t end = (uint32_t)dst - (uint32_t)s_stage[s_cur] + n;
            if (end > STAGE_BYTES) {   // can't happen with the blocks seen; never write past
                s_stats.overflow++;
                d_overflow++;
                return dst;
            }
            if (end > s_used)
                s_used = end;
        }
    }
    return TAI_CONTINUE(void *, s_uref[UH_MEMCPY], dst, src, n);
}

static int reserve_hook(SceGxmContext *ctx, void **buf) {
    if ((uint32_t)__builtin_return_address(0) != s_ret[1] || !*s_on) {
        reuse_reset();
        return TAI_CONTINUE(int, s_uref[UH_RESERVE], ctx, buf);
    }
    // the shader binding: let it fill the staging buffer
    s_staging = 1;
    s_staging_ctx = ctx;
    s_used = 0;
    *buf = s_stage[s_cur];
    return 0;
}

static int setdata_hook(void *buf, const SceGxmProgramParameter *param, unsigned int offset,
        unsigned int count, const float *src) {
    if (s_staging && buf == s_stage[s_cur]) {
        // a float parameter's data starts at its resource index, in 32-bit units
        uint32_t end = (sceGxmProgramParameterGetResourceIndex(param) + offset + count) * 4;
        s_stats.setdata++;
        if (end > STAGE_BYTES) {
            s_stats.overflow++;
            return 0;
        }
        if (end > s_used)
            s_used = end;
    }
    return TAI_CONTINUE(int, s_uref[UH_SETDATA], buf, param, offset, count, src);
}

static int draw_hook(SceGxmContext *ctx, SceGxmPrimitiveType type, SceGxmIndexFormat format,
        const void *indices, unsigned int count) {
    if (s_staging && ctx == s_staging_ctx) {
        s_staging = 0;
        // not "cur" / "next": TAI_CONTINUE declares those and would shadow them
        uint8_t *blk = s_stage[s_cur];
        if (s_count) {
            s_stats.groups++;
            s_stats.group_bytes += s_used;
        }
        if (s_used > d_max_used)
            d_max_used = s_used;
        if (*s_on != 3 && s_prev_gpu && s_used == s_prev_used && same_bytes(blk, s_stage[s_cur ^ 1], s_used)) {
            sceGxmSetVertexDefaultUniformBuffer(ctx, s_prev_gpu);
            d_reused++;
            if (s_count) {
                s_stats.same++;
                s_stats.same_bytes += s_used;
            }
        } else {
            void *gpu = NULL;
            int r = TAI_CONTINUE(int, s_uref[UH_RESERVE], ctx, &gpu);
            if (r >= 0 && gpu) {
                if (s_used)
                    TAI_CONTINUE(void *, s_uref[UH_MEMCPY], gpu, blk, s_used);
                d_commits++;
                s_prev_gpu = gpu;
                s_prev_used = s_used;
                s_cur ^= 1;   // this block becomes the one to compare with
            } else {
                d_reserve_fail++;
                d_reserve_err = r;
                s_prev_gpu = NULL;
            }
        }
    }
    return TAI_CONTINUE(int, s_uref[UH_DRAW], ctx, type, format, indices, count);
}

int probe_install(uint32_t text_base, uint32_t memcpy_ret, uint32_t reserve_ret, uint32_t setdata_ret) {
    if (s_uhook[UH_MEMCPY] >= 0 || !memcpy_ret)
        return s_uhook[UH_MEMCPY];
    s_ret[0] = (text_base + memcpy_ret) | 1;   // Thumb return addresses
    s_ret[1] = (text_base + reserve_ret) | 1;
    s_ret[2] = (text_base + setdata_ret) | 1;
    s_uhook[UH_DRAW] = taiHookFunctionImport(&s_uref[UH_DRAW], TAI_MAIN_MODULE, NID_LIB_GXM, NID_DRAW, draw_hook);
    s_uhook[UH_SETDATA] = taiHookFunctionImport(&s_uref[UH_SETDATA], TAI_MAIN_MODULE, NID_LIB_GXM, NID_SET_DATA_F, setdata_hook);
    s_uhook[UH_MEMCPY] = taiHookFunctionImport(&s_uref[UH_MEMCPY], TAI_MAIN_MODULE, NID_LIB_LIBC, NID_MEMCPY, memcpy_hook);
    // last: until it is in, nothing is staged
    s_uhook[UH_RESERVE] = taiHookFunctionImport(&s_uref[UH_RESERVE], TAI_MAIN_MODULE, NID_LIB_GXM, NID_RESERVE_VDUB, reserve_hook);
    for (int i = 0; i < UH_COUNT; i++)
        if (s_uhook[i] < 0)
            return s_uhook[i];
    return s_uhook[UH_RESERVE];
}

int probe_diag(char *buf, int size) {
    return sceClibSnprintf(buf, size,
            "uniform: commits %u reused %u reserve-fail %u (%08X) discards %u max %u overflow %u",
            d_commits, d_reused, d_reserve_fail, (unsigned)d_reserve_err, d_discards, d_max_used, d_overflow);
}

void probe_enable(int on) {
    s_count = on;
}

void probe_take(probe_stats_t *out) {
    out->calls = s_stats.calls;
    out->bytes = s_stats.bytes;
    out->groups = s_stats.groups;
    out->group_bytes = s_stats.group_bytes;
    out->same = s_stats.same;
    out->same_bytes = s_stats.same_bytes;
    out->setdata = s_stats.setdata;
    out->overflow = s_stats.overflow;
    s_stats.calls = s_stats.bytes = s_stats.groups = s_stats.group_bytes = 0;
    s_stats.same = s_stats.same_bytes = s_stats.setdata = 0;
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
    for (int i = UH_COUNT - 1; i >= 0; i--) {
        if (s_uhook[i] >= 0)
            taiHookRelease(s_uhook[i], s_uref[i]);
        s_uhook[i] = -1;
    }
}

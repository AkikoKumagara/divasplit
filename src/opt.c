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
 *
 * f 2nd main-thread cuts (docs/engine-notes.md "Main-thread cuts"; each one is
 * a bit of the mask in ux0:data/divasplit/opt.txt, OPT switches the set):
 *
 *   centre  The 3D sort keys (seg0+0x245CDC) average a skinned object's centre
 *           over its bones: per bone two 4x4 products and a transform,
 *           Wi * B_i * (W * X) * c. That is linear in B_i, so summing the bone
 *           matrices first and doing the products once gives the same average
 *           (up to float rounding: keys move by a few ulps; the list order was
 *           already nondeterministic, the task workers fill it in parallel).
 *           One call in 64 (every call with check=1 in opt.txt) also runs the
 *           game's version and records the largest difference (diag.txt).
 *   dirty   seg0+0x4DEF04, once per draw flush, clears 224 dirty-flag bytes one
 *           by one ("if (b) b = 0"). Patched (once, at game start) to call
 *           dirty_clear, which does one memset of the same bytes with the cut
 *           on (the same memory afterwards) and the game's loop with it off.
 *   lsort   std::list::sort of the draw lists (seg0+0x5ABB9C) allocates 27 list
 *           heads from a locked heap and frees them every call. Replaced by a
 *           bottom-up merge sort over static arrays: also stable, so the same
 *           order.
 *   nosort0 (off by default: changes draw order) skips sorting the opaque
 *           list 0 front to back (stage table byte 0xA1). The GPU hides opaque
 *           overdraw, but coplanar surfaces could flicker.
 *   pin     the core pinning experiment (threadview.c), off by default.
 */
#include <arm_neon.h>
#include <psp2/gxm.h>
#include <psp2/kernel/clib.h>
#include <taihen.h>

#include "opt.h"
#include "cuts.h"

static tai_hook_ref_t s_matmul_ref;
static SceUID s_matmul_hook = -1;
static volatile int *s_on;
static volatile unsigned int s_mask = OPT_DEFAULT;

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

/* f 2nd main-thread cuts */

static const opt_cuts_t *s_cuts;
static uintptr_t s_seg0, s_seg1;
static mul44_fn s_mul;
static inv44_fn s_inv;
static xform_fn s_xform;

enum { CH_CENTRE, CH_LSORT, CH_COUNT };
static tai_hook_ref_t s_cref[CH_COUNT];
static SceUID s_chook[CH_COUNT] = {-1, -1};

// main thread only
static SceUID s_dirty_inj = -1;   // the patch that calls dirty_clear
static int s_dirty_err;           // failed injection (or the original bytes weren't there)
static volatile int s_stopping;   // opt_shutdown started: opt_frame does nothing
static int s_sort0_cleared;       // we switched the list 0 sort off

// diagnostics (cumulative)
static volatile uint32_t d_centre_calls, d_centre_bones, d_sorts, d_sort_big, d_dirty_fast;
static volatile uint32_t d_check_n, d_check_abs, d_check_rel;   // largest differences, x1e6

static inline int cut_on(unsigned int bit) {
    return *s_on && (s_mask & bit);
}

// the original's arguments: obj+0x10 bone count (signed), *obj the sub mesh with
// its centre c at +4 and the bone count it averages over at +0x20, bone matrices
// (4x4, contiguous) at *(obj+0x14)
static void centre_hook(const float *w, const float *x, const uint8_t *obj, float *out) {
    const uint8_t *sub = *(const uint8_t *const *)obj;
    uint32_t n = *(const uint32_t *)(sub + 0x20);
    if (!cut_on(OPT_CENTRE) || *(const int32_t *)(obj + 0x10) <= 0 || n == 0) {
        TAI_CONTINUE(void, s_cref[CH_CENTRE], w, x, obj, out);
        return;
    }
    cuts_centre(w, x, obj, out, s_mul, s_inv, s_xform);
    d_centre_calls++;
    d_centre_bones += n;
    // compare with the game's result: 1 call in 64, every call with check=1
    if ((s_mask & OPT_CHECK) || (d_centre_calls & 63) == 0) {
        float ref[3];
        TAI_CONTINUE(void, s_cref[CH_CENTRE], w, x, obj, ref);
        for (int k = 0; k < 3; k++) {
            float d = out[k] - ref[k], a = ref[k];
            if (d < 0)
                d = -d;
            if (a < 0)
                a = -a;
            float rel = d / (a > 1e-3f ? a : 1e-3f);
            // NaN fails the compares: recorded as the largest difference
            uint32_t abs6 = d < 4000.0f ? (uint32_t)(d * 1e6f) : 0xFFFFFFFFu;
            uint32_t rel6 = rel < 4000.0f ? (uint32_t)(rel * 1e6f) : 0xFFFFFFFFu;
            if (abs6 > d_check_abs)
                d_check_abs = abs6;
            if (rel6 > d_check_rel)
                d_check_rel = rel6;
        }
        d_check_n++;
    }
}

#define SORT_MAX 4096
static lnode_t *s_sa[SORT_MAX], *s_sb[SORT_MAX];   // main thread only (the draw lists)

static void list_sort_hook(dlist_t *l, less_fn less) {
    uint32_t n = l->size;
    if (!cut_on(OPT_LSORT) || n < 2 || n > SORT_MAX) {
        if (n > SORT_MAX && cut_on(OPT_LSORT))
            d_sort_big++;
        TAI_CONTINUE(void, s_cref[CH_LSORT], l, less);
        return;
    }
    cuts_list_sort(l, less, s_sa, s_sb);
    d_sorts++;
}

// 0x4DF012..0x4DF031: for (i = 0; i < 0xE0; i++) if (t[0x1A2A + i]) t[0x1A2A + i] = 0;
static const uint8_t DIRTY_ORIG[32] = {
    0x5f, 0xf0, 0x00, 0x0e,   // movs.w lr, #0
    0x41, 0xf6, 0x2a, 0x22,   // movw r2, #0x1a2a
    0x71, 0x46,               // mov r1, lr
    0x0b, 0x1c,               // adds r3, r1, #0
    0xa2, 0x18,               // adds r2, r4, r2
    0x10, 0x78,               // ldrb r0, [r2]
    0x00, 0xb1,               // cbz r0, +4
    0x13, 0x70,               // strb r3, [r2]
    0x0e, 0xf1, 0x01, 0x0e,   // add.w lr, lr, #1
    0x01, 0x32,               // adds r2, #1
    0xbe, 0xf1, 0xe0, 0x0f,   // cmp.w lr, #0xe0
    0xf6, 0xd3};              // blo loop
// called by the patch (cuts.h) in place of that loop, from the main thread: the
// game's loop with the cut off, one memset with it on. The patch goes in once,
// at game start: patching code the main thread runs every frame, while it moves
// between cores, could meet a core still holding the old bytes.
static void *dirty_clear(void *p, int c, unsigned int n) {
    if (cut_on(OPT_DIRTY)) {
        d_dirty_fast++;
        return sceClibMemset(p, c, n);
    }
    volatile uint8_t *b = (volatile uint8_t *)p;
    for (unsigned int i = 0; i < n; i++)
        if (b[i])
            b[i] = (uint8_t)c;
    return p;
}

void opt_set_mask(unsigned int mask) {
    s_mask = mask;
}

unsigned int opt_mask(void) {
    return s_mask;
}

// the main thread at the start of a frame, before stage 3 sorts
void opt_frame(void) {
    if (!s_cuts || s_stopping)
        return;
    if (s_cuts->sort0_flag) {
        // the game sets it to 1 once, at start
        volatile uint8_t *f = (volatile uint8_t *)(s_seg1 + s_cuts->sort0_flag);
        if (cut_on(OPT_NOSORT0)) {
            if (*f == 1) {
                *f = 0;
                s_sort0_cleared = 1;
            }
        } else if (s_sort0_cleared) {
            if (*f == 0)
                *f = 1;
            s_sort0_cleared = 0;
        }
    }
}

int opt_diag(char *buf, int size) {
    if (!s_cuts)
        return sceClibSnprintf(buf, size, "cuts: none for this game");
    return sceClibSnprintf(buf, size,
            "cuts: mask %X hooks %08X %08X patch %08X %08X; centre %u calls %u bones, sorts %u (%u too big),"
            " dirty %u; check %u max abs %u rel %u (x1e-6)",
            s_mask, s_chook[CH_CENTRE], s_chook[CH_LSORT], s_dirty_inj, (unsigned)s_dirty_err,
            d_centre_calls, d_centre_bones, d_sorts, d_sort_big, d_dirty_fast, d_check_n, d_check_abs, d_check_rel);
}

int opt_init(SceUID modid, uintptr_t seg0, uintptr_t seg1, uint32_t matmul_off,
        const opt_cuts_t *cuts, volatile int *on) {
    s_on = on;
    s_seg0 = seg0;
    s_seg1 = seg1;
    if (matmul_off)
        s_matmul_hook = taiHookFunctionOffset(&s_matmul_ref, modid, 0, matmul_off, 1, matmul_hook);
    if (cuts && seg0) {
        s_cuts = cuts;
        s_mul = (mul44_fn)(seg0 + cuts->mul44 + 1);   // Thumb
        s_inv = (inv44_fn)(seg0 + cuts->inv44 + 1);
        s_xform = (xform_fn)(seg0 + cuts->xform + 1);
        s_chook[CH_CENTRE] = taiHookFunctionOffset(&s_cref[CH_CENTRE], modid, 0, cuts->centre, 1, centre_hook);
        s_chook[CH_LSORT] = taiHookFunctionOffset(&s_cref[CH_LSORT], modid, 0, cuts->list_sort, 1, list_sort_hook);
        if (sceClibMemcmp((const void *)(seg0 + cuts->dirty_loop), DIRTY_ORIG, sizeof(DIRTY_ORIG)) == 0) {
            uint8_t patch[32];
            cuts_dirty_patch(patch, (uint32_t)(uintptr_t)dirty_clear);   // Thumb address (bit 0 set)
            s_dirty_inj = taiInjectData(modid, 0, cuts->dirty_loop, patch, sizeof(patch));
            if (s_dirty_inj < 0)
                s_dirty_err = s_dirty_inj;
        } else {
            s_dirty_err = 0x80000000;   // not the bytes we expect: leave it
        }
    }
    return s_matmul_hook;
}

void opt_shutdown(void) {
    s_stopping = 1;
    if (s_matmul_hook >= 0)
        taiHookRelease(s_matmul_hook, s_matmul_ref);
    s_matmul_hook = -1;
    if (s_dirty_inj >= 0)
        taiInjectRelease(s_dirty_inj);
    s_dirty_inj = -1;
    if (s_cuts && s_sort0_cleared && *(volatile uint8_t *)(s_seg1 + s_cuts->sort0_flag) == 0)
        *(volatile uint8_t *)(s_seg1 + s_cuts->sort0_flag) = 1;
    s_sort0_cleared = 0;
    for (int i = 0; i < CH_COUNT; i++) {
        if (s_chook[i] >= 0)
            taiHookRelease(s_chook[i], s_cref[i]);
        s_chook[i] = -1;
    }
    for (int i = UH_COUNT - 1; i >= 0; i--) {
        if (s_uhook[i] >= 0)
            taiHookRelease(s_uhook[i], s_uref[i]);
        s_uhook[i] = -1;
    }
}

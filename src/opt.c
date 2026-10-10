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
 *           byte-identical to the previous draw's (mostly 2D sprites; Round 2's
 *           ureuse below reuses those without looking at the data).
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
 *
 * Round 2 (further down): ureuse, matdup, palette, texwrap, keyset, h2f, nolog,
 * and the experiments earlykick and ucopy.
 */
#include <arm_neon.h>
#include <psp2/gxm.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <taihen.h>

#include "opt.h"
#include "cuts.h"

static tai_hook_ref_t s_matmul_ref;
static SceUID s_matmul_hook = -1;
static volatile int *s_on;
static volatile unsigned int s_mask = OPT_DEFAULT;
static int s_r2_reuse;                    // round 2 uniform reuse installed: the probe below must stay out
static uint32_t s_ur_scene = 1;            // scene count (round 2 uniform reuse)
static int s_scenes_tracked;               // main.c hooks Begin/EndScene: probe_scene() runs

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
    s_ur_scene++;   // round 2: no block reuse across a scene boundary
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
    if (s_r2_reuse)   // both replace the same reservation: the GPU would read stale blocks
        return -1;
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
// game's loop with the cut off, one memset with it on (the same memory
// afterwards; timed at start on the console: 0.15 us against the loop's 2.8).
// The patch goes in once, at game start: patching code the main thread runs
// every frame, while it moves between cores, could meet a core still holding
// the old bytes.
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

/* ---------------------------------------------------------------------------
 * Round 2 (f 2nd only; offsets in main.c F2ND_R2, docs/engine-notes.md).
 *
 *   ureuse  2D draws: the per-draw uniform setup (seg0+0x4DF052: MVP, flush
 *           0x4DEF04, vertex binding 0x4DE828) rebuilds and uploads the same
 *           4880-byte vertex block for nearly every sprite. Nothing in the
 *           sprite loop writes the uniform table T, and the engine already marks
 *           every write: 224+2 setter flags, 17 program/texture matrix dirty
 *           bytes. With none set and MV, P and the material colours unchanged,
 *           T after the flush is byte-identical to T after the last flush, so the
 *           block uploaded then (same program, same scene) is the block the game
 *           would write now: point the draw at it and skip the flush and copies.
 *           2D draws only: 3D ones always have flags set (bone palette, env
 *           1/2/11), and a 3D pre-draw isn't always followed by its draw.
 *           check=1 (else 1 reuse in 1024) compares the reused block with T,
 *           entry by entry; one difference turns ureuse off for the session.
 *   matdup  The flush's ten 16-byte material getters are two 80-byte copies of
 *           one contiguous source; 0x4DE954 recomputes the MVP that 0x4DF052
 *           stored at T+0x10C0 moments before. Both patched (once, at start) to
 *           call the plugin: the copies with the cut on, the game's code off.
 *   palette The bone palette (0x2472A0) goes into T with three setter calls per
 *           bone; same bytes and flags with plain copies.
 *   texwrap texParameteri wrap modes (0x4E1B30; WRAP_S and WRAP_T for every
 *           texture of every 3D draw, from 0x24763A) go through a 3-level generic
 *           dispatch with stack canaries; the same stores and the same libgxm
 *           call, directly.
 *   keyset  ROB_BASE key-set loop (0x20462A, RobThreads): the evaluator
 *           0x204334 inlined (cuts.h), same bits (host test against the game's
 *           machine code, 80000 vectors).
 *   h2f     0x4B1A48 half -> float: 90-byte replacement, checked bit-exact for
 *           all 65536 inputs at start (before and after patching).
 *   nolog   The DSC player formats a debug trace line for every command into a
 *           ring nothing reads: branch at 0x3367D2 made a nop.
 *   earlykick (off by default, validate first) kick ROB_BASE right after the
 *           DSC interpreter instead of after the PV update tail. ekcheck
 *           (with earlykick off) hashes the robot state at both points.
 *   ucopy   (off by default, timing experiment) uniform copies with aligned
 *           NEON stores instead of one SceLibc memcpy per parameter.
 *   h2f and nolog are patched at start if set then, and stay for the session;
 *   earlykick, ekcheck and ucopy are hooked only if set at start.
 */

// TAI_CONTINUE calls through an unprototyped pointer: a float argument is
// promoted to double and lands in d0, not s0. Hooks with float arguments call
// the next hook / original through a prototyped pointer instead.
#define TAI_NEXT(fn_t, ref) ({ \
        struct _tai_hook_user *h_ = (struct _tai_hook_user *)(ref); \
        struct _tai_hook_user *n_ = (struct _tai_hook_user *)h_->next; \
        (fn_t)(n_ ? n_->func : h_->old); })

static const opt_r2_t *s_r2;
static uint8_t *s_g;                       // the engine's GL state; T = s_g + 0x2EF24
#define R2_T(g) ((g) + 0x2EF24)
static uintptr_t s_main_lo, s_main_hi;     // the main thread's stack (opt_frame)
static volatile const int *s_rstage;       // main.c's render stage, counters only

static inline int on_main(void) {
    uintptr_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    return sp - s_main_lo < s_main_hi - s_main_lo;
}

static inline int stage_idx(void) {
    int st = s_rstage ? *s_rstage : -1;
    return st >= 0 && st < 8 ? st : 7;
}

// the game's functions and import stubs (stubs are ARM: bit 0 clear)
typedef void (*void_fn)(void);
typedef void (*fbind_fn)(uint8_t *fp, SceGxmContext *ctx, const uint8_t *src);
typedef void (*setvp_fn)(SceGxmContext *ctx, const void *prog);
typedef void (*setfp_fn)(SceGxmContext *ctx, const void *prog);
typedef int (*reserve_fn)(SceGxmContext *ctx, void **buf);
typedef int (*setdata_fn)(void *buf, const void *param, unsigned int off, unsigned int n, const float *src);
typedef void *(*memcpy_fn)(void *d, const void *s, unsigned int n);
typedef void *(*tls_fn)(const void *desc);
typedef void (*matget_fn)(uint8_t *light, int face, int pname, void *dst);
typedef void (*transpose_fn)(float *out, const float *in);
typedef int (*addrmode_fn)(void *tex, int mode);
typedef float (*h2f_fn)(uint32_t h);
static void_fn s_flush_tex;
static fbind_fn s_fbind;
static setvp_fn s_set_vp;
static setfp_fn s_set_fp;
static reserve_fn s_res_v, s_res_f;
static setdata_fn s_set_data_f;
static memcpy_fn s_gmemcpy;
static tls_fn s_tls;
static matget_fn s_matget;
static transpose_fn s_transpose;
static addrmode_fn s_set_uaddr, s_set_vaddr;

// shutdown releases from the last: a hook that calls through another's ref
// (DSC -> KICK) comes after it
enum { R2_PRE, R2_URES, R2_PAL, R2_TEX, R2_KS, R2_KICK, R2_DSC, R2_VBIND, R2_FBIND, R2_HOOKS };
static tai_hook_ref_t s_r2ref[R2_HOOKS];
static SceUID s_r2hook[R2_HOOKS] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
enum { R2P_MAT, R2P_MVP, R2P_H2F, R2P_LOG, R2_PATCHES };
static SceUID s_r2inj[R2_PATCHES] = {-1, -1, -1, -1};
static int s_r2err[R2_PATCHES];            // why a patch is not in (0: it is, or not asked for)

/* ---- ureuse ------------------------------------------------------------- */

typedef struct {
    const uint8_t *vp;     // program record: *(T+0x1B50) + index * 0x368
    const void *prog;      // the SceGxmVertexProgram bound (variant [G+0x2CD2C] != 0)
    uint32_t n;            // parameter entries
    void *buf;             // its default uniform block in the GXM vertex ring
    uint32_t gen, scene;   // T's contents / the scene when it was uploaded
} ublk_t;
#define UR_SLOTS 8
static ublk_t s_ublk[UR_SLOTS];            // main thread only, like everything below
static int s_ublk_next;
static uint32_t s_ur_gen = 1;
static int s_ur_snap;                      // s_ur_mv/p/mat are what the last flush used
static const uint8_t *s_ur_progs;
static uint32_t s_ur_mv[16], s_ur_p[16], s_ur_mat[20];
static volatile int *s_ur_ctxi;            // the main thread's render-context index (TLS)
static int s_ur_pend;                      // the next vbind reserve is for s_ur_key
static ublk_t s_ur_key;
static uint32_t s_ur_ret;                  // seg0 + reserve_ret | 1
static uint32_t s_ur_tick;
static uint32_t s_ur_ret2d;                // seg0 + predraw_ret2d | 1: called by a 2D draw
static int s_ur_off;                       // a check failed: the game's path for the session
static volatile uint32_t d_ur_calls, d_ur_dirty, d_ur_changed, d_ur_miss, d_ur_skip, d_ur_rec, d_ur_other;
static volatile uint32_t d_ur_vchk, d_ur_vbad, d_ur_vna;
static volatile uint32_t d_ur_st_calls[8], d_ur_st_skip[8];

// the setters' flags since the last flush: 224 vertex env bytes at T+0x1A2A
// (2 mod 4), the 2 local bytes at T+0x1B2A and T+0x1B4C (cleared by the flush)
static inline uint32_t ur_flags(const uint8_t *t) {
    const uint32_t *w = (const uint32_t *)(t + 0x1A2C);   // word aligned (checked at init)
    uint32_t a = *(const uint16_t *)(t + 0x1A2A) | *(const uint16_t *)(t + 0x1B08)
            | *(const uint16_t *)(t + 0x1B2A) | t[0x1B4C];
    a |= w[0] | w[1] | w[2];   // 3D items write env 1, 2 and 11 every draw: out here
    if (a)
        return a;
    for (int i = 3; i < 55; i++)
        a |= w[i];
    return a;
}

static inline uint32_t ur_diff(const uint32_t *a, const uint32_t *b, int n) {
    uint32_t d = 0;
    for (int i = 0; i < n; i++)
        d |= a[i] ^ b[i];
    return d;
}

static ublk_t *ur_find(const uint8_t *vp, const void *prog, uint32_t n) {
    for (int i = 0; i < UR_SLOTS; i++) {
        ublk_t *b = &s_ublk[i];
        if (b->buf && b->vp == vp && b->prog == prog && b->n == n && b->gen == s_ur_gen
                && b->scene == s_ur_scene)
            return b;
    }
    return NULL;
}

// a reserve of vbind's for s_ur_key succeeded: remember the block
static void ur_record(void *buf) {
    ublk_t *b = NULL;
    for (int i = 0; i < UR_SLOTS && !b; i++)
        if (s_ublk[i].vp == s_ur_key.vp && s_ublk[i].prog == s_ur_key.prog && s_ublk[i].n == s_ur_key.n)
            b = &s_ublk[i];
    if (!b) {
        b = &s_ublk[s_ublk_next];
        s_ublk_next = (s_ublk_next + 1) % UR_SLOTS;
    }
    *b = s_ur_key;
    b->buf = buf;
    d_ur_rec++;
}

// import hook, sceGxmReserveVertexDefaultUniformBuffer (installed at start)
static int ur_reserve_hook(SceGxmContext *ctx, void **buf) {
    uint32_t ra = (uint32_t)__builtin_return_address(0);
    int r = TAI_CONTINUE(int, s_r2ref[R2_URES], ctx, buf);
    if (s_ur_pend && ra == s_ur_ret) {
        s_ur_pend = 0;
        if (r >= 0 && *buf)
            ur_record(*buf);
    }
    return r;
}

// the reused block against the bytes the game's copies would take from T now
// (T is in cached memory, the block in the uncached ring). With the generation
// unchanged, T before this flush is T after the flush that recorded the block,
// so a difference means a T writer this file doesn't know about.
// 0: same, 1: different, -1: a SetUniformDataF entry (not comparable here)
static int ur_check(const uint8_t *vp, uint32_t n, const uint8_t *buf, const uint8_t *t) {
    const uint32_t *e = (const uint32_t *)(vp + 0x34);   // {dst offset, T offset, bytes} (0x4DE8C4)
    int r = 0;
    for (uint32_t i = 0; i < n; i++, e += 3) {
        if (e[2] >> 31)
            r = -1;
        else if (sceClibMemcmp(buf + e[0], t + e[1], e[2]) != 0)
            return 1;
    }
    return r;
}

// seg0+0x4DF052: no arguments, no result; once per draw (3D: bl at 0x247ED6,
// 2D: the draw descriptor's pre-draw callback, built at 0x4D8626, blx r3 at 0x4E11A8)
static void predraw_hook(void) {
    uint8_t *g = s_g, *t = R2_T(g);
    uint32_t ra = (uint32_t)__builtin_return_address(0);   // offset hook: the game's lr
    const uint8_t *progs = *(const uint8_t *const volatile *)(t + 0x1B50);
    // 2D only: a 2D pre-draw is always followed by its draw (0x4E1190), a 3D one
    // isn't (0x4D8CCA can return first), so a 3D reservation may never be used
    if (!cut_on(OPT_UREUSE) || s_ur_off || !progs || ra != s_ur_ret2d || !on_main()) {
        if (ra != s_ur_ret2d)
            d_ur_other++;
        s_ur_snap = 0;   // T may change behind us: start over
        TAI_CONTINUE(void, s_r2ref[R2_PRE]);
        return;
    }
    if (!s_ur_ctxi)   // as 0x4DF072: __tls_get_addr(desc), then the int there
        s_ur_ctxi = (volatile int *)s_tls((const void *)(s_seg1 + s_r2->d_tls_render));
    int st = stage_idx();
    d_ur_calls++;
    d_ur_st_calls[st]++;
    const uint8_t *mc = g + 0x24C * *s_ur_ctxi;   // matrix context, as 0x4DF07C
    uint32_t dirty = 0;
    for (int i = 0; i < 9; i++)
        dirty |= mc[0x6EC + 0x14 * i];   // program matrices (0x4DEB26)
    for (int i = 0; i < 8; i++)
        dirty |= mc[0x5AC + 0x14 * i];   // texture matrices (0x4DEDB6)
    if (ur_flags(t) | dirty) {
        // a setter or matrix op ran: this flush changes T. The next call compares
        // against nothing (snapshot dropped), so it bumps the generation again,
        // which also covers program matrices 4..6 overwriting T+0xF80..0x1040 now
        s_ur_snap = 0;
        s_ur_gen++;
        d_ur_dirty++;
        TAI_CONTINUE(void, s_r2ref[R2_PRE]);
        return;
    }
    const uint8_t *vp = progs + *(const volatile uint32_t *)(t + 0x1A18) * 0x368;
    int var = *(const volatile uint32_t *)(g + 0x2CD2C) != 0;   // 0x4D9144(G+0x2CB80, 15)
    const void *prog = *(const void *const volatile *)(vp + 0x2C + 4 * var);
    uint32_t n = *(const volatile uint32_t *)(vp + 0x1B4);
    const uint32_t *mv = *(const uint32_t *const volatile *)(mc + 0x560);
    const uint32_t *p = *(const uint32_t *const volatile *)(mc + 0x574);
    const uint32_t *mat = (const uint32_t *)(g + 0x30F14);   // material colours, 0x4DE314's source
    ublk_t *b = NULL;
    if (s_ur_snap && progs == s_ur_progs
            && (ur_diff(mv, s_ur_mv, 16) | ur_diff(p, s_ur_p, 16) | ur_diff(mat, s_ur_mat, 20)) == 0) {
        b = ur_find(vp, prog, n);   // T now == T after the last flush
        if (!b)
            d_ur_miss++;
    } else {
        for (int i = 0; i < 16; i++) {
            s_ur_mv[i] = mv[i];
            s_ur_p[i] = p[i];
        }
        for (int i = 0; i < 20; i++)
            s_ur_mat[i] = mat[i];
        s_ur_snap = 1;
        s_ur_progs = progs;
        s_ur_gen++;
        d_ur_changed++;
    }
    if (b && ((s_mask & OPT_CHECK) || (++s_ur_tick & 1023) == 0)) {
        int r = ur_check(vp, n, (const uint8_t *)b->buf, t);
        if (r < 0)
            d_ur_vna++;
        else
            d_ur_vchk++;
        if (r > 0) {   // an assumption is wrong: no more reuse this session
            d_ur_vbad++;
            s_ur_off = 1;
            b = NULL;
        }
    }
    if (!b) {
        // the game's path; the reserve hook remembers the new block
        s_ur_key = (ublk_t){vp, prog, n, NULL, s_ur_gen, s_ur_scene};
        s_ur_pend = 1;
        TAI_CONTINUE(void, s_r2ref[R2_PRE]);
        s_ur_pend = 0;
        return;
    }
    // skip: the flush would write the same bytes and clear flags that are 0;
    // what is left of it and of 0x4DE828 is GXM state, in the game's order
    s_flush_tex();                                    // 0x4DF048: bl 0x4E185A
    ((volatile uint8_t *)t)[0x1A29] = 1;              // 0x4DE85A
    ((volatile uint8_t *)t)[0x1A28] = 1;              // 0x4DE862
    SceGxmContext *ctx = *(SceGxmContext *const volatile *)(g + 0x3C);
    s_set_vp(ctx, prog);                              // 0x4DE88E
    sceGxmSetVertexDefaultUniformBuffer(ctx, b->buf); // in place of reserve + copies
    uint32_t fi = *(const volatile uint32_t *)(t + 0x1A1C);
    s_fbind((uint8_t *)progs + fi * 0x368 + 0x1B8, *(SceGxmContext *const volatile *)(g + 0x3C),
            g + 0x30274);                             // 0x4DE934
    d_ur_skip++;
    d_ur_st_skip[st]++;
}

/* ---- matdup ------------------------------------------------------------- */

// 0x4DEF28 (16 bytes, inside 0x4DEF04: r6 = G, r4/r5 kept): mov r0, r6; movw/movt ip, fn; blx ip; b.w 0x4DF004
static const uint8_t MAT_ORIG[16] = {0x5f, 0xf4, 0x09, 0x70, 0xc0, 0xf2, 0x03, 0x00,
        0x33, 0x18, 0x38, 0x1c, 0x2a, 0x21, 0x40, 0xf2};
static const uint8_t MAT_HEAD[2] = {0x30, 0x46};
static const uint8_t MAT_TAIL[6] = {0xe0, 0x47, 0x00, 0xf0, 0x66, 0xb8};
// 0x4DE994 (22 bytes, inside 0x4DE954: r4 = matrix context, r7 = G):
// adds.w r6, r4, #0x560; mov r0, r7; mov r1, r4; movw/movt ip, fn; blx ip; b.w 0x4DEAC2
static const uint8_t MVP_ORIG[22] = {0x28, 0x1c, 0x14, 0xf5, 0xac, 0x66, 0x80, 0xf1, 0xe6, 0xee, 0x00,
        0x68, 0x00, 0xfb, 0x08, 0x70, 0xd0, 0xf8, 0x60, 0x25, 0xd0, 0xf8};
static const uint8_t MVP_HEAD[8] = {0x14, 0xf5, 0xac, 0x66, 0x38, 0x46, 0x21, 0x46};
static const uint8_t MVP_TAIL[6] = {0xe0, 0x47, 0x00, 0xf0, 0x8c, 0xb8};
static volatile uint32_t d_mat_fast, d_mvp_fast;

// head; movw ip, #lo(fn); movt ip, #hi(fn); tail
static int r2_callout(uint8_t *o, const uint8_t *head, int nh, uint32_t fn, const uint8_t *tail) {
    int k = 0;
    for (int i = 0; i < nh; i++)
        o[k++] = head[i];
    cuts_thumb_movw(o + k, 0, 12, fn & 0xFFFF);
    cuts_thumb_movw(o + k + 4, 1, 12, fn >> 16);
    k += 8;
    for (int i = 0; i < 6; i++)
        o[k++] = tail[i];
    return k;
}

// called by the 0x4DEF28 patch, r0 = G: the ten 0x4DE314(G+0x30A90, 0x2A, pname, dst) calls
// copy material colours G+0x30F14..0x30F64 (pnames 0x14F, 0x150, 0x151, 0x176, 0x177 ->
// +0x484, +0x494, +0x4A4, +0x4B4, +0x4C4) to T+0x1300 (G+0x30224) and F+0x640 (G+0x308B4)
// scalar copies: the flush runs right after VFP stores to the source, and NEON
// loads of VFP-stored data stall (the reason the NEON matmul was slower)
__attribute__((optimize("no-tree-vectorize"))) static void r2_matcopy(uint8_t *g) {
    if (cut_on(OPT_MATDUP)) {
        const uint32_t *s = (const uint32_t *)(g + 0x30F14);
        uint32_t *d1 = (uint32_t *)(g + 0x30224), *d2 = (uint32_t *)(g + 0x308B4);
        for (int i = 0; i < 20; i++) {
            uint32_t v = s[i];
            d1[i] = v;
            d2[i] = v;
        }
        d_mat_fast++;
        return;
    }
    static const uint16_t PN[5] = {0x14F, 0x150, 0x151, 0x176, 0x177};
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < 5; i++)
            s_matget(g + 0x30A90, 0x2A, PN[i], g + (k ? 0x308B4 : 0x30224) + 16 * i);
}

// called by the 0x4DE994 patch, r0 = G, r1 = matrix context: T+0x1000 = transpose(MV * P),
// which 0x4DF052 computed with the same functions and inputs into T+0x10C0 (0x4DF174)
__attribute__((optimize("no-tree-vectorize"))) static void r2_mvpdup(uint8_t *g, uint8_t *mc) {
    uint8_t *t = R2_T(g);
    if (cut_on(OPT_MATDUP)) {
        const uint32_t *s = (const uint32_t *)(t + 0x10C0);
        uint32_t *d = (uint32_t *)(t + 0x1000);
        for (int i = 0; i < 16; i++)
            d[i] = s[i];
        d_mvp_fast++;
        return;
    }
    // the game's 0x4DE99A..0x4DEABE
    float m[16] __attribute__((aligned(16))), c[16] __attribute__((aligned(16)));
    float o[16] __attribute__((aligned(16)));
    const uint32_t *mv = *(const uint32_t *const *)(mc + 0x560);
    for (int i = 0; i < 16; i++)
        ((uint32_t *)m)[i] = mv[i];
    s_mul(m, m, *(const float *const *)(mc + 0x574));
    for (int i = 0; i < 16; i++)
        ((uint32_t *)c)[i] = ((const uint32_t *)m)[i];
    s_transpose(o, c);
    for (int i = 0; i < 16; i++)
        ((uint32_t *)(t + 0x1000))[i] = ((const uint32_t *)o)[i];
}

/* ---- palette ------------------------------------------------------------ */

// seg0+0x2472A0(bones, n), only caller 0x246C82 (skinned draw items): for n in 1..64,
// rows 0..2 of bone i (64-byte stride) -> env[0x20 + 3i + row] = T+0x280+48i, and
// their flags T+0x1A4A+3i.. = 1 (0x4DFBE4: data, then flag; index never clamped)
static void palette_hook(const uint32_t *b, int n) {
    if (!cut_on(OPT_PALETTE)) {
        TAI_CONTINUE(void, s_r2ref[R2_PAL], b, n);
        return;
    }
    if (n > 64 || n <= 0)
        return;
    uint8_t *t = R2_T(s_g);
    uint32_t *d = (uint32_t *)(t + 0x280);
    for (int i = 0; i < n; i++, b += 16, d += 12)
        for (int k = 0; k < 12; k++)
            d[k] = b[k];
    sceClibMemset(t + 0x1A2A + 0x20, 1, 3 * n);
}

/* ---- texwrap ------------------------------------------------------------ */

static volatile uint32_t d_tex_fast;
// seg0+0x4E1B30(target, pname, value) -> 0x4E1A3A -> 0x4E18A2. For target 0x11D/0x11E the
// texture is the current one, *(G+0x1217B4); WRAP_S (0x1B3) / WRAP_T (0x1B4) map the value
// with 0x4DC4AC and, if valid, store mode at tex+0x50/+0x54, value at +0x8C/+0x90 and call
// sceGxmTextureSetU/VAddrMode(tex+8, mode). Invalid values change nothing. Callers ignore r0.
static void texparam_hook(uint32_t target, uint32_t pname, uint32_t value) {
    if (!cut_on(OPT_TEXWRAP) || target - 0x11D > 1 || pname - 0x1B3 > 1) {
        TAI_CONTINUE(void, s_r2ref[R2_TEX], target, pname, value);
        return;
    }
    uint8_t *tex = *(uint8_t *const volatile *)(s_g + 0x1217B4);
    if (!tex)
        return;
    uint32_t m;
    if (value == 0x1B8)
        m = 0;
    else if (value == 0x284)
        m = 1;
    else if (value == 0x1B7 || value == 0x212 || value == 0x257)
        m = 2;
    else if (value == 0x820 || value == 0x821 || value == 0xA12)
        m = 3;
    else
        return;
    if (pname == 0x1B3) {
        *(volatile uint32_t *)(tex + 0x50) = m;
        *(volatile uint32_t *)(tex + 0x8C) = value;
        s_set_uaddr(tex + 8, (int)m);
    } else {
        *(volatile uint32_t *)(tex + 0x54) = m;
        *(volatile uint32_t *)(tex + 0x90) = value;
        s_set_vaddr(tex + 8, (int)m);
    }
    d_tex_fast++;
}

/* ---- keyset (RobThreads; nothing but the flags shared) ------------------- */

typedef void (*keyset_loop_fn)(void *unused, uint32_t *out, keyset_t *ks, uint32_t n, float frame);
// seg0+0x20462A(r0 unused, out, key sets, count, frame): 0x204334(out++, ks++, frame) per set
static void keyset_hook(void *unused, uint32_t *out, keyset_t *ks, uint32_t n, float frame) {
    if (!cut_on(OPT_KEYSET)) {
        TAI_NEXT(keyset_loop_fn, s_r2ref[R2_KS])(unused, out, ks, n, frame);
        return;
    }
    for (; n != 0; n--)
        cuts_keyset_eval(out++, ks++, frame);
}

/* ---- h2f (patched at start) --------------------------------------------- */

// uxth r1,r0; lsls r0,r1,#16; and r0,#0x80000000; ubfx r3,r1,#10,#5; ubfx r2,r1,#0,#10;
// cbz r3,sub; cmp r3,#31; beq big; ubfx r1,r1,#0,#15; add.w r0,r0,r1,lsl#13;
// add.w r0,#0x38000000; vmov s0,r0; bx lr; big: orr r0,#0x7f800000; orr.w r0,r0,r2,lsl#13;
// vmov s0,r0; bx lr; sub: cbz r2,out; clz r3,r2; sub r1,r3,#8; lsl r2,r1; bic r2,#0x800000;
// rsb r3,#134; orr r0,r3,lsl#23; orr r0,r2; out: vmov s0,r0; bx lr
static const uint8_t H2F_NEW[90] = {
    0x81, 0xb2, 0x08, 0x04, 0x00, 0xf0, 0x00, 0x40, 0xc1, 0xf3, 0x84, 0x23, 0xc1, 0xf3, 0x09, 0x02,
    0x8b, 0xb1, 0x1f, 0x2b, 0x08, 0xd0, 0xc1, 0xf3, 0x0e, 0x01, 0x00, 0xeb, 0x41, 0x30, 0x00, 0xf1,
    0x60, 0x50, 0x00, 0xee, 0x10, 0x0a, 0x70, 0x47, 0x40, 0xf0, 0xff, 0x40, 0x40, 0xea, 0x42, 0x30,
    0x00, 0xee, 0x10, 0x0a, 0x70, 0x47, 0x6a, 0xb1, 0xb2, 0xfa, 0x82, 0xf3, 0xa3, 0xf1, 0x08, 0x01,
    0x02, 0xfa, 0x01, 0xf2, 0x22, 0xf4, 0x00, 0x02, 0xc3, 0xf1, 0x86, 0x03, 0x40, 0xea, 0xc3, 0x50,
    0x40, 0xea, 0x02, 0x00, 0x00, 0xee, 0x10, 0x0a, 0x70, 0x47};
// mov ip, sp; push.w {r0-r3}; push.w {r4, r5, ip, lr}; sub sp, #8; then
// movw/movt r5 of the stack-canary word, seg0+0x661F4C: relocated at load, so
// h2f_orig() builds those 8 bytes for where seg0 is
static const uint8_t H2F_HEAD[12] = {0xec, 0x46, 0x2d, 0xe9, 0x0f, 0x00, 0x2d, 0xe9,
        0x30, 0x50, 0x82, 0xb0};
#define H2F_CANARY 0x661F4C
static int h2f_orig(uint8_t o[20]) {
    for (int i = 0; i < 12; i++)
        o[i] = H2F_HEAD[i];
    uint32_t a = (uint32_t)(s_seg0 + H2F_CANARY);
    cuts_thumb_movw(o + 12, 0, 5, a & 0xFFFF);
    cuts_thumb_movw(o + 16, 1, 5, a >> 16);
    return 20;
}

// every input through the code at seg0+h2f against cuts_h2f_bits
static int h2f_selftest(void) {
    h2f_fn f = (h2f_fn)(s_seg0 + s_r2->h2f + 1);
    for (uint32_t h = 0; h < 0x10000; h++)
        if (cuts_f_bits(f(h)) != cuts_h2f_bits(h))
            return -(int)h - 1;
    return 0;
}

/* ---- nolog (patched at start) -------------------------------------------- */

// 0x3367D2 bne 0x3367FC (13 D1) -> nop (00 BF): skip the trace block 0x3367FC..0x33687E,
// which only reads the command, formats it with 0x6424 into a ring no code reads, and
// rejoins at 0x3367D4 with the same r7
static const uint8_t LOG_ORIG[2] = {0x13, 0xd1}, LOG_NEW[2] = {0x00, 0xbf};

/* ---- earlykick (validate first) ------------------------------------------ */

typedef uint64_t (*dsc_fn)(void *pl, int a1, int64_t t, float dt, uint32_t a5, uint32_t a6);
static int s_ek_kicked;                    // kicked early this frame: skip the call at 0x5F1B56
static uint32_t s_ek_state[4];             // ROB_MANAGER task state words at the early kick
static uint32_t s_ek_hash;                 // ekcheck: rob state hash at the DSC return
static int s_ek_hashed;
static volatile uint32_t d_ek_kicks, d_ek_gated, d_ek_late, d_ek_lost, d_ek_chk, d_ek_chk_diff;

// no PvOsageManager slot busy or ready: then the post-DSC performer block
// (0x31BF34..0x31BFB2) does nothing this frame, no slot can become busy before
// 0x5F1B56 (0x327708, its only in-song setter, is in that block), and
// 0x34C9A4's own gate (0x327600) reads the same 0 both times
static int ek_gate(void) {
    const uint8_t *tbl = *(const uint8_t *const volatile *)(s_seg1 + s_r2->d_osage);
    if (!tbl)
        return 1;
    for (int i = 0; i < 4; i++) {
        const volatile uint8_t *s = tbl + i * 0x908;
        if (s[0x878] | s[0x8E1])
            return 0;
    }
    return 1;
}

static uint32_t ek_hash_rob(void) {   // FNV-1a over rob_chara[4] and the ROB_MANAGER task
    uint32_t h = 2166136261u;
    const uint8_t *rob = (const uint8_t *)(s_seg1 + s_r2->d_rob_chara);
    const uint8_t *rm = (const uint8_t *)(s_seg1 + s_r2->d_rob_mgr);
    for (int i = 0; i < 4 * 0x2C68; i += 4)
        h = (h ^ *(const uint32_t *)(rob + i)) * 16777619u;
    for (int i = 0; i < 0x300; i += 4)
        h = (h ^ *(const uint32_t *)(rm + i)) * 16777619u;
    return h;
}

// seg0+0x336402, the DSC player; 0x31B346 calls it at 0x31BA1E (normal path) and 0x31B458
static uint64_t dsc_hook(void *pl, int a1, int64_t t, float dt, uint32_t a5, uint32_t a6) {
    uint32_t ra = (uint32_t)__builtin_return_address(0);
    uint64_t ret = TAI_NEXT(dsc_fn, s_r2ref[R2_DSC])(pl, a1, t, dt, a5, a6);
    if (ra != ((s_seg0 + s_r2->dsc_ret) | 1) || !on_main()
            || !*(const volatile uint8_t *)(s_seg1 + s_r2->d_pv_kick))   // 0x3256EC's result: no kick
        return ret;
    if (cut_on(OPT_EARLYKICK)) {
        if (!ek_gate()) {
            d_ek_gated++;
            return ret;
        }
        TAI_CONTINUE(void, s_r2ref[R2_KICK]);   // the game's 0x35C71E, now
        const uint32_t *rm = (const uint32_t *)(s_seg1 + s_r2->d_rob_mgr);
        for (int i = 0; i < 4; i++)
            s_ek_state[i] = rm[3 + i];           // task +0xC..+0x18 (0x141B8's state words)
        s_ek_kicked = 1;
        d_ek_kicks++;
    } else if ((s_mask & OPT_EKCHECK) && ek_gate()) {
        s_ek_hash = ek_hash_rob();
        s_ek_hashed = 1;
    }
    return ret;
}

// seg0+0x35C71E, only caller 0x5F1B56 (PVGAME ctrl, after the PV update)
static void kick_hook(void) {
    if (s_ek_kicked) {
        s_ek_kicked = 0;
        // what the game's call here would have seen differently: a task state request
        // (0x141B8 applies +0xC..+0x18) or a create / free request (list sizes +0x2CC,
        // +0x2D8, drained by the ROB_MANAGER ctrl 0x35D602) made after the early kick
        const uint32_t *rm = (const uint32_t *)(s_seg1 + s_r2->d_rob_mgr);
        int late = rm[0x2CC / 4] != 0 || rm[0x2D8 / 4] != 0;
        for (int i = 0; i < 4; i++)
            late |= rm[3 + i] != s_ek_state[i];
        if (late)
            d_ek_late++;
        return;
    }
    if (s_ek_hashed) {
        s_ek_hashed = 0;
        d_ek_chk++;
        if (ek_hash_rob() != s_ek_hash)          // the PV update tail wrote rob state
            d_ek_chk_diff++;
    }
    TAI_CONTINUE(void, s_r2ref[R2_KICK]);
}

/* ---- ucopy (timing experiment, off by default) --------------------------- */

static volatile uint32_t d_uc_vbind;

// memcpy(d, s, n) for word-aligned d, s, n: ascending stores, 32/16-byte NEON
// stores aligned to their size (the ring is written in whole bursts)
static inline __attribute__((always_inline)) void ucopy(uint8_t *d, const uint8_t *s, uint32_t n) {
    if ((((uintptr_t)d | (uintptr_t)s | n) & 3) != 0) {
        s_gmemcpy(d, s, n);   // never seen: memcpy's semantics for anything else
        return;
    }
    while (n != 0 && ((uintptr_t)d & 31) != 0) {
        *(uint32_t *)d = *(const uint32_t *)s;
        d += 4;
        s += 4;
        n -= 4;
    }
    uint32_t blocks = n >> 6;
    if (blocks != 0)
        __asm__ volatile(
                "1:\n\t"
                "vld1.32 {d0-d3}, [%[s]]!\n\t"
                "vld1.32 {d4-d7}, [%[s]]!\n\t"
                "subs    %[k], %[k], #1\n\t"
                "vst1.64 {d0-d3}, [%[d]:256]!\n\t"
                "vst1.64 {d4-d7}, [%[d]:256]!\n\t"
                "bne     1b"
                : [d] "+r"(d), [s] "+r"(s), [k] "+r"(blocks)
                :
                : "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "cc", "memory");
    if (n & 32)
        __asm__ volatile("vld1.32 {d0-d3}, [%[s]]!\n\t"
                         "vst1.64 {d0-d3}, [%[d]:256]!"
                         : [d] "+r"(d), [s] "+r"(s) : : "d0", "d1", "d2", "d3", "memory");
    if (n & 16)
        __asm__ volatile("vld1.32 {d0-d1}, [%[s]]!\n\t"
                         "vst1.64 {d0-d1}, [%[d]:128]!"
                         : [d] "+r"(d), [s] "+r"(s) : : "d0", "d1", "memory");
    for (n &= 15; n != 0; n -= 4) {
        *(uint32_t *)d = *(const uint32_t *)s;
        d += 4;
        s += 4;
    }
}

// the game's upload loop (0x4DE8C4..0x4DE90A / 0x4DE79A..0x4DE7DE): first entry
// unconditionally (count checked non-zero), count re-read every pass
static inline __attribute__((always_inline)) void uc_upload(void *buf, const uint8_t *src,
        const uint32_t *e, const volatile uint32_t *count) {
    uint32_t i = 0;
    do {
        uint32_t c = e[3 * i + 2], sz = c & 0x7FFFFFFFu;
        if (c & 0x80000000u)
            s_set_data_f(buf, (const void *)e[3 * i], 0, sz, (const float *)(src + e[3 * i + 1] * 4));
        else
            ucopy((uint8_t *)buf + e[3 * i], src + e[3 * i + 1], sz);
    } while (++i < *count);
}

// seg0+0x4DE706(fp, ctx, src), callers 0x4DE934 and 0x4E05CC
static void fbind_hook(uint8_t *fp, SceGxmContext *ctx, const uint8_t *src) {
    if (!cut_on(OPT_UCOPY)) {
        TAI_CONTINUE(void, s_r2ref[R2_FBIND], fp, ctx, src);
        return;
    }
    const uint8_t *s30 = s_g + 0x30000;
    uint32_t k = 0;
    if (s30[0xABC] != 0) {                     // 0x4DE728
        uint32_t a = *(const volatile uint32_t *)(s30 + 0xAC0), b = *(const volatile uint32_t *)(s30 + 0xAC8);
        if (a == 0x1F) {
            if (b == 0x20) k = 1;
            if (b == 0x1E) k = 2;
            if (b == 1) k = 3;
        } else if (a == 1) {
            if (b == 0x20) k = 4;
            if (b == 1) k = 5;
        } else if (a == 0x1D) {
            if (b == 1) k = 7;
        } else if (a == 0x23) {
            if (b == 0) k = 8;
        }
    }
    const volatile uint32_t *slot = (const volatile uint32_t *)(fp + 8 + 4 * k);
    s_set_fp(ctx, (const void *)*slot);        // 0x4DE768
    const volatile uint32_t *count = (const volatile uint32_t *)(fp + 0x1AC);
    if (*slot == 0 || *count == 0 || src == NULL)
        return;
    void *buf = NULL;
    s_res_f(ctx, &buf);                        // result ignored, as the game
    if (*count == 0)
        return;
    uc_upload(buf, src, (const uint32_t *)(fp + 0x2C), count);
}

// seg0+0x4DE828(T), only caller 0x4DF18A
static void vbind_hook(uint8_t *t) {
    if (!cut_on(OPT_UCOPY)) {
        TAI_CONTINUE(void, s_r2ref[R2_VBIND], t);
        return;
    }
    uint8_t *progs = *(uint8_t *const volatile *)(t + 0x1B50);
    if (!progs)                                // 0x4DE846
        return;
    uint32_t vi = *(const volatile uint32_t *)(t + 0x1A18), fi = *(const volatile uint32_t *)(t + 0x1A1C);
    ((volatile uint8_t *)t)[0x1A29] = 1;
    ((volatile uint8_t *)t)[0x1A28] = 1;
    SceGxmContext *ctx = *(SceGxmContext *const volatile *)(s_g + 0x3C);
    uint8_t *vp = progs + vi * 0x368;
    uint32_t var = *(const volatile uint32_t *)(s_g + 0x2CD2C) != 0;
    s_set_vp(ctx, *(const void *const *)(vp + 0x2C + 4 * var));
    const volatile uint32_t *count = (const volatile uint32_t *)(vp + 0x1B4);
    if (*count != 0) {
        void *buf = NULL;
        int r = s_res_v(ctx, &buf);            // result ignored, as the game
        if (s_ur_pend) {                       // ureuse: this is s_ur_key's block
            s_ur_pend = 0;
            if (r >= 0 && buf)
                ur_record(buf);
        }
        if (*count != 0)
            uc_upload(buf, R2_T(s_g), (const uint32_t *)(vp + 0x34), count);
    }
    d_uc_vbind++;
    if (((volatile uint8_t *)t)[0x1A28] != 0)  // 0x4DE90C
        s_fbind(progs + fi * 0x368 + 0x1B8, *(SceGxmContext *const volatile *)(s_g + 0x3C), R2_T(s_g) + 0x1350);
}

/* ---- install / diag ------------------------------------------------------ */

void opt_set_stage_ptr(volatile const int *stage) {
    s_rstage = stage;
}

static void r2_frame(void) {
    if (!s_main_hi) {   // the main thread's stack, once
        SceKernelThreadInfo ti;
        sceClibMemset(&ti, 0, sizeof(ti));
        ti.size = sizeof(ti);
        if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti) >= 0 && ti.stack) {
            s_main_lo = (uintptr_t)ti.stack;
            s_main_hi = s_main_lo + (uint32_t)ti.stackSize;
        }
    }
    if (s_ek_kicked) {   // early kick without the call at 0x5F1B56 (must stay 0)
        s_ek_kicked = 0;
        d_ek_lost++;
    }
    s_ek_hashed = 0;
}

static SceUID r2_inject(SceUID modid, int which, uint32_t off, const uint8_t *orig, int norig,
        const uint8_t *bytes, int n) {
    if (sceClibMemcmp((const void *)(s_seg0 + off), orig, norig) != 0) {
        s_r2err[which] = 0x80000000;   // not the bytes we expect: leave it
        return -1;
    }
    SceUID id = taiInjectData(modid, 0, off, bytes, n);
    if (id < 0)
        s_r2err[which] = id;
    return id;
}

static void r2_init(SceUID modid, const opt_r2_t *r2) {
    s_r2 = r2;
    s_g = (uint8_t *)(s_seg1 + r2->d_gl);
    uint8_t *t = R2_T(s_g);
    s_flush_tex = (void_fn)(s_seg0 + r2->flush_tex + 1);
    s_fbind = (fbind_fn)(s_seg0 + r2->fbind + 1);
    s_matget = (matget_fn)(s_seg0 + r2->mat_get + 1);
    s_transpose = (transpose_fn)(s_seg0 + r2->transpose + 1);
    s_set_vp = (setvp_fn)(s_seg0 + r2->stub_set_vp);
    s_set_fp = (setfp_fn)(s_seg0 + r2->stub_set_fp);
    s_res_v = (reserve_fn)(s_seg0 + r2->stub_reserve_v);
    s_res_f = (reserve_fn)(s_seg0 + r2->stub_reserve_f);
    s_set_data_f = (setdata_fn)(s_seg0 + r2->stub_set_data_f);
    s_gmemcpy = (memcpy_fn)(s_seg0 + r2->stub_memcpy);
    s_tls = (tls_fn)(s_seg0 + r2->stub_tls);
    s_set_uaddr = (addrmode_fn)(s_seg0 + r2->stub_uaddr);
    s_set_vaddr = (addrmode_fn)(s_seg0 + r2->stub_vaddr);
    s_ur_ret = (uint32_t)(s_seg0 + r2->reserve_ret) | 1;
    s_ur_ret2d = (uint32_t)(s_seg0 + r2->predraw_ret2d) | 1;

    // code patches first (nothing has run yet), each after checking the bytes
    uint8_t buf[32];
    int n = r2_callout(buf, MAT_HEAD, sizeof(MAT_HEAD), (uint32_t)(uintptr_t)r2_matcopy, MAT_TAIL);
    s_r2inj[R2P_MAT] = r2_inject(modid, R2P_MAT, r2->mat_patch, MAT_ORIG, sizeof(MAT_ORIG), buf, n);
    n = r2_callout(buf, MVP_HEAD, sizeof(MVP_HEAD), (uint32_t)(uintptr_t)r2_mvpdup, MVP_TAIL);
    s_r2inj[R2P_MVP] = r2_inject(modid, R2P_MVP, r2->mvp_patch, MVP_ORIG, sizeof(MVP_ORIG), buf, n);
    if (s_mask & OPT_H2F) {   // no run-time switch: in for the session if asked for at start
        int e = h2f_selftest();
        if (e != 0) {
            s_r2err[R2P_H2F] = e;
        } else {
            uint8_t orig[20];
            int no = h2f_orig(orig);
            s_r2inj[R2P_H2F] = r2_inject(modid, R2P_H2F, r2->h2f, orig, no, H2F_NEW, sizeof(H2F_NEW));
            if (s_r2inj[R2P_H2F] >= 0 && (e = h2f_selftest()) != 0) {
                taiInjectRelease(s_r2inj[R2P_H2F]);
                s_r2inj[R2P_H2F] = -1;
                s_r2err[R2P_H2F] = e;
            }
        }
    }
    if (s_mask & OPT_NOLOG)
        s_r2inj[R2P_LOG] = r2_inject(modid, R2P_LOG, r2->dsc_trace, LOG_ORIG, 2, LOG_NEW, 2);

    // hooks. ureuse reads T's flags with word loads: only if they are aligned as
    // expected, and only if every scene start and end reaches probe_scene()
    // (a block must never be reused after GXM recycled its ring memory)
    if (((uintptr_t)(t + 0x1A2C) & 3) == 0 && s_scenes_tracked) {
        s_r2hook[R2_URES] = taiHookFunctionImport(&s_r2ref[R2_URES], TAI_MAIN_MODULE, NID_LIB_GXM,
                NID_RESERVE_VDUB, ur_reserve_hook);
        if (s_r2hook[R2_URES] >= 0)
            s_r2hook[R2_PRE] = taiHookFunctionOffset(&s_r2ref[R2_PRE], modid, 0, r2->predraw, 1, predraw_hook);
    }
    s_r2hook[R2_PAL] = taiHookFunctionOffset(&s_r2ref[R2_PAL], modid, 0, r2->palette, 1, palette_hook);
    s_r2hook[R2_TEX] = taiHookFunctionOffset(&s_r2ref[R2_TEX], modid, 0, r2->texparam, 1, texparam_hook);
    s_r2hook[R2_KS] = taiHookFunctionOffset(&s_r2ref[R2_KS], modid, 0, r2->keyset_loop, 1, keyset_hook);
    // the experiments are hooked only if asked for at start: off, the game runs without them
    if (s_mask & (OPT_EARLYKICK | OPT_EKCHECK)) {
        s_r2hook[R2_KICK] = taiHookFunctionOffset(&s_r2ref[R2_KICK], modid, 0, r2->base_kick, 1, kick_hook);
        if (s_r2hook[R2_KICK] >= 0)
            s_r2hook[R2_DSC] = taiHookFunctionOffset(&s_r2ref[R2_DSC], modid, 0, r2->dsc_run, 1, dsc_hook);
    }
    if (s_mask & OPT_UCOPY) {
        // fragment first: once the vertex hook is in, it calls the fragment entry (hooked) itself
        s_r2hook[R2_FBIND] = taiHookFunctionOffset(&s_r2ref[R2_FBIND], modid, 0, r2->fbind, 1, fbind_hook);
        if (s_r2hook[R2_FBIND] >= 0)
            s_r2hook[R2_VBIND] = taiHookFunctionOffset(&s_r2ref[R2_VBIND], modid, 0, r2->vbind, 1, vbind_hook);
    }
}

static void r2_shutdown(void) {
    for (int i = R2_HOOKS - 1; i >= 0; i--) {
        if (s_r2hook[i] >= 0)
            taiHookRelease(s_r2hook[i], s_r2ref[i]);
        s_r2hook[i] = -1;
    }
    for (int i = 0; i < R2_PATCHES; i++) {
        if (s_r2inj[i] >= 0)
            taiInjectRelease(s_r2inj[i]);
        s_r2inj[i] = -1;
    }
}

// part 0: what is in; 1: ureuse; 2: ureuse per stage; 3: the other counters
// (each under 256 bytes with 10-digit counters)
int opt_diag2(char *buf, int size, int part) {
    if (!s_r2)
        return sceClibSnprintf(buf, size, "r2: none for this game");
    if (part == 0) {
        unsigned int hooks = 0;
        for (int i = 0; i < R2_HOOKS; i++)
            hooks |= (unsigned int)(s_r2hook[i] >= 0) << i;
        return sceClibSnprintf(buf, size, "r2: hooks %X patches %X %X %X %X err %X %X %X %X",
                hooks, s_r2inj[R2P_MAT], s_r2inj[R2P_MVP], s_r2inj[R2P_H2F], s_r2inj[R2P_LOG],
                (unsigned)s_r2err[0], (unsigned)s_r2err[1], (unsigned)s_r2err[2], (unsigned)s_r2err[3]);
    }
    if (part == 1)
        return sceClibSnprintf(buf, size,
                "r2 ureuse%s: calls %u dirty %u changed %u miss %u skip %u rec %u 3d %u; check %u bad %u n/a %u",
                s_ur_off ? " OFF (bad)" : "", d_ur_calls, d_ur_dirty, d_ur_changed, d_ur_miss, d_ur_skip, d_ur_rec,
                d_ur_other, d_ur_vchk, d_ur_vbad, d_ur_vna);
    if (part == 2)
        return sceClibSnprintf(buf, size, "r2 ureuse skip/calls: s2 %u/%u s3 %u/%u s4 %u/%u s5 %u/%u",
                d_ur_st_skip[2], d_ur_st_calls[2], d_ur_st_skip[3], d_ur_st_calls[3],
                d_ur_st_skip[4], d_ur_st_calls[4], d_ur_st_skip[5], d_ur_st_calls[5]);
    return sceClibSnprintf(buf, size,
            "r2: mat %u mvp %u tex %u; earlykick %u gated %u late %u lost %u; ekcheck %u diff %u; ucopy %u",
            d_mat_fast, d_mvp_fast, d_tex_fast,
            d_ek_kicks, d_ek_gated, d_ek_late, d_ek_lost, d_ek_chk, d_ek_chk_diff, d_uc_vbind);
}

void opt_set_scenes_tracked(int ok) {
    s_scenes_tracked = ok;
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
    if (s_r2)
        r2_frame();
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
        if (cuts->r2) {
            r2_init(modid, cuts->r2);
            s_r2_reuse = s_r2hook[R2_PRE] >= 0;
        }
    }
    return s_matmul_hook;
}

void opt_shutdown(void) {
    s_stopping = 1;
    r2_shutdown();
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

#ifndef DIVASPLIT_CUTS_H
#define DIVASPLIT_CUTS_H

/*
 * The plain algorithms of opt.c's f 2nd cuts, apart so test/test_cuts.c can
 * check them on the host against the game's versions.
 */

#include <arm_neon.h>
#include <stdint.h>

typedef void (*mul44_fn)(float *out, const float *a, const float *b);   // out = a * b, out may be a or b
typedef void (*inv44_fn)(float *m);                                    // in place
typedef void (*xform_fn)(const float *m, const float *v, float *out);  // m * [v; 1], 3 floats
typedef int (*less_fn)(const void *a, const void *b);

/*
 * Sort-key centre of a skinned object (f 2nd seg0+0x245CDC), for the case the
 * game averages over bones: obj+0x10 > 0 and n = (*obj)+0x20 > 0. The game
 * computes, per bone, xform(inverse(W) * B_i * (W * X), c) and divides the sum
 * by n; each step is linear in B_i, so this sums the bones first:
 * xform(inverse(W) * sum(B_i) * (W * X), c) / n. Bone matrices (4x4 floats)
 * are contiguous at *(obj+0x14), the centre c at (*obj)+4.
 */
static inline void cuts_centre(const float *w, const float *x, const uint8_t *obj, float *out,
        mul44_fn mul, inv44_fn inv, xform_fn xform) {
    const uint8_t *sub = *(const uint8_t *const *)obj;
    uint32_t n = *(const uint32_t *)(sub + 0x20);
    float wi[16] __attribute__((aligned(16)));
    float wx[16] __attribute__((aligned(16)));
    float m[16] __attribute__((aligned(16)));
    float p[3];
    for (int i = 0; i < 16; i++)
        wi[i] = wx[i] = w[i];
    inv(wi);
    mul(wx, wx, x);
    const float *b = *(const float *const *)(obj + 0x14);
    float32x4_t a0 = vld1q_f32(b), a1 = vld1q_f32(b + 4), a2 = vld1q_f32(b + 8), a3 = vld1q_f32(b + 12);
    for (uint32_t i = 1; i < n; i++) {
        b += 16;
        a0 = vaddq_f32(a0, vld1q_f32(b));
        a1 = vaddq_f32(a1, vld1q_f32(b + 4));
        a2 = vaddq_f32(a2, vld1q_f32(b + 8));
        a3 = vaddq_f32(a3, vld1q_f32(b + 12));
    }
    vst1q_f32(m, a0);
    vst1q_f32(m + 4, a1);
    vst1q_f32(m + 8, a2);
    vst1q_f32(m + 12, a3);
    mul(m, wi, m);   // the game's order: (Wi * B) * (W * X)
    mul(m, m, wx);
    xform(m, (const float *)(sub + 4), p);
    float r = 1.0f / (float)n;   // as the game: sum * (1 / n)
    out[0] = p[0] * r;
    out[1] = p[1] * r;
    out[2] = p[2] * r;
}

// std::list<T *> (Dinkumware): {+4 head sentinel, +8 size}; node {+0 next, +4 prev, +8 value}
typedef struct lnode {
    struct lnode *next, *prev;
    void *val;
} lnode_t;
typedef struct {
    uint32_t alloc;
    lnode_t *head;
    uint32_t size;
} dlist_t;

/*
 * std::list::sort(less) without allocating: bottom-up merge sort over two
 * arrays of size >= l->size. Like the game's merge, a right element goes first
 * only if less(right, left), so it is stable: the same order as the game's sort.
 */
static inline void cuts_list_sort(dlist_t *l, less_fn less, lnode_t **a, lnode_t **b) {
    uint32_t n = l->size;
    lnode_t *h = l->head, *x = h->next;
    lnode_t **src = a, **dst = b;
    for (uint32_t i = 0; i < n; i++, x = x->next)
        src[i] = x;
    for (uint32_t w = 1; w < n; w *= 2) {
        for (uint32_t lo = 0; lo < n; lo += 2 * w) {
            uint32_t mid = lo + w < n ? lo + w : n;
            uint32_t hi = lo + 2 * w < n ? lo + 2 * w : n;
            uint32_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi)
                dst[k++] = less(src[j]->val, src[i]->val) ? src[j++] : src[i++];
            while (i < mid)
                dst[k++] = src[i++];
            while (j < hi)
                dst[k++] = src[j++];
        }
        lnode_t **t = src;
        src = dst;
        dst = t;
    }
    lnode_t *prev = h;
    for (uint32_t i = 0; i < n; i++) {
        prev->next = src[i];
        src[i]->prev = prev;
        prev = src[i];
    }
    prev->next = h;
    h->prev = prev;
}

// every byte of b[0, n) = c, comparing a word at a time and writing only the
// words that differ (most flushes have no flag set: then nothing is written).
// No library call: sceClibMemset, once per flush, made the first build's 2D
// stages slower (A/B 2026-10-10: stage 3 -0.65 ms, the rest +0.35 ms)
typedef uint32_t __attribute__((may_alias)) cuts_u32a;
static inline __attribute__((always_inline)) void cuts_fill_changed(uint8_t *b, uint8_t c, unsigned int n) {
    uint32_t cw = c * 0x01010101u;
    for (; n != 0 && ((uintptr_t)b & 3) != 0; n--, b++)
        if (*b != c)
            *b = c;
    for (; n >= 4; n -= 4, b += 4)
        if (*(cuts_u32a *)b != cw)
            *(cuts_u32a *)b = cw;
    for (; n != 0; n--, b++)
        if (*b != c)
            *b = c;
}

/*
 * The f 2nd dirty-flag patch (32 bytes at seg0+0x4DF012, inside 0x4DEF04; r4 =
 * the uniform table T, callee-saved; lr was pushed on entry). The game's loop
 * there clears T+0x1A2A..+0x1B09 one byte at a time. The patch calls
 * fn(T + 0x1A2A, 0, 0xE0) instead, through ip (fn is the plugin's: out of BL
 * range), then goes on to the 2-byte loop at 0x4DF032, which stores r1 = 0:
 *
 *   movw r0, #0x1a2a ; add r0, r4 ; movs r1, #0 ; movs r2, #0xe0
 *   movw ip, #lo(fn) ; movt ip, #hi(fn) ; blx ip
 *   movs r1, #0 ; b 0x4DF032 ; 4x nop
 *
 * fn has memset's signature and register use (AAPCS: r0-r3, ip, lr clobbered).
 */
static inline void cuts_thumb_movw(uint8_t *o, int top, unsigned int rd, uint32_t imm) {
    uint16_t hw1 = (uint16_t)((top ? 0xF2C0 : 0xF240) | (((imm >> 11) & 1) << 10) | ((imm >> 12) & 0xF));
    uint16_t hw2 = (uint16_t)((((imm >> 8) & 7) << 12) | (rd << 8) | (imm & 0xFF));
    o[0] = (uint8_t)hw1;
    o[1] = (uint8_t)(hw1 >> 8);
    o[2] = (uint8_t)hw2;
    o[3] = (uint8_t)(hw2 >> 8);
}

static inline void cuts_dirty_patch(uint8_t out[32], uint32_t fn_thumb) {
    static const uint8_t head[10] = {
        0x41, 0xf6, 0x2a, 0x20,   // movw r0, #0x1a2a
        0x20, 0x44,               // add r0, r4
        0x00, 0x21,               // movs r1, #0
        0xe0, 0x22};              // movs r2, #0xe0
    static const uint8_t tail[14] = {
        0xe0, 0x47,               // blx ip
        0x00, 0x21,               // movs r1, #0
        0x03, 0xe0,               // b 0x4DF032
        0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf};
    for (int i = 0; i < 10; i++)
        out[i] = head[i];
    cuts_thumb_movw(out + 10, 0, 12, fn_thumb & 0xFFFF);
    cuts_thumb_movw(out + 14, 1, 12, fn_thumb >> 16);
    for (int i = 0; i < 14; i++)
        out[18 + i] = tail[i];
}


/*
 * Round 2: the ROB_BASE key-set evaluation, f 2nd seg0+0x204334 (one key set;
 * the loop 0x20462A calls it once per set), transcribed instruction by
 * instruction so the output bits and the cached index are the game's. The
 * RobThreads run it for every animated bone channel, so the point is to drop
 * the per-set call, the stack canary, the 15-register save and the separate
 * half -> float call (0x4B1A48).
 */
typedef struct {
    int32_t type;            // +0x00: <= 0 or > 3: 0, 1: constant, 2: smooth step, 3: Hermite
    int32_t n;               // +0x04 key count
    int32_t cache;           // +0x08 index found last time (written back)
    int32_t window;          // +0x0C how far past the cached key the cache is still tried
    uint32_t flags;          // +0x10 bit 0: values are half floats
    const int16_t *frame;    // +0x14 key frames
    const uint32_t *tangent; // +0x18 type 3 tangents (float bits)
    const uint32_t *value;   // +0x1C values (float bits)
    const uint16_t *hvalue;  // +0x20 values (half floats)
} keyset_t;                  // 0x24 bytes, the stride 0x20462A uses

// f 2nd seg0+0x4B1A48: half -> float bits, IEEE exact, NaN payloads kept
static inline uint32_t cuts_h2f_bits(uint32_t h) {
    uint32_t s = (h << 16) & 0x80000000u, e = (h >> 10) & 31, m = h & 0x3FF;
    if (e == 0) {
        if (m == 0)
            return s;
        uint32_t c = (uint32_t)__builtin_clz(m);   // 22..31: normalize the subnormal
        return s | ((134 - c) << 23) | ((m << (c - 8)) & ~0x800000u);
    }
    if (e == 31)
        return s | 0x7F800000u | (m << 13);
    return s + ((h & 0x7FFF) << 13) + 0x38000000u;
}

static inline uint32_t cuts_ks_bits(const keyset_t *k, int32_t i) {
    return (k->flags & 1) ? cuts_h2f_bits(k->hvalue[i]) : k->value[i];
}

static inline float cuts_bits_f(uint32_t b) {
    union { uint32_t u; float f; } x = {b};
    return x.f;
}

static inline uint32_t cuts_f_bits(float f) {
    union { float f; uint32_t u; } x = {f};
    return x.u;
}

// 0x20451E..0x204536: (t2 * ((t + t) - 3)) * (v0 - v1) + v0, the game's operand order
static inline float cuts_ks_step(float t, float t2, float v0, float v1) {
#ifdef __arm__
    float r, k3, a, d;
    __asm__("vmov.f32 %[k3], #3.0\n\t"
            "vadd.f32 %[a], %[t], %[t]\n\t"
            "vsub.f32 %[d], %[v0], %[v1]\n\t"
            "vsub.f32 %[a], %[a], %[k3]\n\t"
            "vmul.f32 %[a], %[t2], %[a]\n\t"
            "vmul.f32 %[d], %[a], %[d]\n\t"
            "vadd.f32 %[r], %[d], %[v0]"
            : [r] "=&t"(r), [k3] "=&t"(k3), [a] "=&t"(a), [d] "=&t"(d)
            : [t] "t"(t), [t2] "t"(t2), [v0] "t"(v0), [v1] "t"(v1));
    return r;
#else
    float a = (t + t) - 3.0f;
    a = t2 * a;
    float d = v0 - v1;
    d = a * d;
    return d + v0;
#endif
}

// 0x204596..0x2045BE: Hermite, the game's order (vmla = product rounded, then added)
static inline float cuts_ks_hermite(float t, float t2, float dt, float v0, float v1, float g0, float g1) {
#ifdef __arm__
    float one, u, d, a, uu, b;
    __asm__("vmov.f32 %[one], #1.0\n\t"
            "vsub.f32 %[u], %[t], %[one]\n\t"
            "vsub.f32 %[d], %[v0], %[v1]\n\t"
            "vmul.f32 %[a], %[g0], %[u]\n\t"
            "vadd.f32 %[uu], %[u], %[u]\n\t"
            "vmla.f32 %[a], %[g1], %[t]\n\t"
            "vsub.f32 %[uu], %[uu], %[one]\n\t"
            "vmul.f32 %[b], %[a], %[u]\n\t"
            "vmul.f32 %[uu], %[t2], %[uu]\n\t"
            "vmla.f32 %[v0], %[b], %[dt]\n\t"
            "vmla.f32 %[v0], %[uu], %[d]"
            : [one] "=&t"(one), [u] "=&t"(u), [d] "=&t"(d), [a] "=&t"(a), [uu] "=&t"(uu), [b] "=&t"(b),
              [v0] "+&t"(v0)
            : [t] "t"(t), [t2] "t"(t2), [dt] "t"(dt), [v1] "t"(v1), [g0] "t"(g0), [g1] "t"(g1));
    return v0;
#else
    float u = t - 1.0f, d = v0 - v1, a = g0 * u, uu = u + u;
    a = a + g1 * t;
    uu = uu - 1.0f;
    float b = a * u;
    uu = t2 * uu;
    v0 = v0 + b * dt;
    return v0 + uu * d;
#endif
}

// vcvt.s32.f32: truncates toward zero, saturates, NaN -> 0. On ARM the cast is
// that instruction; elsewhere an out-of-range cast is undefined, so spell it out
static inline int32_t cuts_ks_cvt(float f) {
#ifdef __arm__
    return (int32_t)f;
#else
    if (f != f)
        return 0;
    if (f >= 2147483648.0f)
        return INT32_MAX;
    if (f <= -2147483648.0f)
        return INT32_MIN;
    return (int32_t)f;
#endif
}

static inline int cuts_ks_le(float a, float b) {   // vcmp (quiet) + bls: a <= b, false if unordered
    return __builtin_islessequal(a, b);
}

static inline void cuts_keyset_eval(uint32_t *out, keyset_t *k, float fr) {
    int32_t ty = k->type;
    if (ty <= 0 || ty > 3) {                     // 0x204350..0x20435A
        *out = 0;
        return;
    }
    if (ty == 1) {                               // 0x20435E: bit copy of value[0]
        *out = k->value[0];
        return;
    }
    const int16_t *f = k->frame;
    int32_t n = k->n, c = k->cache;
    int32_t fi = (int16_t)cuts_ks_cvt(fr);       // vcvt.s32.f32, sxth
    int32_t p;
    if (c <= n && fi <= k->window + f[c] && (c <= 0 || fi >= f[c - 1])) {
        for (p = c; p != n && fi >= f[p]; p++)   // 0x20445A: linear from the cached key
            ;
    } else {                                     // 0x2043A6: upper_bound over [0, n)
        int32_t cnt = n;
        p = 0;
        while (cnt > 0) {
            int32_t half = cnt / 2;
            if (fi < f[p + half]) {
                cnt = half;
            } else {
                p += half + 1;
                cnt -= half + 1;
            }
        }
    }
    int32_t i = p, st = 0;
    float f1 = 0.0f;
    for (; i < n; i++) {                         // 0x2043F6..0x204422
        f1 = (float)f[i];
        if (cuts_ks_le(fr, f1)) {                // 0x204476: on a key (1e-6), first key, or between
            float dd = f1 - fr;
            st = cuts_ks_le(__builtin_fabsf(dd), cuts_bits_f(0x358637BDu)) ? 1 : (i <= 0 ? 2 : 3);
            break;
        }
    }
    if (st != 3) {                               // 0x204430..0x204456
        if (st == 0)
            i--;
        *out = cuts_ks_bits(k, i);
    } else {                                     // 0x2044B8..
        float f0 = (float)f[i - 1];
        float dt = fr - f0;
        float t = dt / (f1 - f0);
        float t2 = t * t;
        float v0 = cuts_bits_f(cuts_ks_bits(k, i - 1));
        float v1, r;
        if (ty == 2) {
            v1 = cuts_bits_f(cuts_ks_bits(k, i));
            r = cuts_ks_step(t, t2, v0, v1);
        } else {
            float g0 = cuts_bits_f(k->tangent[i - 1]);
            v1 = cuts_bits_f(cuts_ks_bits(k, i));
            float g1 = cuts_bits_f(k->tangent[i]);
            r = cuts_ks_hermite(t, t2, dt, v0, v1, g0, g1);
        }
        *out = cuts_f_bits(r);
    }
    k->cache = i;                                // 0x204456
}

#endif

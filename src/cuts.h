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

#endif

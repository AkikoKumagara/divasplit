/* Host test for src/cuts.h (needs NEON: an arm64 Mac or ARM Linux):
 *   cc -O2 -ffp-contract=off -I../src -o test_cuts test_cuts.c && ./test_cuts
 * Checks the sort-key centre against the game's per-bone loop (f 2nd
 * seg0+0x245CDC, with its matrix helpers as disassembled) and the list sort
 * against a reference stable sort.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cuts.h"

/* seg0+0x4D0FDE: out[i][j] = a[i][1]*b[1][j] + a[i][0]*b[0][j] + a[i][2]*b[2][j] + a[i][3]*b[3][j],
   all loads before stores */
static void game_mul(float *out, const float *a, const float *b) {
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = a[4 * i + 1] * b[4 + j];
            s += a[4 * i] * b[j];
            s += a[4 * i + 2] * b[8 + j];
            s += a[4 * i + 3] * b[12 + j];
            r[4 * i + j] = s;
        }
    memcpy(out, r, sizeof(r));
}

/* seg0+0x4D1DC6 */
static void game_xform(const float *m, const float *v, float *out) {
    float o[3];
    for (int k = 0; k < 3; k++) {
        float s = m[4 * k + 1] * v[1];
        s += m[4 * k] * v[0];
        s += m[4 * k + 2] * v[2];
        o[k] = s + m[4 * k + 3];
    }
    memcpy(out, o, sizeof(o));
}

/* any inverse will do: both versions call the same one */
static void inv(float *m) {
    double a[4][8];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++)
            a[i][j] = j < 4 ? m[4 * i + j] : (j - 4 == i);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++)
            if (fabs(a[r][c]) > fabs(a[p][c]))
                p = r;
        for (int j = 0; j < 8; j++) {
            double t = a[c][j];
            a[c][j] = a[p][j];
            a[p][j] = t;
        }
        double d = a[c][c];
        for (int j = 0; j < 8; j++)
            a[c][j] /= d;
        for (int r = 0; r < 4; r++)
            if (r != c) {
                double f = a[r][c];
                for (int j = 0; j < 8; j++)
                    a[r][j] -= f * a[c][j];
            }
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            m[4 * i + j] = (float)a[i][j + 4];
}

/* the game's loop, as disassembled */
static void game_centre(const float *w, const float *x, const uint8_t *obj, float *out) {
    const uint8_t *sub = *(const uint8_t *const *)obj;
    const float *c = (const float *)(sub + 4);
    uint32_t n = *(const uint32_t *)(sub + 0x20);
    float wi[16], wx[16], t[16], p[3];
    memcpy(wi, w, 64);
    inv(wi);
    memcpy(wx, w, 64);
    game_mul(wx, wx, x);
    float s0 = 0, s1 = 0, s2 = 0;
    const float *b = *(const float *const *)(obj + 0x14);
    for (uint32_t i = 0; i < n; i++) {
        memcpy(t, wi, 64);
        game_mul(t, t, b + 16 * i);
        game_mul(t, t, wx);
        game_xform(t, c, p);
        s0 += p[0];
        s1 += p[1];
        s2 += p[2];
    }
    float r = 1.0f / (float)n;
    out[0] = s0 * r;
    out[1] = s1 * r;
    out[2] = s2 * r;
}

static float frand(float lo, float hi) {
    return lo + (hi - lo) * (float)rand() / (float)RAND_MAX;
}

/* rotation about a random axis, translation t, row-major with translation in column 3 */
static void rand_rt(float *m, float tmax) {
    float ax = frand(-1, 1), ay = frand(-1, 1), az = frand(-1, 1);
    float l = sqrtf(ax * ax + ay * ay + az * az) + 1e-6f;
    ax /= l, ay /= l, az /= l;
    float a = frand(-3.14f, 3.14f), cs = cosf(a), sn = sinf(a), ic = 1 - cs;
    float r[9] = {cs + ax * ax * ic, ax * ay * ic - az * sn, ax * az * ic + ay * sn,
            ay * ax * ic + az * sn, cs + ay * ay * ic, ay * az * ic - ax * sn,
            az * ax * ic - ay * sn, az * ay * ic + ax * sn, cs + az * az * ic};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++)
            m[4 * i + j] = r[3 * i + j];
        m[4 * i + 3] = frand(-tmax, tmax);
    }
    m[12] = m[13] = m[14] = 0;
    m[15] = 1;
}

static int test_centre(void) {
    double max_rel = 0, max_abs = 0;
    for (int it = 0; it < 20000; it++) {
        static float bones[256 * 16];
        uint32_t n = 1 + rand() % 120;
        float w[16], x[16];
        rand_rt(w, 20);
        rand_rt(x, 5);
        for (uint32_t i = 0; i < n; i++) {
            rand_rt(bones + 16 * i, 2);
            for (int k = 0; k < 3; k++)
                bones[16 * i + 4 * k + 3] += w[4 * k + 3];   // bones near the object
        }
        uint8_t sub[0x40] = {0}, obj[0x40] = {0};
        float c[3] = {frand(-1, 1), frand(-1, 1), frand(-1, 1)};
        memcpy(sub + 4, c, 12);
        memcpy(sub + 0x20, &n, 4);
        const uint8_t *subp = sub;
        const float *bp = bones;
        int32_t nb = 1;
        memcpy(obj, &subp, sizeof(subp));
        memcpy(obj + 0x10, &nb, 4);
        memcpy(obj + 0x14, &bp, sizeof(bp));
        float ref[3], got[3];
        game_centre(w, x, obj, ref);
        cuts_centre(w, x, obj, got, game_mul, inv, game_xform);
        for (int k = 0; k < 3; k++) {
            double d = fabs((double)got[k] - ref[k]);
            double a = fabs(ref[k]) > 1 ? fabs(ref[k]) : 1;
            if (d > max_abs)
                max_abs = d;
            if (d / a > max_rel)
                max_rel = d / a;
        }
    }
    printf("centre: max abs diff %.3g, max rel diff %.3g (keys up to ~40)\n", max_abs, max_rel);
    return max_rel < 1e-4 ? 0 : 1;
}

typedef struct {
    float key;
    int id;
} item_t;
static int calls;
static int less_key(const void *a, const void *b) {
    calls++;
    return ((const item_t *)a)->key < ((const item_t *)b)->key;
}

static int test_sort(void) {
    static item_t items[1500];
    static lnode_t nodes[1500], head;
    static lnode_t *ta[1500], *tb[1500];
    static item_t *ref[1500];
    for (int it = 0; it < 3000; it++) {
        uint32_t n = it < 40 ? (uint32_t)it : 1 + rand() % 1500;
        int keys = 1 + rand() % 50;   // many equal keys: stability matters
        for (uint32_t i = 0; i < n; i++) {
            items[i].key = (float)(rand() % keys);
            items[i].id = (int)i;
            ref[i] = &items[i];
        }
        // reference: insertion sort, stable
        for (uint32_t i = 1; i < n; i++) {
            item_t *v = ref[i];
            uint32_t j = i;
            while (j > 0 && v->key < ref[j - 1]->key) {
                ref[j] = ref[j - 1];
                j--;
            }
            ref[j] = v;
        }
        head.next = head.prev = &head;
        for (uint32_t i = 0; i < n; i++) {
            nodes[i].val = &items[i];
            nodes[i].prev = head.prev;
            nodes[i].next = &head;
            head.prev->next = &nodes[i];
            head.prev = &nodes[i];
        }
        dlist_t l = {0, &head, n};
        if (n >= 2)
            cuts_list_sort(&l, less_key, ta, tb);
        lnode_t *p = head.next;
        for (uint32_t i = 0; i < n; i++, p = p->next) {
            if (p->val != ref[i] || p->next->prev != p) {
                printf("sort: mismatch n=%u at %u\n", n, i);
                return 1;
            }
        }
        if (p != &head || head.prev->next != &head) {
            printf("sort: bad links n=%u\n", n);
            return 1;
        }
    }
    printf("sort: 3000 lists match a stable sort\n");
    return 0;
}

/* prints the dirty-flag patch for a sample plugin address, for checking with a
   disassembler (the bytes go at seg0+0x4DF012) */
static void print_patch(void) {
    static const uint32_t fns[] = {0x98123457u, 0x8FFFF801u, 0x81A5C80Du};
    for (int k = 0; k < 3; k++) {
        uint8_t p[32];
        cuts_dirty_patch(p, fns[k]);
        printf("dirty patch (fn 0x%08X): ", fns[k]);
        for (int i = 0; i < 32; i++)
            printf("%02x", p[i]);
        printf("\n");
    }
}

// cuts_fill_changed against memset, every alignment and length, random contents
static int test_fill(void) {
    static uint8_t a[300], b[300];
    int bad = 0;
    for (int off = 0; off < 8; off++)
        for (unsigned int n = 0; n <= 240; n++)
            for (int rep = 0; rep < 20; rep++) {
                for (int i = 0; i < 300; i++)
                    a[i] = b[i] = (rand() & 3) ? 0 : (uint8_t)rand();
                uint8_t c = rep & 1 ? 0 : (uint8_t)rand();
                memset(a + off, c, n);
                cuts_fill_changed(b + off, c, n);
                if (memcmp(a, b, sizeof(a)) != 0)
                    bad++;
            }
    printf("fill: %d mismatches against memset\n", bad);
    return bad != 0;
}

int main(void) {
    srand(1);
    print_patch();
    int bad = test_centre();
    bad |= test_sort();
    bad |= test_fill();
    printf(bad ? "FAILED\n" : "ok\n");
    return bad;
}

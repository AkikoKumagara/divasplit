/*
 * Host test of the round-2 ports in src/cuts.h:
 *
 *   cc -O2 -ffp-contract=off -Isrc -o test_r2 test/test_r2.c && ./test_r2 test/vectors/keyset.txt
 *
 * - cuts_h2f_bits (f 2nd 0x4B1A48) for all 65536 halves against a reference written
 *   from the IEEE definition (NaN payloads kept, as the game's software conversion does).
 *   With "-h file" also against a table of the game's own outputs ("hhhh xxxxxxxx").
 * - cuts_keyset_eval (0x204334) against vectors made by emulating the game's machine
 *   code (0x204334 + 0x4B1A48) on random key sets. One per line, hex:
 *     type n cache window flags frame want_out want_cache, then for keys -1..n+1:
 *     frames, tangents, values, half values
 *   test/vectors/keyset.txt is a sample (400) of the 80000 the port was checked with.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cuts.h"

static uint32_t ref_h2f(uint32_t h) {
    uint32_t s = (h & 0x8000u) << 16, e = (h >> 10) & 31, m = h & 0x3FF;
    if (e == 31)
        return s | 0x7F800000u | (m << 13);
    float f = e ? ldexpf((float)(m | 0x400), (int)e - 25) : ldexpf((float)m, -24);
    return s | cuts_f_bits(f);
}

int main(int argc, char **argv) {
    int bad = 0, a = 1;
    for (uint32_t h = 0; h < 0x10000; h++)
        if (cuts_h2f_bits(h) != ref_h2f(h) && bad++ < 5)
            printf("h2f %04x: %08x want %08x\n", h, cuts_h2f_bits(h), ref_h2f(h));
    printf("h2f: 65536 values, %d mismatches against IEEE\n", bad);
    if (a + 1 < argc && strcmp(argv[a], "-h") == 0) {
        FILE *fh = fopen(argv[a + 1], "r");
        unsigned h, b;
        int n = 0, gbad = 0;
        while (fh && fscanf(fh, "%x %x", &h, &b) == 2) {
            n++;
            if (cuts_h2f_bits(h) != b && gbad++ < 5)
                printf("h2f %04x: %08x, the game %08x\n", h, cuts_h2f_bits(h), b);
        }
        if (fh)
            fclose(fh);
        printf("h2f: %d values, %d mismatches against the game\n", n, gbad);
        bad += gbad + (n != 0x10000);
        a += 2;
    }
    long tests = 0, kbad = 0;
    static char line[1 << 16];
    for (; a < argc; a++) {
        FILE *f = fopen(argv[a], "r");
        if (!f) {
            printf("can't open %s\n", argv[a]);
            return 1;
        }
        while (fgets(line, sizeof(line), f)) {
            uint32_t v[512];
            int nv = 0;
            for (char *p = strtok(line, " \n"); p && nv < 512; p = strtok(NULL, " \n"))
                v[nv++] = (uint32_t)strtoul(p, NULL, 16);
            int32_t n = (int32_t)v[1];
            int m = n + 3;   // keys -1..n+1
            if (n < 0 || m > 64 || nv != 8 + 4 * m) {
                printf("bad line in %s\n", argv[a]);
                return 1;
            }
            int16_t fr[64];
            uint32_t tan[64], val[64];
            uint16_t hv[64];
            for (int k = 0; k < m; k++) {
                fr[k] = (int16_t)v[8 + k];
                tan[k] = v[8 + m + k];
                val[k] = v[8 + 2 * m + k];
                hv[k] = (uint16_t)v[8 + 3 * m + k];
            }
            keyset_t ks = {(int32_t)v[0], n, (int32_t)v[2], (int32_t)v[3], v[4], fr + 1, tan + 1, val + 1, hv + 1};
            uint32_t out = 0;   // as in the emulator's memory
            cuts_keyset_eval(&out, &ks, cuts_bits_f(v[5]));
            tests++;
            if ((out != v[6] || (uint32_t)ks.cache != v[7]) && kbad++ < 10)
                printf("MISMATCH type %d n %d cache %d win %d flags %x fr %08x: out %08x want %08x, cache %d want %d\n",
                        (int)v[0], n, (int)v[2], (int)v[3], v[4], v[5], out, v[6], ks.cache, (int)v[7]);
        }
        fclose(f);
    }
    printf("keyset: %ld vectors, %ld mismatches\n", tests, kbad);
    return bad || kbad;
}

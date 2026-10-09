#ifndef DIVASPLIT_OPT_H
#define DIVASPLIT_OPT_H

#include <psp2/types.h>
#include <stdint.h>

// engine optimizations (opt.c); *on switches them at run time
int opt_init(SceUID modid, uint32_t matmul_off, volatile int *on);
void opt_shutdown(void);

// probe: memcpy calls from one call site (seg0 offset of the return address)
typedef struct {
    uint32_t calls, bytes;
    uint32_t hist[5];          // sizes <=16, <=64, <=256, <=1024, more
    uint32_t dst_min, dst_max;
    uint32_t dst_aligned16;    // calls with a 16-byte aligned destination
    // per draw: the whole vertex default uniform block (one reservation)
    uint32_t groups, group_bytes;
    uint32_t same, same_bytes; // identical to the previous draw's block, same scene
} probe_stats_t;

// hooks the shader binding's memcpy / reserve / SetUniformDataF (once);
// offsets are seg0 offsets of the return addresses of those three calls
int probe_start(uint32_t text_base, uint32_t memcpy_ret, uint32_t reserve_ret, uint32_t setdata_ret);
void probe_scene(void);                                   // a scene started: no reuse across
void probe_take(probe_stats_t *out);                      // read and reset

#endif

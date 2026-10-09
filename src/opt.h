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
} probe_stats_t;

int probe_start(uint32_t text_base, uint32_t ret_off);   // hooks memcpy (once)
void probe_take(probe_stats_t *out);                      // read and reset

#endif

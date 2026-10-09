#ifndef DIVASPLIT_OPT_H
#define DIVASPLIT_OPT_H

#include <psp2/types.h>
#include <stdint.h>

// engine optimizations (opt.c); *on switches them at run time
int opt_init(SceUID modid, uint32_t matmul_off, volatile int *on);
void opt_shutdown(void);

// uniform reuse (see opt.c) and its counters
typedef struct {
    uint32_t calls, bytes;      // shader binding memcpys
    uint32_t groups, group_bytes;   // blocks (one per draw) with opt on
    uint32_t same, same_bytes;  // blocks that reused the previous GPU copy
    uint32_t setdata;           // SetUniformDataF into a staged block
    uint32_t overflow;          // writes past the staging buffer (dropped; must stay 0)
} probe_stats_t;

// hooks the shader binding's memcpy / reserve / SetUniformDataF and sceGxmDraw;
// offsets are seg0 offsets of the return addresses of the first three calls.
// Call from module_start, after opt_init: patching these hot imports while the
// game runs crashes it.
int probe_install(uint32_t text_base, uint32_t memcpy_ret, uint32_t reserve_ret, uint32_t setdata_ret);
void probe_enable(int on);                                // keep counters (recording)
void probe_scene(void);                                   // a scene started or ended
void probe_take(probe_stats_t *out);                      // read and reset
int probe_diag(char *buf, int size);                      // cumulative self-checks, for diag.txt
int probe_diag_first(char *buf, int size, int i);         // the first commits in detail (0: none)

#endif

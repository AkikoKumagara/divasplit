#ifndef DIVASPLIT_OPT_H
#define DIVASPLIT_OPT_H

#include <psp2/types.h>
#include <stdint.h>

// f 2nd main-thread cuts (opt.c): seg0 offsets, seg1 for sort0_flag
typedef struct {
    uint32_t centre;              // sort-key centre of a skinned object (bone average)
    uint32_t mul44, inv44, xform; // the game's matrix helpers it uses
    uint32_t dirty_loop;          // dirty-flag clear loop, patched to a memset
    uint32_t list_sort;           // std::list::sort of the draw lists
    uint32_t sort0_flag;          // stage table byte: sort the opaque list (seg1 offset)
} opt_cuts_t;

// which cuts OPT switches on (ux0:data/divasplit/opt.txt)
enum {
    OPT_CENTRE = 1 << 0,
    OPT_DIRTY = 1 << 1,
    OPT_LSORT = 1 << 2,
    OPT_NOSORT0 = 1 << 3,   // changes draw order: off by default
    OPT_PIN = 1 << 4,       // core pinning (threadview.c): measured no faster
    OPT_CHECK = 1 << 5,     // compare every centre with the game's (else 1 in 64)
};
#define OPT_DEFAULT (OPT_CENTRE | OPT_DIRTY | OPT_LSORT)

// engine optimizations (opt.c); *on switches them at run time
int opt_init(SceUID modid, uintptr_t seg0, uintptr_t seg1, uint32_t matmul_off,
        const opt_cuts_t *cuts, volatile int *on);
void opt_shutdown(void);
void opt_set_mask(unsigned int mask);
unsigned int opt_mask(void);
void opt_frame(void);                 // main thread, frame start: applies patches / switches
int opt_diag(char *buf, int size);    // cumulative counters, for diag.txt

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

#endif

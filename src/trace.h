#ifndef DIVASPLIT_TRACE_H
#define DIVASPLIT_TRACE_H

/*
 * Tracer (trace.c): the blocking calls of every game thread (locks, waits,
 * sleeps, file reads, GPU and vblank waits) and the frame / render stage
 * boundaries, with exact times, into ux0:data/divasplit/trace.bin while
 * recording. tools/perfetto.py turns it and prof.bin into a timeline.
 *
 * trace.bin = trace_header_t, then trace_rec_t records.
 */

#include <psp2/types.h>
#include <stdint.h>

#define TRACE_MAGIC    0x43525444   // "DTRC"
#define TRACE_VERSION  1
#define TRACE_NAME_LEN 32

// marks written by divasplit's own hooks; the hooked imports follow
enum {
    TR_FRAME,         // arg: frame number
    TR_STAGE,         // arg: render stage
    TR_VBLANK,        // sceDisplayWaitVblankStartMulti on the main thread
    TR_GXM_BEGIN,
    TR_GXM_END,
    TR_GXM_DQ,
    TR_MARKS
};

typedef struct {
    uint32_t magic, version, header_size, rec_size;
    uint32_t start_lo, start_hi;    // system time of t_us 0 (sceKernelGetSystemTimeWide)
    uint32_t nfuncs;                // entries of names[]
    uint32_t reserved[9];
    char names[64][TRACE_NAME_LEN]; // by func id
} trace_header_t;

typedef struct {
    uint32_t t_us;      // start, since the trace started
    uint32_t dur_us;
    uint32_t thid;
    uint32_t arg;       // first argument: object, fd, microseconds, stage...
    uint32_t caller;    // return address of the call
    uint16_t func;      // TR_* or the index of a hooked import
    uint16_t seq;       // written last; the writer skips records not finished yet
} trace_rec_t;

int trace_install(void);        // module_start: hooks the blocking imports, allocates the buffer
void trace_set(int on);         // record while on (recording)
int trace_on(void);
uint64_t trace_now(void);       // system time, us
void trace_add(int func, uint64_t t0, uint64_t t1, uint32_t arg, uint32_t caller);
void trace_flush(void);         // input thread: write what is finished
void trace_shutdown(void);

#endif

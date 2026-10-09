#ifndef DIVASPLIT_PROF_H
#define DIVASPLIT_PROF_H

/*
 * Shared between divasplit.suprx (game side) and divasplit_prof.skprx
 * (kernel side): the sampling profiler's start arguments and file format.
 *
 * ux0:data/divasplit/prof.bin = prof_header_t, then prof_sample_t records:
 * one per sampled thread at each pause, same t_us.
 */

#include <stdint.h>

#define PROF_MAGIC        0x46525044   // "DPRF"
#define PROF_VERSION      3
#define PROF_STACK_WORDS  24
#define PROF_MAX_THREADS  8
#define PROF_NAME_LEN     32
#define PROF_FILE         "ux0:data/divasplit/prof.bin"
#define PROF_STATS_FILE   "ux0:data/divasplit/prof.txt"

typedef struct {
    uint32_t magic, version, size;
    uint32_t nthreads;        // threads to sample; [0] is the game's main thread
    int32_t thids[PROF_MAX_THREADS];               // user ids
    char names[PROF_MAX_THREADS][PROF_NAME_LEN];   // for the file header
    uint32_t module_nid;
    uint32_t text_base, text_size;   // eboot segment 0
    uint32_t frame_addr;      // &g_frame in divasplit (game process address)
    uint32_t stage_addr;      // &g_cur_stage
    uint32_t rate_hz;
} prof_args_t;

typedef struct {
    uint32_t magic, version, header_size, sample_size;
    uint32_t rate_hz, module_nid, text_base, text_size;
    uint32_t stack_words, nthreads;
    uint32_t reserved[6];
    char names[PROF_MAX_THREADS][PROF_NAME_LEN];   // sampled threads, [0] = main
} prof_header_t;

#define PROF_F_USER     1   // the main thread was in user mode
#define PROF_F_REGS     2   // registers read
#define PROF_F_STACK    4   // stack scanned
#define PROF_F_SYSCALL  8   // in the kernel; pc / lr / sp are where user mode called it

typedef struct {
    uint32_t t_us;            // since the start
    uint32_t frame;           // divasplit frame counter
    int8_t stage;             // RenderStage running (-1: none)
    uint8_t thread;           // index into the sampled threads
    uint8_t flags;            // PROF_F_*
    uint8_t nstack;           // used entries of stack[]
    uint32_t pc, lr, sp, cpsr;    // user mode registers (see PROF_F_USER / PROF_F_SYSCALL)
    uint32_t kpc, kcpsr;          // kernel mode pc and cpsr, when in the kernel
    uint32_t cost_us;         // how long the game was paused for this sample
    uint32_t stack[PROF_STACK_WORDS];   // possible return addresses into the eboot, innermost first
} prof_sample_t;

// system calls of divasplit_prof.skprx; return < 0 on errors
int divasplit_prof_start(const prof_args_t *args);
int divasplit_prof_stop(void);   // number of samples taken

#endif

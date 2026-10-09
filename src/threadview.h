#ifndef DIVASPLIT_THREADVIEW_H
#define DIVASPLIT_THREADVIEW_H

#include <psp2/types.h>

#define TV_CORES 4

typedef struct {
    int valid;
    int samples;                   // samples in the last second
    int run10, ready10, wait10;    // main thread state, tenths of a percent
    int core10[TV_CORES];          // game threads running on each core, tenths of a percent (-1: not seen)
} tv_summary_t;

typedef void (*tv_diag_fn)(const char *fmt, ...);

void tv_init(tv_diag_fn diag);
void tv_shutdown(void);
void tv_set(int on, SceUID main_thid);   // input thread: sample while recording
void tv_thread_seen(SceUID thid);        // hooks: a thread of the game
void tv_get(tv_summary_t *out);
const char *tv_text(int *len);           // status bar line, empty when off
void tv_flush(void);                     // input thread: write threads.csv
// known game threads whose name starts with prefix; returns the count
int tv_threads_named(const char *prefix, SceUID *ids, char (*names)[32], int max);
// optimization: the main thread alone on core 1, the game's other threads on
// cores 0 and 2 (on), or their own masks back (off). Returns threads changed.
int tv_pin(int on, SceUID main_thid);

#endif

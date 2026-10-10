/*
 * Thread view (only while recording): a sampler thread looks at the game about
 * 1000 times a second and logs where the main thread's time goes.
 *
 *   - main thread state: running, ready (runnable, but its core is busy with
 *     another thread) or waiting, and which kernel object it waits on
 *   - each CPU core: share of the time a game thread runs on it (every 10th
 *     sample looks at all game threads)
 *   - every game thread: CPU time used (run clocks), priority, affinity
 *
 * Game threads are found when they are created (sceKernelCreateThread hook in
 * main.c), when they call a hooked function, and when a sample sees them run.
 *
 * The sampler pins itself to one core and can't see that core (-1 in the
 * per-core numbers): while it samples, nothing else runs there. When the main
 * thread may run on that core too, a sample that finds it pushed off by the
 * sampler is left out ("displaced").
 *
 * Only well-known thread manager calls are used. sceKernelGetThreadRunStatus
 * crashed the game (its real signature isn't the one in the SDK headers).
 *
 * Every second the sampler formats a block for ux0:data/divasplit/threads.csv;
 * the input thread writes it out.
 */
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/cpu.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include "settings.h"
#include "threadview.h"

#define TV_FILE      SETTINGS_DIR "/threads.csv"
#define MAX_THREADS  48
#define MAX_WAITS    16
#define PEND_SLOTS   64
#define BLOCK_SIZE   6144
#define TEXT_CHARS   100
#define SAMPLE_US    1000
#define ALL_EVERY    10     // all game threads every 10th sample

typedef struct {
    SceUID uid;
    int alive;
    uint64_t last_run;   // run clocks at the last summary
    int busy10;          // tenths of a percent of one core, last second
} tthread_t;

typedef struct {
    uint32_t type;
    SceUID id;
    unsigned int n;
} twait_t;

static tv_diag_fn s_diag;
static SceUID s_thread = -1;
static volatile int s_run = 1;
static volatile int s_on = 0;
static volatile SceUID s_main = -1;

// hooks -> sampler: new thread ids
static volatile SceUID s_pend[PEND_SLOTS];
static volatile unsigned int s_pend_head = 0;
static unsigned int s_pend_tail = 0;

// sampler only
static SceUID s_self;
static int s_pinned = 0;
static int s_hidden_core = -1;
static int s_logged = 0;
static tthread_t s_thr[MAX_THREADS];
static SceKernelThreadInfo s_info[MAX_THREADS];
static int s_nthr = 0;
static twait_t s_wait[MAX_WAITS];
static int s_nwait;
static unsigned int s_wait_other;
static unsigned int s_n, s_running, s_ready, s_waiting, s_displaced, s_tick;
static unsigned int s_all_n, s_core_main[TV_CORES], s_core_game[TV_CORES];
static uint64_t s_win_start, s_rec_start;

// sampler -> others
static volatile tv_summary_t s_sum;
static char s_text[2][TEXT_CHARS + 1];
static int s_text_len[2];
static volatile int s_text_idx = 0;
static char s_block[BLOCK_SIZE];
static volatile int s_block_len = 0;   // > 0: a block is waiting for the input thread
static SceUID s_fd = -1;               // input thread only

#define PCT10(a, b) ((b) ? (int)((uint64_t)(a) * 1000 / (b)) : 0)
#define P10(v) (v) / 10, (v) % 10

void tv_thread_seen(SceUID thid) {
    if (thid <= 0)
        return;
    unsigned int i = __sync_fetch_and_add(&s_pend_head, 1);
    s_pend[i % PEND_SLOTS] = thid;
}

static int thread_info(SceUID uid, SceKernelThreadInfo *ti);

/*
 * Core pinning. The main thread was ready to run but without a core about 5%
 * of each frame, behind game threads of the same priority. Core 0 has the GXM
 * display queue thread and core 2 the sound mixer (both pinned by the game),
 * so the main thread gets core 1 and every game thread that could run anywhere
 * moves to cores 0 and 2. Threads the game pinned itself stay where they are.
 *
 * Measured on f 2nd (A/B, 99 paired seconds): no help. Ready time only went
 * from 2.7% to 2.5% of the frame, and the main thread losing the freedom to
 * move cost more: cpu +0.6 ms, fps -1..-3. Kept as the OPT experiment.
 */
#define CORE(n) (SCE_KERNEL_CPU_MASK_USER_0 << (n))
static SceUID s_pin_ids[MAX_THREADS];
static int s_pin_masks[MAX_THREADS];
static int s_npin, s_pinned_state;

int tv_pin(int on, SceUID main_thid) {
    if (on == s_pinned_state || main_thid <= 0)
        return 0;
    int changed = 0;
    if (on) {
        s_npin = 0;
        for (int i = 0; i < s_nthr && s_npin < MAX_THREADS; i++) {
            SceKernelThreadInfo ti;
            if (!s_thr[i].alive || thread_info(s_thr[i].uid, &ti) < 0)
                continue;
            int mask = ti.currentCpuAffinityMask, want;
            if (s_thr[i].uid == main_thid)
                want = CORE(1);
            else if (mask == 0 || (mask & SCE_KERNEL_CPU_MASK_USER_ALL) == SCE_KERNEL_CPU_MASK_USER_ALL)
                want = CORE(0) | CORE(2);
            else
                continue;
            if (sceKernelChangeThreadCpuAffinityMask(s_thr[i].uid, want) >= 0) {
                s_pin_ids[s_npin] = s_thr[i].uid;
                s_pin_masks[s_npin] = mask;
                s_npin++;
                changed++;
            }
        }
    } else {
        for (int i = 0; i < s_npin; i++)
            if (sceKernelChangeThreadCpuAffinityMask(s_pin_ids[i], s_pin_masks[i]) >= 0)
                changed++;
        s_npin = 0;
    }
    s_pinned_state = on;
    return changed;
}

int tv_threads_named(const char *prefix, SceUID *ids, char (*names)[32], int max) {
    int n = 0, len = (int)sceClibStrnlen(prefix, 32);
    for (int i = 0; i < s_nthr && n < max; i++) {
        SceKernelThreadInfo ti;
        if (!s_thr[i].alive || thread_info(s_thr[i].uid, &ti) < 0)
            continue;
        if (sceClibStrncmp(ti.name, prefix, len) != 0)
            continue;
        ids[n] = s_thr[i].uid;
        sceClibStrncpy(names[n], ti.name, 31);
        names[n][31] = 0;
        n++;
    }
    return n;
}

void tv_set(int on, SceUID main_thid) {
    s_main = main_thid;
    s_on = on && main_thid > 0;
}

void tv_get(tv_summary_t *out) {
    out->valid = s_on && s_sum.valid;
    out->samples = s_sum.samples;
    out->run10 = s_sum.run10;
    out->ready10 = s_sum.ready10;
    out->wait10 = s_sum.wait10;
    for (int c = 0; c < TV_CORES; c++)
        out->core10[c] = s_sum.core10[c];
}

const char *tv_text(int *len) {
    int i = s_text_idx;
    *len = s_on ? s_text_len[i] : 0;
    return s_text[i];
}

static int thread_info(SceUID uid, SceKernelThreadInfo *ti) {
    sceClibMemset(ti, 0, sizeof(*ti));
    ti->size = sizeof(*ti);
    return sceKernelGetThreadInfo(uid, ti);
}

static void thread_add(SceUID uid) {
    if (uid <= 0 || uid == s_self)
        return;
    int free_slot = -1;
    for (int i = 0; i < s_nthr; i++) {
        if (s_thr[i].alive && s_thr[i].uid == uid)
            return;
        if (!s_thr[i].alive && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0) {
        if (s_nthr >= MAX_THREADS)
            return;
        free_slot = s_nthr++;
    }
    SceKernelThreadInfo ti;
    if (thread_info(uid, &ti) < 0)
        return;
    tthread_t *t = &s_thr[free_slot];
    t->uid = uid;
    t->last_run = ti.runClocks;
    t->busy10 = 0;
    t->alive = 1;
}

static void pend_drain(void) {
    unsigned int head = s_pend_head;
    if (head - s_pend_tail > PEND_SLOTS)
        s_pend_tail = head - PEND_SLOTS;
    while (s_pend_tail != head) {
        SceUID uid = s_pend[s_pend_tail % PEND_SLOTS];
        s_pend_tail++;
        thread_add(uid);
    }
}

static void wait_add(uint32_t type, SceUID id) {
    for (int i = 0; i < s_nwait; i++) {
        if (s_wait[i].type == type && s_wait[i].id == id) {
            s_wait[i].n++;
            return;
        }
    }
    if (s_nwait < MAX_WAITS) {
        s_wait[s_nwait].type = type;
        s_wait[s_nwait].id = id;
        s_wait[s_nwait].n = 1;
        s_nwait++;
    } else {
        s_wait_other++;
    }
}

// object name and kind for a wait id, by asking each object type in turn
static const char *wait_object(SceUID id, char *name, int size) {
    union {
        SceKernelSemaInfo sema;
        SceKernelEventFlagInfo evf;
        SceKernelMutexInfo mutex;
        char pad[256];   // room in case a struct is larger than the headers say
    } u;
    const char *kind = NULL, *src = NULL;
    name[0] = 0;
    if (id <= 0)
        return "";
    sceClibMemset(&u, 0, sizeof(u));
    u.sema.size = sizeof(u.sema);
    if (sceKernelGetSemaInfo(id, &u.sema) >= 0) {
        kind = "sema";
        src = u.sema.name;
    }
    if (!kind) {
        sceClibMemset(&u, 0, sizeof(u));
        u.evf.size = sizeof(u.evf);
        if (sceKernelGetEventFlagInfo(id, &u.evf) >= 0) {
            kind = "eventflag";
            src = u.evf.name;
        }
    }
    if (!kind) {
        sceClibMemset(&u, 0, sizeof(u));
        u.mutex.size = sizeof(u.mutex);
        if (sceKernelGetMutexInfo(id, &u.mutex) >= 0) {
            kind = "mutex";
            src = u.mutex.name;
        }
    }
    if (!kind)
        return "?";
    sceClibStrncpy(name, src, size - 1);
    name[size - 1] = 0;
    return kind;
}

// csv-safe copy of a kernel object name
static void clean_name(char *dst, const char *src, int size) {
    int i;
    for (i = 0; i < size - 1 && src[i]; i++)
        dst[i] = (src[i] == ',' || src[i] < ' ') ? '_' : src[i];
    dst[i] = 0;
}

// keep the sampler off the main thread's core
static void pin_self(void) {
    SceKernelThreadInfo ti;
    int main_mask = 0;
    if (thread_info(s_main, &ti) >= 0)
        main_mask = ti.currentCpuAffinityMask;
    int r = sceKernelChangeThreadCpuAffinityMask(s_self, SCE_KERNEL_CPU_MASK_SYSTEM);
    if (r >= 0) {
        s_hidden_core = 3;
        s_diag("threadview: main thread mask %08X, sampler on the system core", main_mask);
        return;
    }
    int core = 2;
    if (main_mask & SCE_KERNEL_CPU_MASK_USER_ALL) {
        for (int c = 2; c >= 0; c--) {
            if (!(main_mask & (SCE_KERNEL_CPU_MASK_USER_0 << c))) {
                core = c;
                break;
            }
        }
    }
    int r2 = sceKernelChangeThreadCpuAffinityMask(s_self, SCE_KERNEL_CPU_MASK_USER_0 << core);
    s_hidden_core = r2 >= 0 ? core : -1;
    s_diag("threadview: main thread mask %08X, system core %08X, sampler on core %d (%08X)",
            main_mask, r, core, r2);
}

static int cpu_of(const SceKernelThreadInfo *ti) {
    if (ti->currentCpuId >= 0 && ti->currentCpuId < TV_CORES)
        return ti->currentCpuId;
    if (ti->lastExecutedCpuId >= 0 && ti->lastExecutedCpuId < TV_CORES)
        return ti->lastExecutedCpuId;
    return -1;
}

static void sample(void) {
    SceKernelThreadInfo ti;
    if (thread_info(s_main, &ti) >= 0) {
        if (s_logged < 3) {
            s_logged++;
            s_diag("threadview: main status %X cpu %d last %d prio %d wait %08X/%08X run %08X%08X",
                    (unsigned)ti.status, ti.currentCpuId, ti.lastExecutedCpuId, ti.currentPriority,
                    (unsigned)ti.waitType, (unsigned)ti.waitId,
                    (unsigned)(ti.runClocks >> 32), (unsigned)ti.runClocks);
        }
        if ((ti.status & SCE_THREAD_READY) && !(ti.status & SCE_THREAD_RUNNING)
                && cpu_of(&ti) == s_hidden_core) {
            s_displaced++;   // the sampler itself took its core
        } else {
            s_n++;
            if (ti.status & SCE_THREAD_RUNNING)
                s_running++;
            else if (ti.status & SCE_THREAD_READY)
                s_ready++;
            else {
                s_waiting++;
                wait_add(ti.waitType, ti.waitId);
            }
        }
    }

    if (++s_tick % ALL_EVERY)
        return;
    s_all_n++;
    for (int i = 0; i < s_nthr; i++) {
        if (!s_thr[i].alive || thread_info(s_thr[i].uid, &ti) < 0)
            continue;
        if (!(ti.status & SCE_THREAD_RUNNING))
            continue;
        int c = cpu_of(&ti);
        if (c < 0)
            continue;
        s_core_game[c]++;
        if (s_thr[i].uid == s_main)
            s_core_main[c]++;
    }
}

static void window_reset(uint64_t now) {
    s_win_start = now;
    s_n = s_running = s_ready = s_waiting = s_displaced = 0;
    s_nwait = 0;
    s_wait_other = 0;
    s_all_n = 0;
    sceClibMemset(s_core_main, 0, sizeof(s_core_main));
    sceClibMemset(s_core_game, 0, sizeof(s_core_game));
}

#define BLOCK_ADD(...) do { if (n < BLOCK_SIZE - 160) \
        n += sceClibSnprintf(s_block + n, BLOCK_SIZE - n, __VA_ARGS__); } while (0)

static void summary(uint64_t now) {
    uint64_t span = now - s_win_start;
    unsigned int t10 = (unsigned int)((now - s_rec_start) / 100000);

    s_sum.samples = (int)s_n;
    s_sum.run10 = PCT10(s_running, s_n);
    s_sum.ready10 = PCT10(s_ready, s_n);
    s_sum.wait10 = PCT10(s_waiting, s_n);
    for (int c = 0; c < TV_CORES; c++)
        s_sum.core10[c] = (c == s_hidden_core || !s_all_n) ? -1 : PCT10(s_core_game[c], s_all_n);
    s_sum.valid = 1;

    // CPU time of every known thread
    int order[MAX_THREADS], count = 0;
    for (int i = 0; i < s_nthr; i++) {
        tthread_t *t = &s_thr[i];
        if (!t->alive)
            continue;
        if (thread_info(t->uid, &s_info[i]) < 0) {
            t->alive = 0;
            continue;
        }
        uint64_t run = s_info[i].runClocks;
        t->busy10 = span ? (int)((run - t->last_run) * 1000 / span) : 0;
        t->last_run = run;
        // busiest first
        int k = count++;
        while (k > 0 && s_thr[order[k - 1]].busy10 < t->busy10) {
            order[k] = order[k - 1];
            k--;
        }
        order[k] = i;
    }

    // waits of the main thread, most frequent first
    for (int i = 1; i < s_nwait; i++) {
        twait_t w = s_wait[i];
        int k = i;
        while (k > 0 && s_wait[k - 1].n < w.n) {
            s_wait[k] = s_wait[k - 1];
            k--;
        }
        s_wait[k] = w;
    }

    char top_wait[32];
    top_wait[0] = 0;
    int top_wait10 = 0;
    if (s_nwait > 0) {
        char raw[32];
        const char *kind = wait_object(s_wait[0].id, raw, sizeof(raw));
        clean_name(top_wait, raw[0] ? raw : kind, sizeof(top_wait));
        top_wait10 = PCT10(s_wait[0].n, s_n);
    }

    if (s_block_len == 0) {
        int n = 0;
        BLOCK_ADD("%u.%u,M,main,,run %d.%d ready %d.%d wait %d.%d samples %u displaced %u,%d.%d\n",
                t10 / 10, t10 % 10, P10(s_sum.run10), P10(s_sum.ready10), P10(s_sum.wait10),
                s_n, s_displaced, P10(s_sum.run10));
        for (int c = 0; c < TV_CORES; c++) {
            if (s_sum.core10[c] < 0)
                continue;
            BLOCK_ADD("%u.%u,C,%d,,main %d.%d game %d.%d samples %u,%d.%d\n",
                    t10 / 10, t10 % 10, c, P10(PCT10(s_core_main[c], s_all_n)),
                    P10(s_sum.core10[c]), s_all_n, P10(s_sum.core10[c]));
        }
        for (int j = 0; j < count; j++) {
            int i = order[j];
            char name[32];
            clean_name(name, s_info[i].name, sizeof(name));
            BLOCK_ADD("%u.%u,T,%08X,%s,prio %d mask %08X cpu %d%s,%d.%d\n",
                    t10 / 10, t10 % 10, (unsigned)s_thr[i].uid, name,
                    s_info[i].currentPriority, (unsigned)s_info[i].currentCpuAffinityMask,
                    s_info[i].lastExecutedCpuId, s_thr[i].uid == s_main ? " main" : "",
                    P10(s_thr[i].busy10));
        }
        for (int i = 0; i < s_nwait && i < 8; i++) {
            char raw[32], name[32];
            const char *kind = wait_object(s_wait[i].id, raw, sizeof(raw));
            clean_name(name, raw, sizeof(name));
            BLOCK_ADD("%u.%u,W,%08X,%s,type %08X %s,%d.%d\n", t10 / 10, t10 % 10,
                    (unsigned)s_wait[i].id, name, (unsigned)s_wait[i].type, kind,
                    P10(PCT10(s_wait[i].n, s_n)));
        }
        if (s_wait_other)
            BLOCK_ADD("%u.%u,W,other,,,%d.%d\n", t10 / 10, t10 % 10, P10(PCT10(s_wait_other, s_n)));
        __sync_synchronize();
        s_block_len = n;
    }   // else the input thread is behind: this second is dropped

    // status bar line
    int i = s_text_idx ^ 1;
    char *t = s_text[i];
    const int size = sizeof(s_text[i]);
    int n = sceClibSnprintf(t, size, "main run %d rdy %d wait %d  cores",
            (s_sum.run10 + 5) / 10, (s_sum.ready10 + 5) / 10, (s_sum.wait10 + 5) / 10);
    for (int c = 0; c < TV_CORES && n < TEXT_CHARS; c++) {
        if (s_sum.core10[c] < 0)
            n += sceClibSnprintf(t + n, size - n, " -");
        else
            n += sceClibSnprintf(t + n, size - n, " %d", (s_sum.core10[c] + 5) / 10);
    }
    if (top_wait[0] && n < TEXT_CHARS)
        n += sceClibSnprintf(t + n, size - n, "  wait %s %d", top_wait, (top_wait10 + 5) / 10);
    s_text_len[i] = n > TEXT_CHARS ? TEXT_CHARS : n;
    s_text_idx = i;

    window_reset(now);
}

static int sampler_thread(SceSize args, void *argp) {
    s_self = sceKernelGetThreadId();
    int was_on = 0;
    while (s_run) {
        pend_drain();
        if (!s_on) {
            was_on = 0;
            sceKernelDelayThread(100 * 1000);
            continue;
        }
        uint64_t now = sceKernelGetProcessTimeWide();
        if (!was_on) {
            was_on = 1;
            if (!s_pinned) {
                s_pinned = 1;
                pin_self();
            }
            thread_add(s_main);
            s_rec_start = now;
            s_sum.valid = 0;
            window_reset(now);
        }
        sample();
        if (now - s_win_start >= 1000000)
            summary(now);
        sceKernelDelayThread(SAMPLE_US);
    }
    return 0;
}

void tv_flush(void) {
    if (s_on && s_fd < 0) {
        sceIoMkdir(SETTINGS_DIR, 0777);
        s_fd = sceIoOpen(TV_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
        if (s_fd >= 0) {
            static const char hdr[] = "# divasplit 2.0 threads\nt,kind,id,name,detail,pct\n";
            sceIoWrite(s_fd, hdr, sizeof(hdr) - 1);
        }
    }
    if (s_block_len > 0) {
        __sync_synchronize();
        if (s_fd >= 0)
            sceIoWrite(s_fd, s_block, s_block_len);
        s_block_len = 0;
    }
    if (!s_on && s_fd >= 0) {
        sceIoClose(s_fd);
        s_fd = -1;
    }
}

void tv_init(tv_diag_fn diag) {
    s_diag = diag;
    s_thread = sceKernelCreateThread("divasplit_threads", sampler_thread, 64, 0x4000, 0, 0, NULL);
    if (s_thread >= 0)
        sceKernelStartThread(s_thread, 0, NULL);
    else
        s_diag("threadview: sampler thread not created: %08X", s_thread);
}

void tv_shutdown(void) {
    if (s_thread >= 0) {
        s_run = 0;
        sceKernelWaitThreadEnd(s_thread, NULL, NULL);
        sceKernelDeleteThread(s_thread);
        s_thread = -1;
    }
    if (s_fd >= 0) {
        sceIoClose(s_fd);
        s_fd = -1;
    }
}

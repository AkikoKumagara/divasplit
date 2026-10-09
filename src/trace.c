/*
 * Tracer: hooks the game's imports of blocking functions and records every
 * call that took at least TRACE_MIN_US (who, what, how long, called from
 * where). divasplit's own hooks add frame, render stage, vblank and GXM marks.
 *
 * The hooks go in at game start (patching hot imports while the game runs can
 * crash it), only when the profiler kernel plugin is installed: it's a
 * development tool. They record only while recording; otherwise they just pass
 * the call on.
 *
 * Every hooked function takes at most four arguments and returns an int, so
 * one hook shape forwards r0-r3 for all of them.
 *
 * Records go into a ring buffer from any thread; the input thread writes the
 * finished ones out.
 */
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/clib.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <taihen.h>

#include "settings.h"
#include "trace.h"

#define TRACE_FILE    SETTINGS_DIR "/trace.bin"
#define TRACE_MIN_US  20
#define RING_BYTES    (512 * 1024)
#define RING_RECS     (RING_BYTES / sizeof(trace_rec_t))

typedef struct {
    const char *name;
    uint32_t lib, nid;
} traced_t;

static const traced_t TRACED[] = {
    {"sceKernelLockLwMutex",        0xCAE9ACE6, 0x46E7BE7B},
    {"sceKernelWaitLwCond",         0xCAE9ACE6, 0xE1878282},
    {"sceKernelWaitSema",           0xCAE9ACE6, 0x0C7B834B},
    {"sceKernelWaitThreadEnd",      0xCAE9ACE6, 0xDDB395A9},
    {"sceKernelDelayThread",        0x859A24B1, 0x4B675D05},
    {"sceUltMutexLock",             0x220668A5, 0x001EAC8A},
    {"sceUltConditionVariableWait", 0x220668A5, 0x2CD0F57C},
    {"std::mutex::lock",            0xF7629240, 0xF9FAB558},
    {"sceIoOpen",                   0xCAE9ACE6, 0x6C60AC61},
    {"sceIoRead",                   0xF2FF276E, 0xFDB32293},
    {"sceIoClose",                  0xF2FF276E, 0xC70B8886},
    {"sceFiosOpWait",               0x3AD4D630, 0x2AC79DFC},
    {"sceAudioOutOutput",           0x438BB957, 0x02DB3F5F},
    {"sceGxmFinish",                0xF76B66BD, 0x0733D8AE},
};
#define TRACED_COUNT (int)(sizeof(TRACED) / sizeof(TRACED[0]))

static const char *const MARK_NAMES[TR_MARKS] = {
    "frame", "stage", "sceDisplayWaitVblankStartMulti", "sceGxmBeginScene", "sceGxmEndScene",
    "sceGxmDisplayQueueAddEntry"};

static tai_hook_ref_t s_ref[TRACED_COUNT];
static SceUID s_hook[TRACED_COUNT];
static int s_installed;
static volatile int s_on;
static uint64_t s_start;

static SceUID s_mem = -1;
static trace_rec_t *s_ring;
static volatile unsigned int s_head, s_tail, s_dropped;
static SceUID s_fd = -1;

uint64_t trace_now(void) {
    return (uint64_t)sceKernelGetSystemTimeWide();
}

int trace_on(void) {
    return s_on;
}

void trace_add(int func, uint64_t t0, uint64_t t1, uint32_t arg, uint32_t caller) {
    if (!s_on || !s_ring || t0 < s_start)
        return;
    unsigned int i = __sync_fetch_and_add(&s_head, 1);
    if (i - s_tail >= RING_RECS) {
        s_dropped++;
        return;   // the slot stays unwritten; the writer skips it after a while
    }
    trace_rec_t *r = &s_ring[i % RING_RECS];
    r->t_us = (uint32_t)(t0 - s_start);
    r->dur_us = (uint32_t)(t1 - t0);
    r->thid = (uint32_t)sceKernelGetThreadId();
    r->arg = arg;
    r->caller = caller;
    r->func = (uint16_t)func;
    __sync_synchronize();
    r->seq = (uint16_t)(i + 1);
}

// one hook per traced import, all the same shape
#define HOOK(n) \
    static int hook_##n(int a0, int a1, int a2, int a3) { \
        if (!s_on) \
            return TAI_CONTINUE(int, s_ref[n], a0, a1, a2, a3); \
        uint32_t ret_addr = (uint32_t)__builtin_return_address(0); \
        uint64_t t0 = trace_now(); \
        int ret = TAI_CONTINUE(int, s_ref[n], a0, a1, a2, a3); \
        uint64_t t1 = trace_now(); \
        if (t1 - t0 >= TRACE_MIN_US) \
            trace_add(TR_MARKS + n, t0, t1, (uint32_t)a0, ret_addr); \
        return ret; \
    }
HOOK(0) HOOK(1) HOOK(2) HOOK(3) HOOK(4) HOOK(5) HOOK(6) HOOK(7)
HOOK(8) HOOK(9) HOOK(10) HOOK(11) HOOK(12) HOOK(13)
static const void *const HOOKS[] = {
    hook_0, hook_1, hook_2, hook_3, hook_4, hook_5, hook_6, hook_7,
    hook_8, hook_9, hook_10, hook_11, hook_12, hook_13};

int trace_install(void) {
    s_mem = sceKernelAllocMemBlock("divasplit_trace", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, RING_BYTES, NULL);
    if (s_mem < 0)
        return s_mem;
    sceKernelGetMemBlockBase(s_mem, (void **)&s_ring);
    s_installed = 1;
    int n = 0;
    for (int i = 0; i < TRACED_COUNT; i++) {
        // not every game imports every function: those just fail
        s_hook[i] = taiHookFunctionImport(&s_ref[i], TAI_MAIN_MODULE, TRACED[i].lib, TRACED[i].nid, HOOKS[i]);
        n += s_hook[i] >= 0;
    }
    return n;
}

void trace_set(int on) {
    if (on && !s_on && s_ring) {
        s_start = trace_now();
        s_tail = s_head;
        sceClibMemset(s_ring, 0, RING_BYTES);
        s_on = 1;
    } else if (!on) {
        s_on = 0;
    }
}

static void write_header(void) {
    static trace_header_t h;
    sceClibMemset(&h, 0, sizeof(h));
    h.magic = TRACE_MAGIC;
    h.version = TRACE_VERSION;
    h.header_size = sizeof(h);
    h.rec_size = sizeof(trace_rec_t);
    h.start_lo = (uint32_t)s_start;
    h.start_hi = (uint32_t)(s_start >> 32);
    h.nfuncs = TR_MARKS + TRACED_COUNT;
    for (int i = 0; i < TR_MARKS; i++)
        sceClibStrncpy(h.names[i], MARK_NAMES[i], TRACE_NAME_LEN - 1);
    for (int i = 0; i < TRACED_COUNT; i++)
        sceClibStrncpy(h.names[TR_MARKS + i], TRACED[i].name, TRACE_NAME_LEN - 1);
    sceIoWrite(s_fd, &h, sizeof(h));
}

void trace_flush(void) {
    if (!s_ring)
        return;
    if (s_on && s_fd < 0) {
        sceIoMkdir(SETTINGS_DIR, 0777);
        s_fd = sceIoOpen(TRACE_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
        if (s_fd >= 0)
            write_header();
    }
    // write the finished records in order; a slot still being written (or
    // dropped while the ring was full) ends this round
    static unsigned int waited = 0;   // rounds the oldest slot stayed unfinished
    unsigned int head = s_head;
    while (s_tail != head) {
        unsigned int start = s_tail, n = 0;
        while (s_tail + n != head && n < RING_RECS - (s_tail % RING_RECS)) {
            trace_rec_t *r = &s_ring[(s_tail + n) % RING_RECS];
            if (r->seq != (uint16_t)(s_tail + n + 1))
                break;
            n++;
        }
        if (n) {
            if (s_fd >= 0)
                sceIoWrite(s_fd, &s_ring[start % RING_RECS], n * sizeof(trace_rec_t));
            for (unsigned int k = 0; k < n; k++)
                s_ring[(start + k) % RING_RECS].seq = 0;
            s_tail += n;
            waited = 0;
            continue;
        }
        // not finished yet: skip it if it stays that way (a dropped record)
        if (++waited > 3) {
            s_tail++;
            waited = 0;
        } else {
            break;
        }
    }
    if (!s_on && s_fd >= 0 && s_tail == s_head) {
        sceIoClose(s_fd);
        s_fd = -1;
    }
}

void trace_shutdown(void) {
    s_on = 0;
    for (int i = 0; s_installed && i < TRACED_COUNT; i++)
        if (s_hook[i] >= 0)
            taiHookRelease(s_hook[i], s_ref[i]);
    if (s_fd >= 0)
        sceIoClose(s_fd);
}

/*
 * divasplit_prof - sampling profiler for divasplit (kernel plugin).
 *
 * Loaded at boot from *KERNEL, idle until divasplit.suprx calls
 * divasplit_prof_start() (when recording starts). A kernel thread then, at
 * rate_hz:
 *   1. pauses the game process (ksceKernelSuspendProcess, as kvdb does)
 *   2. reads the registers of up to 8 threads (the main thread and the workers
 *      divasplit picked), the divasplit frame counter and render stage, and
 *      scans 2 KB of each stack for return addresses into the eboot
 *   3. resumes the process
 * and appends the samples to ux0:data/divasplit/prof.bin. tools/profview.py
 * turns them into a per-function profile.
 *
 * module_start only looks up two functions, so a problem here can't stop the
 * console from booting. The sampler stops by itself if the game exits.
 */
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/threadmgr/debugger.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/sysmem/data_transfers.h>
#include <psp2kern/kernel/sysmem/uid_puid.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <stdarg.h>

#include "../../src/prof.h"

#define KERNEL_PID   0x10005
#define BUF_SAMPLES  128
#define SCAN_BYTES   2048
#define SUSPEND_REASON 0x1C   // what kvdb passes
#define MAX_FAILS    200      // in a row: the game is gone

#define ERR_NOT_READY  0x80DA0001
#define ERR_BUSY       0x80DA0002
#define ERR_ARGS       0x80DA0003
#define ERR_THREAD     0x80DA0004

int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid, uint32_t funcnid, uintptr_t *func);

static int (*_suspend_process)(SceUID pid, int status);
static int (*_resume_process)(SceUID pid);

static prof_args_t g_args;
static SceUID g_pid, g_threads[PROF_MAX_THREADS];   // game process, sampled threads (kernel ids)
static SceUID g_thread = -1;
static volatile int g_run = 0;
static volatile int g_running = 0;   // the sampler thread is alive
static volatile int g_samples = 0;

static prof_sample_t g_buf[BUF_SAMPLES];
static uint32_t g_scan[SCAN_BYTES / 4];

// stats for prof.txt
static unsigned int g_suspend_fail, g_regs_fail, g_scan_fail, g_user, g_cost_max;
static uint64_t g_cost_total;
static prof_sample_t g_first[4];
static unsigned int g_pauses;
// last written state of each thread: a waiting thread is written once
static uint32_t g_last_pc[PROF_MAX_THREADS], g_last_lr[PROF_MAX_THREADS];
static uint8_t g_last_flags[PROF_MAX_THREADS];
static prof_args_t g_new_args;

static void write_stats(uint64_t duration_us) {
    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
            "samples %d (%u pauses, %u threads) in %u ms (rate %u Hz)\n"
            "suspend fails %u, register fails %u, stack scan fails %u\n"
            "user mode %u\n"
            "pause per sample: avg %u us, max %u us\n",
            g_samples, g_pauses, g_args.nthreads, (unsigned)(duration_us / 1000), g_args.rate_hz,
            g_suspend_fail, g_regs_fail, g_scan_fail, g_user,
            g_pauses ? (unsigned)(g_cost_total / g_pauses) : 0, g_cost_max);
    for (int i = 0; i < 4 && i < g_samples && n < (int)sizeof(buf) - 160; i++) {
        prof_sample_t *s = &g_first[i];
        n += snprintf(buf + n, sizeof(buf) - n,
                "first %d: thread %u flags %X pc %08X lr %08X sp %08X cpsr %08X kpc %08X kcpsr %08X stage %d n %u\n",
                i, s->thread, s->flags, s->pc, s->lr, s->sp, s->cpsr, s->kpc, s->kcpsr, s->stage, s->nstack);
    }
    SceUID fd = ksceIoOpen(PROF_STATS_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) {
        ksceIoWrite(fd, buf, n);
        ksceIoClose(fd);
    }
}

static int in_text(uint32_t addr) {
    return addr - g_args.text_base < g_args.text_size;
}

// while the game is paused: registers and return addresses on the stack.
// Returns 1 when the sample is worth writing.
static int take(prof_sample_t *s, int t, SceUID thread, uint32_t stack_top) {
    SceThreadCpuRegisters regs;
    memset(&regs, 0, sizeof(regs));
    if (ksceKernelGetThreadCpuRegisters(thread, &regs) < 0) {
        g_regs_fail++;
        return 0;
    }
    // set 0 is the current context. Inside a system call it is the kernel's and
    // set 1 holds the user mode registers saved on entry.
    SceArmCpuRegisters *u, *cur = &regs.entry[0], *saved = &regs.entry[1];
    s->flags |= PROF_F_REGS;
    if ((cur->cpsr & 0x1F) == 0x10) {
        u = cur;
        s->flags |= PROF_F_USER;
    } else {
        s->kpc = cur->pc;
        s->kcpsr = cur->cpsr;
        if ((saved->cpsr & 0x1F) != 0x10)
            return 0;
        u = saved;
        s->flags |= PROF_F_SYSCALL;
    }
    s->pc = u->pc;
    s->lr = u->lr;
    s->sp = u->sp;
    s->cpsr = u->cpsr;

    // still waiting where it was: nothing new
    int user = s->flags & PROF_F_USER;
    if (!user && g_last_flags[t] == s->flags && g_last_pc[t] == s->pc && g_last_lr[t] == s->lr)
        return 0;
    g_last_flags[t] = s->flags;
    g_last_pc[t] = s->pc;
    g_last_lr[t] = s->lr;

    // Thumb return addresses (bit 0 set) into the eboot; profview checks
    // that each one follows a call instruction
    uint32_t sp = u->sp;
    if (sp < 0x80000000 || sp >= 0xE0000000)
        return 1;
    // never read past the end of the stack: a failed copy is slow. A wait
    // only needs the caller of the system call.
    int len = user ? SCAN_BYTES : SCAN_BYTES / 4;
    if (stack_top) {
        if (sp >= stack_top)
            return 1;
        if (stack_top - sp < (uint32_t)len)
            len = (int)(stack_top - sp) & ~3;
    }
    if (len < 4 || ksceKernelCopyFromUserProc(g_pid, g_scan, (const void *)sp, len) < 0) {
        g_scan_fail++;
        return 1;
    }
    s->flags |= PROF_F_STACK;
    for (int i = 0; i < len / 4 && s->nstack < PROF_STACK_WORDS; i++)
        if ((g_scan[i] & 1) && in_text(g_scan[i] - 1))
            s->stack[s->nstack++] = g_scan[i];
    return 1;
}

static int sampler(SceSize argc, void *argp) {
    unsigned int period = 1000000 / (g_args.rate_hz ? g_args.rate_hz : 500);
    int fails = 0, nbuf = 0;
    ksceIoMkdir("ux0:data/divasplit", 0777);
    SceUID fd = ksceIoOpen(PROF_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    static prof_header_t h;
    memset(&h, 0, sizeof(h));
    if (fd >= 0) {
        h.magic = PROF_MAGIC;
        h.version = PROF_VERSION;
        h.header_size = sizeof(h);
        h.sample_size = sizeof(prof_sample_t);
        h.rate_hz = g_args.rate_hz;
        h.module_nid = g_args.module_nid;
        h.text_base = g_args.text_base;
        h.text_size = g_args.text_size;
        h.stack_words = PROF_STACK_WORDS;
        h.nthreads = g_args.nthreads;
        memcpy(h.names, g_args.names, sizeof(h.names));
        memcpy(h.thids, g_args.thids, sizeof(h.thids));
    }
    uint64_t start = ksceKernelGetSystemTimeWide();
    if (fd >= 0) {
        h.start_lo = (uint32_t)start;
        h.start_hi = (uint32_t)(start >> 32);
        ksceIoWrite(fd, &h, sizeof(h));
    }
    while (g_run) {
        ksceKernelDelayThread(period);
        if (nbuf + g_args.nthreads > BUF_SAMPLES) {
            if (fd >= 0)
                ksceIoWrite(fd, g_buf, nbuf * sizeof(prof_sample_t));
            nbuf = 0;
        }
        prof_sample_t *first = NULL;
        uint64_t t0 = ksceKernelGetSystemTimeWide();
        if (_suspend_process(g_pid, SUSPEND_REASON) < 0) {
            g_suspend_fail++;
            if (++fails > MAX_FAILS)
                break;
            continue;
        }
        int32_t frame = 0, stage = -1;
        ksceKernelCopyFromUserProc(g_pid, &frame, (const void *)g_args.frame_addr, 4);
        ksceKernelCopyFromUserProc(g_pid, &stage, (const void *)g_args.stage_addr, 4);
        int written = 0;
        for (unsigned int t = 0; t < g_args.nthreads; t++) {
            prof_sample_t *s = &g_buf[nbuf + written];
            memset(s, 0, sizeof(*s));
            s->t_us = (uint32_t)(t0 - start);
            s->frame = (uint32_t)frame;
            s->stage = (int8_t)stage;
            s->thread = (uint8_t)t;
            if (take(s, t, g_threads[t], g_args.stack_top[t])) {
                if (!first)
                    first = s;
                written++;
            }
        }
        _resume_process(g_pid);
        fails = 0;
        uint32_t cost = (uint32_t)(ksceKernelGetSystemTimeWide() - t0);
        if (first)
            first->cost_us = cost;
        g_cost_total += cost;
        if (cost > g_cost_max)
            g_cost_max = cost;
        if (first && first->thread == 0 && (first->flags & PROF_F_USER))
            g_user++;
        if (first && g_pauses < 4)
            g_first[g_pauses] = *first;
        g_pauses++;
        g_samples += written;
        nbuf += written;
    }
    if (fd >= 0) {
        if (nbuf)
            ksceIoWrite(fd, g_buf, nbuf * sizeof(prof_sample_t));
        ksceIoClose(fd);
    }
    write_stats(ksceKernelGetSystemTimeWide() - start);
    g_running = 0;
    return 0;
}

static void reap(void) {
    if (g_thread >= 0) {
        SceUInt timeout = 2000000;
        g_run = 0;
        ksceKernelWaitThreadEnd(g_thread, NULL, &timeout);
        ksceKernelDeleteThread(g_thread);
        g_thread = -1;
    }
}

static int start(const prof_args_t *uargs) {
    if (!_suspend_process || !_resume_process)
        return ERR_NOT_READY;
    if (g_running)
        return ERR_BUSY;
    reap();   // a sampler that stopped on its own (game exited)
    // the arguments are too big for the system call's stack
    prof_args_t *a = &g_new_args;
    if (ksceKernelMemcpyUserToKernel(a, uargs, sizeof(*a)) < 0)
        return ERR_ARGS;
    if (a->magic != PROF_MAGIC || a->version != PROF_VERSION || a->size != sizeof(*a)
            || a->text_size == 0 || a->rate_hz == 0 || a->rate_hz > 2000
            || a->nthreads == 0 || a->nthreads > PROF_MAX_THREADS)
        return ERR_ARGS;
    SceUID pid = ksceKernelGetProcessId();
    for (unsigned int t = 0; t < a->nthreads; t++) {
        g_threads[t] = kscePUIDtoGUID(pid, a->thids[t]);
        if (g_threads[t] < 0)
            return g_threads[t];
        a->names[t][PROF_NAME_LEN - 1] = 0;
    }
    g_args = *a;
    g_pid = pid;
    memset(g_last_flags, 0xFF, sizeof(g_last_flags));
    g_samples = 0;
    g_pauses = 0;
    g_suspend_fail = g_regs_fail = g_scan_fail = g_user = g_cost_max = 0;
    g_cost_total = 0;
    g_run = 1;
    g_running = 1;
    // the system core keeps the sampler off the game's cores; any core if refused
    g_thread = ksceKernelCreateThread("divasplit_prof", sampler, 64, 0x2000, 0, 0x80000, NULL);
    if (g_thread < 0)
        g_thread = ksceKernelCreateThread("divasplit_prof", sampler, 64, 0x2000, 0, 0, NULL);
    if (g_thread < 0) {
        g_running = 0;
        return ERR_THREAD;
    }
    ksceKernelStartThread(g_thread, 0, NULL);
    return 0;
}

int divasplit_prof_start(const prof_args_t *args) {
    int state;
    ENTER_SYSCALL(state);
    int ret = start(args);
    EXIT_SYSCALL(state);
    return ret;
}

int divasplit_prof_stop(void) {
    int state;
    ENTER_SYSCALL(state);
    reap();
    int ret = g_samples;
    EXIT_SYSCALL(state);
    return ret;
}

void _start() __attribute__ ((weak, alias("module_start")));
int module_start(SceSize argc, const void *args) {
    // 3.60, then 3.63+
    if (module_get_export_func(KERNEL_PID, "SceProcessmgr", 0x7A69DE86, 0x6AECE4CD, (uintptr_t *)&_suspend_process) < 0)
        module_get_export_func(KERNEL_PID, "SceProcessmgr", 0xEB1F8EF7, 0x93B8E785, (uintptr_t *)&_suspend_process);
    if (module_get_export_func(KERNEL_PID, "SceProcessmgr", 0x7A69DE86, 0x080CDC59, (uintptr_t *)&_resume_process) < 0)
        module_get_export_func(KERNEL_PID, "SceProcessmgr", 0xEB1F8EF7, 0x6894499C, (uintptr_t *)&_resume_process);
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
    reap();
    return SCE_KERNEL_STOP_SUCCESS;
}

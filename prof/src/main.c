/*
 * divasplit_prof - sampling profiler for divasplit (kernel plugin).
 *
 * Loaded at boot from *KERNEL, idle until divasplit.suprx calls
 * divasplit_prof_start() (when recording starts). A kernel thread then, at
 * rate_hz:
 *   1. pauses the game process (ksceKernelSuspendProcess, as kvdb does)
 *   2. reads the main thread's registers, the divasplit frame counter and
 *      render stage, and scans 2 KB of its stack for return addresses into the
 *      eboot
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
static SceUID g_pid, g_main;   // game process, main thread (kernel id)
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

static void write_stats(uint64_t duration_us) {
    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
            "samples %d in %u ms (rate %u Hz)\n"
            "suspend fails %u, register fails %u, stack scan fails %u\n"
            "user mode %u\n"
            "pause per sample: avg %u us, max %u us\n",
            g_samples, (unsigned)(duration_us / 1000), g_args.rate_hz,
            g_suspend_fail, g_regs_fail, g_scan_fail, g_user,
            g_samples ? (unsigned)(g_cost_total / g_samples) : 0, g_cost_max);
    for (int i = 0; i < 4 && i < g_samples && n < (int)sizeof(buf) - 160; i++) {
        prof_sample_t *s = &g_first[i];
        n += snprintf(buf + n, sizeof(buf) - n,
                "first %d: flags %X pc %08X lr %08X sp %08X cpsr %08X kpc %08X kcpsr %08X stage %d n %u\n",
                i, s->flags, s->pc, s->lr, s->sp, s->cpsr, s->kpc, s->kcpsr, s->stage, s->nstack);
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

// while the game is paused: registers, counters, return addresses on the stack
static void take(prof_sample_t *s) {
    SceThreadCpuRegisters regs;
    memset(&regs, 0, sizeof(regs));
    if (ksceKernelGetThreadCpuRegisters(g_main, &regs) < 0) {
        g_regs_fail++;
        return;
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
            return;
        u = saved;
        s->flags |= PROF_F_SYSCALL;
    }
    s->pc = u->pc;
    s->lr = u->lr;
    s->sp = u->sp;
    s->cpsr = u->cpsr;

    int32_t v;
    if (ksceKernelCopyFromUserProc(g_pid, &v, (const void *)g_args.frame_addr, 4) >= 0)
        s->frame = (uint32_t)v;
    if (ksceKernelCopyFromUserProc(g_pid, &v, (const void *)g_args.stage_addr, 4) >= 0)
        s->stage = (int16_t)v;

    // Thumb return addresses (bit 0 set) into the eboot; profview checks
    // that each one follows a call instruction
    uint32_t sp = u->sp;
    if (sp < 0x80000000 || sp >= 0xE0000000)
        return;
    int len = SCAN_BYTES;
    while (len >= 256 && ksceKernelCopyFromUserProc(g_pid, g_scan, (const void *)sp, len) < 0)
        len /= 2;
    if (len < 256) {
        g_scan_fail++;
        return;
    }
    s->flags |= PROF_F_STACK;
    for (int i = 0; i < len / 4 && s->nstack < PROF_STACK_WORDS; i++)
        if ((g_scan[i] & 1) && in_text(g_scan[i] - 1))
            s->stack[s->nstack++] = g_scan[i];
}

static int sampler(SceSize argc, void *argp) {
    unsigned int period = 1000000 / (g_args.rate_hz ? g_args.rate_hz : 500);
    int fails = 0, nbuf = 0;
    ksceIoMkdir("ux0:data/divasplit", 0777);
    SceUID fd = ksceIoOpen(PROF_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) {
        prof_header_t h;
        memset(&h, 0, sizeof(h));
        h.magic = PROF_MAGIC;
        h.version = PROF_VERSION;
        h.header_size = sizeof(h);
        h.sample_size = sizeof(prof_sample_t);
        h.rate_hz = g_args.rate_hz;
        h.module_nid = g_args.module_nid;
        h.text_base = g_args.text_base;
        h.text_size = g_args.text_size;
        h.stack_words = PROF_STACK_WORDS;
        ksceIoWrite(fd, &h, sizeof(h));
    }
    uint64_t start = ksceKernelGetSystemTimeWide();
    while (g_run) {
        ksceKernelDelayThread(period);
        prof_sample_t *s = &g_buf[nbuf];
        memset(s, 0, sizeof(*s));
        s->stage = -1;
        uint64_t t0 = ksceKernelGetSystemTimeWide();
        s->t_us = (uint32_t)(t0 - start);
        if (_suspend_process(g_pid, SUSPEND_REASON) < 0) {
            g_suspend_fail++;
            if (++fails > MAX_FAILS)
                break;
            continue;
        }
        take(s);
        _resume_process(g_pid);
        fails = 0;
        uint32_t cost = (uint32_t)(ksceKernelGetSystemTimeWide() - t0);
        s->cost_us = cost;
        g_cost_total += cost;
        if (cost > g_cost_max)
            g_cost_max = cost;
        if (s->flags & PROF_F_USER)
            g_user++;
        if (g_samples < 4)
            g_first[g_samples] = *s;
        g_samples++;
        if (++nbuf == BUF_SAMPLES) {
            if (fd >= 0)
                ksceIoWrite(fd, g_buf, sizeof(g_buf));
            nbuf = 0;
        }
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
    prof_args_t a;
    if (ksceKernelMemcpyUserToKernel(&a, uargs, sizeof(a)) < 0)
        return ERR_ARGS;
    if (a.magic != PROF_MAGIC || a.version != PROF_VERSION || a.size != sizeof(a)
            || a.text_size == 0 || a.rate_hz == 0 || a.rate_hz > 2000)
        return ERR_ARGS;
    SceUID pid = ksceKernelGetProcessId();
    SceUID main = kscePUIDtoGUID(pid, a.main_thid);
    if (main < 0)
        return main;
    g_args = a;
    g_pid = pid;
    g_main = main;
    g_samples = 0;
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

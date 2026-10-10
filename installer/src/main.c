/*
 * divasplit installer: installs or updates divasplit and the DIVA 60 fps
 * VitaGrafix patches, then reboots.
 *
 *   - ur0:tai/divasplit.suprx
 *   - taiHEN config.txt: the plugin under every supported title (ux0:tai/config.txt
 *     when it exists, since taiHEN prefers it, else ur0:tai/config.txt)
 *   - Quick Menu plugin, when QuickMenuReborn is installed
 *   - VitaGrafix patchlist.txt blocks and FPS=60 in its config.txt, when
 *     VitaGrafix is installed
 *   - optional (SQUARE): the profiler kernel plugin, ur0:tai/divasplit_prof.skprx
 *     under *KERNEL
 *   - the plugin's shaders, compiled with the system's shader compiler when
 *     ur0:data/libshacccg.suprx is there (extracted with ShaRKF00D), into
 *     ux0:data/divasplit/shaders/<name>.gxp; on errors <name>.log says why.
 *     Optional: the plugin has the same shaders built in.
 *
 * Files come from the VPK (app0:payload). Files in ux0:data/divasplit/install/
 * (same names) take priority and are deleted after a successful install, so a
 * test build can be dropped there over USB. Such a test build is remembered
 * (ux0:data/divasplit/testbuild_<name>.txt, a checksum) and never replaced by
 * the older copy bundled in the VPK; only a newer test build or a reinstall
 * after uninstalling replaces it.
 *
 * Every edited file is backed up to <file>.bak first and replaced through a
 * temporary file.
 */
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/shacccg.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/display.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/power.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font_ter_u14b.h"
#include "edit.h"

#define VERSION "2.0"

#define SCREEN_W 960
#define SCREEN_H 544
#define FONT_W 8
#define FONT_H 14
#define LINE_H 16
#define COLS (SCREEN_W / FONT_W - 4)
#define ROWS 33

#define PAYLOAD_DIR  "app0:payload"
#define STAGE_DIR    "ux0:data/divasplit/install"
#define PLUGIN_NAME  "divasplit.suprx"
#define QM_NAME      "divasplit_qm.suprx"
#define PROF_NAME    "divasplit_prof.skprx"
#define PROF_DST     "ur0:tai/" PROF_NAME
#define PROF_LINE    "ur0:tai/" PROF_NAME
#define PLUGIN_DST   "ur0:tai/" PLUGIN_NAME
#define PLUGIN_LINE  "ur0:tai/" PLUGIN_NAME
#define TAI_UX       "ux0:tai/config.txt"
#define TAI_UR       "ur0:tai/config.txt"
#define QMR_UX       "ux0:QuickMenuReborn"
#define QMR_UR       "ur0:QuickMenuReborn"
#define QMR_RCO      QMR_UR "/qmr_plugin.rco"
#define VG_PATCHLIST "ux0:data/VitaGrafix/patchlist.txt"
#define VG_CONFIG    "ux0:data/VitaGrafix/config.txt"
#define MAX_BLOCKS   16
#define SHACC_PATH   "ur0:data/libshacccg.suprx"
#define SHADER_SRC   PAYLOAD_DIR "/shaders"
#define SHADER_DIR   "ux0:data/divasplit/shaders"

#define C_BG     0xFF201812
#define C_TEXT   0xFFE0E0E0
#define C_DIM    0xFF909090
#define C_TITLE  0xFFBBC539   // Miku teal
#define C_OK     0xFF66DD66
#define C_WARN   0xFF33CCFF
#define C_ERR    0xFF5555FF

/* screen */

static uint32_t *g_fb;
static struct {
    uint32_t color;
    char text[COLS + 1];
} g_lines[ROWS];
static int g_nlines;

static void fb_init(void) {
    SceUID mem = sceKernelAllocMemBlock("display", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
            (SCREEN_W * SCREEN_H * 4 + 0x3FFFF) & ~0x3FFFF, NULL);
    sceKernelGetMemBlockBase(mem, (void **)&g_fb);
    SceDisplayFrameBuf fb = {sizeof(fb), g_fb, SCREEN_W, SCE_DISPLAY_PIXELFORMAT_A8B8G8R8, SCREEN_W, SCREEN_H};
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

static void draw_string(int x0, int y0, const char *s, uint32_t color) {
    for (int c = 0; s[c] && x0 + (c + 1) * FONT_W <= SCREEN_W; c++) {
        for (int y = 0; y < FONT_H; y++) {
            unsigned char bits = FONT_TER_U14B[(unsigned char)s[c] * FONT_H + y];
            uint32_t *row = g_fb + (y0 + y) * SCREEN_W + x0 + c * FONT_W;
            for (int x = 0; x < FONT_W; x++)
                row[x] = ((bits >> (7 - x)) & 1) ? color : C_BG;
        }
    }
}

static void render(void) {
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
        g_fb[i] = C_BG;
    for (int i = 0; i < g_nlines; i++)
        draw_string(16, 12 + i * LINE_H, g_lines[i].text, g_lines[i].color);
}

static void clear(void) {
    g_nlines = 0;
}

static void say(uint32_t color, const char *fmt, ...) {
    if (g_nlines == ROWS) {   // scroll
        memmove(&g_lines[0], &g_lines[1], sizeof(g_lines[0]) * (ROWS - 1));
        g_nlines--;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_lines[g_nlines].text, COLS + 1, fmt, ap);
    va_end(ap);
    g_lines[g_nlines].color = color;
    g_nlines++;
    render();
}

/* files */

static int exists(const char *path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

static char *read_file(const char *path, size_t *len) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0)
        return NULL;
    SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    char *buf = size >= 0 ? malloc((size_t)size + 1) : NULL;
    if (buf && sceIoRead(fd, buf, (SceSize)size) != (int)size) {
        free(buf);
        buf = NULL;
    }
    sceIoClose(fd);
    if (buf) {
        buf[size] = 0;
        if (len)
            *len = (size_t)size;
    }
    return buf;
}

static int write_raw(const char *path, const void *data, size_t len) {
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0)
        return fd;
    int w = sceIoWrite(fd, data, len);
    sceIoClose(fd);
    return w == (int)len ? 0 : -1;
}

// backup to path.bak, write path.tmp, then swap it in
static int write_file(const char *path, const void *data, size_t len, int backup) {
    char tmp[256], bak[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    snprintf(bak, sizeof(bak), "%s.bak", path);
    if (backup && exists(path)) {
        size_t old_len;
        char *old = read_file(path, &old_len);
        if (!old || write_raw(bak, old, old_len) < 0) {
            free(old);
            return -1;
        }
        free(old);
    }
    if (write_raw(tmp, data, len) < 0) {
        sceIoRemove(tmp);
        return -1;
    }
    size_t check_len = 0;
    char *check = read_file(tmp, &check_len);
    int ok = check && check_len == len && memcmp(check, data, len) == 0;
    free(check);
    if (!ok) {
        sceIoRemove(tmp);
        return -1;
    }
    sceIoRemove(path);
    return sceIoRename(tmp, path) < 0 ? -1 : 0;
}

static uint32_t checksum(const char *data, size_t len) {
    uint32_t h = 0x811C9DC5;
    for (size_t i = 0; i < len; i++)
        h = (h ^ (unsigned char)data[i]) * 0x01000193;
    return h;
}

static void testbuild_path(char *out, size_t size, const char *name) {
    snprintf(out, size, "ux0:data/divasplit/testbuild_%s.txt", name);
}

// dst holds the test build installed from the install folder
static int is_test_build(const char *dst, const char *name) {
    char path[128];
    testbuild_path(path, sizeof(path), name);
    char *mark = read_file(path, NULL);
    size_t len;
    char *cur = read_file(dst, &len);
    int yes = mark && cur && strtoul(mark, NULL, 16) == checksum(cur, len);
    free(mark);
    free(cur);
    return yes;
}

static int same_file(const char *path, const char *data, size_t len) {
    size_t cur_len;
    char *cur = read_file(path, &cur_len);
    int same = cur && cur_len == len && memcmp(cur, data, len) == 0;
    free(cur);
    return same;
}

/* payload */

typedef struct {
    char name[64];       // path inside the payload, e.g. vitagrafix/divax.txt
    char *data;
    size_t len;
    int staged;          // from ux0:data/divasplit/install
} file_t;

static file_t g_plugin, g_qm, g_prof;
static file_t g_blocks[MAX_BLOCKS];
static int g_nblocks;
static char g_titles[MAX_TITLES][TITLE_LEN + 1];
static int g_ntitles;

static int load(file_t *f, const char *name) {
    char path[256];
    free(f->data);
    memset(f, 0, sizeof(*f));
    snprintf(f->name, sizeof(f->name), "%s", name);
    snprintf(path, sizeof(path), STAGE_DIR "/%s", name);
    f->data = read_file(path, &f->len);
    if (f->data) {
        f->staged = 1;
        return 0;
    }
    snprintf(path, sizeof(path), PAYLOAD_DIR "/%s", name);
    f->data = read_file(path, &f->len);
    return f->data ? 0 : -1;
}

static void add_block_names(const char *dir, char names[][64], int *n) {
    SceUID d = sceIoDopen(dir);
    if (d < 0)
        return;
    SceIoDirent e;
    while (*n < MAX_BLOCKS && sceIoDread(d, &e) > 0) {
        size_t l = strlen(e.d_name);
        // "._name" files are macOS metadata left by copying over USB
        if (l < 5 || strcmp(e.d_name + l - 4, ".txt") != 0 || e.d_name[0] == '.')
            continue;
        int dup = 0;
        for (int i = 0; i < *n; i++)
            dup |= strcmp(names[i] + 11, e.d_name) == 0;   // skip "vitagrafix/"
        if (!dup)
            snprintf(names[(*n)++], 64, "vitagrafix/%s", e.d_name);
    }
    sceIoDclose(d);
}

static int load_payload(void) {
    int ok = load(&g_plugin, PLUGIN_NAME) == 0;
    load(&g_qm, QM_NAME);
    load(&g_prof, PROF_NAME);
    char names[MAX_BLOCKS][64];
    int n = 0;
    add_block_names(STAGE_DIR "/vitagrafix", names, &n);
    add_block_names(PAYLOAD_DIR "/vitagrafix", names, &n);
    for (int i = 0; i < g_nblocks; i++)
        free(g_blocks[i].data);
    g_nblocks = 0;
    g_ntitles = 0;
    for (int i = 0; i < n; i++) {
        if (load(&g_blocks[g_nblocks], names[i]) < 0)
            continue;
        char t[MAX_TITLES][TITLE_LEN + 1];
        int nt = block_titles(g_blocks[g_nblocks].data, t, MAX_TITLES);
        for (int j = 0; j < nt && g_ntitles < MAX_TITLES; j++) {
            int dup = 0;
            for (int k = 0; k < g_ntitles; k++)
                dup |= strcmp(g_titles[k], t[j]) == 0;
            if (!dup)
                strcpy(g_titles[g_ntitles++], t[j]);
        }
        g_nblocks++;
    }
    return ok && g_ntitles > 0;
}

static void remove_staged(void) {
    char path[256];
    file_t *all[3 + MAX_BLOCKS] = {&g_plugin, &g_qm, &g_prof};
    for (int i = 0; i < g_nblocks; i++)
        all[3 + i] = &g_blocks[i];
    for (int i = 0; i < 3 + g_nblocks; i++) {
        if (all[i]->staged) {
            snprintf(path, sizeof(path), STAGE_DIR "/%s", all[i]->name);
            sceIoRemove(path);
            all[i]->staged = 0;   // installed now; the copy in memory stays the reference
        }
    }
}

/* state of the console */

typedef struct {
    const char *tai;          // config.txt taiHEN uses
    int plugin;               // 0 missing, 1 different, 2 same, 3 test build (kept)
    int tai_ok;
    const char *qmr_dir;      // NULL: QuickMenuReborn not installed
    int qm;
    int prof;                 // 0 missing, 1 different, 2 same
    int prof_line;            // listed under *KERNEL
    int qmr_rco;
    int vg_plugin;            // VitaGrafix listed in the taiHEN config
    int vg_patch_ok, vg_config_ok;
    int vg_files;
} state_t;

static int patchlist_edit(const char *in, char **out) {
    char *cur = NULL;
    *out = NULL;
    for (int i = 0; i < g_nblocks; i++) {
        char *next;
        if (vg_patch_replace(cur ? cur : in, g_blocks[i].data, &next) < 0) {
            free(cur);
            return -1;
        }
        if (next) {
            free(cur);
            cur = next;
        }
    }
    *out = cur;
    return 0;
}

// remember a test build, or forget it when the bundled copy goes in
static void note_install(const file_t *f) {
    char path[128], text[16];
    testbuild_path(path, sizeof(path), f->name);
    if (f->staged) {
        snprintf(text, sizeof(text), "%08X\n", (unsigned)checksum(f->data, f->len));
        write_raw(path, text, strlen(text));
    } else {
        sceIoRemove(path);
    }
}

// 0 missing, 1 different, 2 same, 3 a test build that the bundled copy must not replace
static int file_state(const char *dst, const file_t *f) {
    if (!exists(dst))
        return 0;
    if (same_file(dst, f->data, f->len))
        return 2;
    if (!f->staged && is_test_build(dst, f->name))
        return 3;
    return 1;
}

static void check(state_t *s) {
    memset(s, 0, sizeof(*s));
    s->tai = exists(TAI_UX) ? TAI_UX : TAI_UR;
    s->plugin = file_state(PLUGIN_DST, &g_plugin);

    char *tai = read_file(s->tai, NULL);
    if (tai) {
        char *out = NULL;
        s->tai_ok = tai_install(tai, g_titles, g_ntitles, PLUGIN_LINE, &out) == 0 && !out;
        free(out);
        s->vg_plugin = find_ci(tai, "vitagrafix.suprx") != NULL;
        s->prof_line = tai_kernel_has(tai, PROF_NAME);
        free(tai);
    }

    s->qmr_dir = exists(QMR_UX) ? QMR_UX : exists(QMR_UR) ? QMR_UR : NULL;
    if (s->qmr_dir && g_qm.data) {
        char path[256];
        snprintf(path, sizeof(path), "%s/" QM_NAME, s->qmr_dir);
        s->qm = file_state(path, &g_qm);
    }
    s->qmr_rco = exists(QMR_RCO);
    if (g_prof.data)
        s->prof = file_state(PROF_DST, &g_prof);

    char *pl = read_file(VG_PATCHLIST, NULL);
    char *cfg = read_file(VG_CONFIG, NULL);
    s->vg_files = pl != NULL;
    if (pl) {
        char *out = NULL;
        s->vg_patch_ok = patchlist_edit(pl, &out) == 0 && !out;
        free(out);
    }
    if (cfg) {
        char *out = NULL;
        s->vg_config_ok = vg_config_fps(cfg, g_titles, g_ntitles, &out) == 0 && !out;
        free(out);
    } else {
        s->vg_config_ok = 0;
    }
    free(pl);
    free(cfg);
}

static void show_status(const state_t *s) {
    clear();
    say(C_TITLE, "divasplit installer %s", VERSION);
    say(C_DIM, "Frame pacing plugin + 60 fps patches for Project DIVA X, f and f 2nd");
    say(C_TEXT, "");
    char titles[COLS + 1] = "";
    for (int i = 0; i < g_ntitles; i++) {
        strncat(titles, " ", sizeof(titles) - strlen(titles) - 1);
        strncat(titles, g_titles[i], sizeof(titles) - strlen(titles) - 1);
    }
    say(C_TEXT, "Games:%s", titles);
    say(C_TEXT, "");
    static const char *const plugin_state[] = {"not installed", "different version", "up to date",
            "test build (kept)"};
    say(s->plugin >= 2 ? C_OK : C_WARN, "  Plugin        %s  %s%s", PLUGIN_DST, plugin_state[s->plugin],
            g_plugin.staged ? "  (new build in install folder)" : "");
    say(s->tai_ok ? C_OK : C_WARN, "  taiHEN config %s  %s", s->tai,
            s->tai_ok ? "ok" : "needs the plugin lines");
    if (s->qmr_dir)
        say(s->qm >= 2 ? C_OK : C_WARN, "  Quick Menu    %s/%s  %s", s->qmr_dir, QM_NAME, plugin_state[s->qm]);
    else
        say(C_DIM, "  Quick Menu    QuickMenuReborn not installed (optional, skipped)");
    if (s->qmr_dir && !s->qmr_rco)
        say(C_WARN, "                %s missing: the Quick Menu won't show", QMR_RCO);
    if (s->vg_files) {
        say(s->vg_patch_ok ? C_OK : C_WARN, "  VitaGrafix    patchlist.txt  %s",
                s->vg_patch_ok ? "up to date" : "DIVA blocks need updating");
        say(s->vg_config_ok ? C_OK : C_WARN, "                config.txt     %s",
                s->vg_config_ok ? "FPS=60 set" : "FPS=60 needs setting");
        if (!s->vg_plugin)
            say(C_WARN, "                VitaGrafix.suprx isn't in the taiHEN config: 60 fps won't apply");
    } else {
        say(C_DIM, "  VitaGrafix    not found (%s): 60 fps patches skipped", VG_PATCHLIST);
    }
    if (g_prof.data) {
        if (s->prof_line)
            say(s->prof >= 2 ? C_OK : C_WARN, "  Profiler      %s  %s%s", PROF_DST, plugin_state[s->prof],
                    g_prof.staged ? "  (new build in install folder)" : "");
        else
            say(C_DIM, "  Profiler      not installed (optional, for development)");
    }
    say(C_TEXT, "");
    say(C_TEXT, "  X         install / update (reboots if needed)");
    if (g_prof.data)
        say(C_TEXT, "  SQUARE    %s the profiler (kernel plugin, samples the game while recording)",
                s->prof_line ? "remove" : "install");
    say(C_TEXT, "  TRIANGLE  uninstall everything (VitaGrafix patches stay)");
    say(C_TEXT, "  START     exit");
    say(C_TEXT, "");
}

/* actions */

static int step(int ok, const char *what) {
    say(ok ? C_OK : C_ERR, "  %s %s", ok ? "ok  " : "FAIL", what);
    return ok;
}

/* shaders: <name>_v.cg / <name>_f.cg from the payload -> SHADER_DIR/<name>_v.gxp ... */

static SceShaccCgSourceFile g_src;

static SceShaccCgSourceFile *shacc_open(const char *name, const SceShaccCgSourceLocation *from,
        const SceShaccCgCompileOptions *opts, const char **err) {
    (void)name; (void)from; (void)opts; (void)err;
    return &g_src;
}

static int compile_one(const char *name, char *text, size_t len) {
    size_t n = strlen(name);
    int frag = n > 5 && name[n - 4] == 'f';   // ..._f.cg
    g_src.fileName = name;
    g_src.text = text;
    g_src.size = (SceUInt32)len;
    SceShaccCgCompileOptions o;
    sceShaccCgInitializeCompileOptions(&o);
    o.mainSourceFile = name;
    o.targetProfile = frag ? SCE_SHACCCG_PROFILE_FP : SCE_SHACCCG_PROFILE_VP;
    o.entryFunctionName = "main";
    o.useFx = 1;
    o.optimizationLevel = 3;
    o.useFastmath = 1;
    o.warningLevel = 1;
    SceShaccCgCallbackList cb;
    sceShaccCgInitializeCallbackList(&cb, SCE_SHACCCG_TRIVIAL);
    cb.openFile = shacc_open;
    const SceShaccCgCompileOutput *out = sceShaccCgCompileProgram(&o, &cb, 0);

    char base[64], path[256];
    snprintf(base, sizeof(base), "%.*s", (int)(n - 3), name);
    int ok = out && out->programData;
    if (out && out->diagnosticCount > 0) {   // warnings or errors: keep them next to the output
        snprintf(path, sizeof(path), SHADER_DIR "/%s.log", base);
        SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
        for (int i = 0; fd >= 0 && i < out->diagnosticCount; i++) {
            const SceShaccCgDiagnosticMessage *m = &out->diagnostics[i];
            char line[512];
            int k = snprintf(line, sizeof(line), "%s line %u: %s\n",
                    m->level == SCE_SHACCCG_DIAGNOSTIC_LEVEL_ERROR ? "error" :
                    m->level == SCE_SHACCCG_DIAGNOSTIC_LEVEL_WARNING ? "warning" : "info",
                    m->location ? (unsigned)m->location->lineNumber : 0u, m->message);
            sceIoWrite(fd, line, k < (int)sizeof(line) ? k : (int)sizeof(line) - 1);
        }
        if (fd >= 0)
            sceIoClose(fd);
    }
    if (ok) {
        snprintf(path, sizeof(path), SHADER_DIR "/%s.gxp", base);
        ok = write_raw(path, out->programData, out->programSize) == 0;
    }
    if (out)
        sceShaccCgDestroyCompileOutput(out);
    return ok;
}

// 1: all compiled, 0: some failed, -1: no compiler
static int compile_shaders(void) {
    if (!exists(SHACC_PATH))
        return -1;
    static SceUID mod = -1;
    if (mod < 0) {
        mod = sceKernelLoadStartModule(SHACC_PATH, 0, NULL, 0, NULL, NULL);
        if (mod < 0)
            return 0;
        sceShaccCgSetDefaultAllocator(malloc, free);
    }
    sceIoMkdir("ux0:data/divasplit", 0777);
    sceIoMkdir(SHADER_DIR, 0777);
    SceUID d = sceIoDopen(SHADER_SRC);
    if (d < 0)
        return 1;   // nothing to compile
    int ok = 1;
    SceIoDirent e;
    while (sceIoDread(d, &e) > 0) {
        size_t n = strlen(e.d_name);
        if (n < 6 || strcmp(e.d_name + n - 3, ".cg") != 0)
            continue;
        char path[256];
        snprintf(path, sizeof(path), SHADER_SRC "/%s", e.d_name);
        size_t len = 0;
        char *text = read_file(path, &len);
        int r = text && compile_one(e.d_name, text, len);
        free(text);
        step(r, e.d_name);
        ok &= r;
    }
    sceIoDclose(d);
    return ok;
}

// returns 1 when a reboot is needed, 0 when not, -1 on errors
static int install(const state_t *s) {
    int reboot = 0, fail = 0;
    say(C_TITLE, "Installing...");

    if (s->plugin < 2) {
        sceIoMkdir("ur0:tai", 0777);
        fail |= !step(write_file(PLUGIN_DST, g_plugin.data, g_plugin.len, 0) == 0, PLUGIN_DST);
        note_install(&g_plugin);
        reboot = 1;
    }

    char *tai = read_file(s->tai, NULL);
    if (!tai) {
        fail |= !step(0, "read taiHEN config");
    } else {
        char *out = NULL;
        if (tai_install(tai, g_titles, g_ntitles, PLUGIN_LINE, &out) < 0)
            fail |= !step(0, "edit taiHEN config");
        else if (out) {
            fail |= !step(write_file(s->tai, out, strlen(out), 1) == 0, s->tai);
            reboot = 1;
        }
        free(out);
        free(tai);
    }

    if (s->qmr_dir && g_qm.data && s->qm < 2) {
        char path[256];
        snprintf(path, sizeof(path), "%s/" QM_NAME, s->qmr_dir);
        fail |= !step(write_file(path, g_qm.data, g_qm.len, 0) == 0, path);
        note_install(&g_qm);
        reboot = 1;
    }

    if (s->prof_line && g_prof.data && s->prof < 2) {
        fail |= !step(write_file(PROF_DST, g_prof.data, g_prof.len, 0) == 0, PROF_DST);
        note_install(&g_prof);
        reboot = 1;
    }

    if (s->vg_files) {
        char *pl = read_file(VG_PATCHLIST, NULL);
        char *out = NULL;
        if (!pl || patchlist_edit(pl, &out) < 0)
            fail |= !step(0, "edit " VG_PATCHLIST);
        else if (out)
            fail |= !step(write_file(VG_PATCHLIST, out, strlen(out), 1) == 0, VG_PATCHLIST);
        free(out);
        free(pl);

        char *cfg = read_file(VG_CONFIG, NULL);
        out = NULL;
        if (vg_config_fps(cfg ? cfg : "", g_titles, g_ntitles, &out) < 0)
            fail |= !step(0, "edit " VG_CONFIG);
        else if (out)
            fail |= !step(write_file(VG_CONFIG, out, strlen(out), 1) == 0, VG_CONFIG);
        free(out);
        free(cfg);
    }

    int sh = compile_shaders();
    if (sh < 0)
        say(C_DIM, "  (no " SHACC_PATH ": the plugin's built-in shaders are used)");
    else if (sh == 0)
        say(C_WARN, "  some shaders didn't compile: see " SHADER_DIR "/*.log");

    if (fail) {
        say(C_ERR, "Something failed. Edited files have a .bak copy next to them.");
        return -1;
    }
    remove_staged();
    say(C_OK, "Done.");
    return reboot;
}

static int uninstall(const state_t *s) {
    int fail = 0;
    say(C_TITLE, "Uninstalling...");
    char *tai = read_file(s->tai, NULL);
    if (tai) {
        char *out = NULL;
        if (tai_uninstall(tai, g_titles, g_ntitles, &out) < 0)
            fail |= !step(0, "edit taiHEN config");
        else if (out)
            fail |= !step(write_file(s->tai, out, strlen(out), 1) == 0, s->tai);
        free(out);
        free(tai);
    }
    const char *files[] = {PLUGIN_DST, PROF_DST, QMR_UX "/" QM_NAME, QMR_UR "/" QM_NAME};
    for (int i = 0; i < 4; i++)
        if (exists(files[i]))
            fail |= !step(sceIoRemove(files[i]) >= 0, files[i]);
    const char *names[] = {PLUGIN_NAME, QM_NAME, PROF_NAME};
    for (int i = 0; i < 3; i++) {
        char path[128];
        testbuild_path(path, sizeof(path), names[i]);
        sceIoRemove(path);
    }
    say(fail ? C_ERR : C_OK, fail ? "Something failed." : "Done. Settings and logs stay in ux0:data/divasplit.");
    return fail ? -1 : 1;
}

// SQUARE: add or remove the profiler
static int prof_toggle(const state_t *s) {
    int fail = 0;
    char *tai = read_file(s->tai, NULL);
    char *out = NULL;
    if (!tai) {
        step(0, "read taiHEN config");
        return -1;
    }
    if (!s->prof_line) {
        say(C_TITLE, "Installing the profiler...");
        sceIoMkdir("ur0:tai", 0777);
        fail |= !step(write_file(PROF_DST, g_prof.data, g_prof.len, 0) == 0, PROF_DST);
        note_install(&g_prof);
        if (!fail && tai_kernel_add(tai, PROF_LINE, &out) == 0 && out)
            fail |= !step(write_file(s->tai, out, strlen(out), 1) == 0, s->tai);
    } else {
        say(C_TITLE, "Removing the profiler...");
        if (tai_kernel_remove(tai, PROF_NAME, &out) == 0 && out)
            fail |= !step(write_file(s->tai, out, strlen(out), 1) == 0, s->tai);
        if (!fail && exists(PROF_DST))
            fail |= !step(sceIoRemove(PROF_DST) >= 0, PROF_DST);
    }
    free(out);
    free(tai);
    if (fail) {
        say(C_ERR, "Something failed. Edited files have a .bak copy next to them.");
        return -1;
    }
    if (g_prof.staged) {
        char path[256];
        snprintf(path, sizeof(path), STAGE_DIR "/%s", g_prof.name);
        sceIoRemove(path);
        g_prof.staged = 0;
    }
    say(C_OK, "Done.");
    return 1;
}

/* input */

static unsigned int g_prev;

static unsigned int pressed(void) {
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    unsigned int p = pad.buttons & ~g_prev;
    g_prev = pad.buttons;
    return p;
}

static void wait_frames(int n) {
    while (n-- > 0)
        sceDisplayWaitVblankStart();
}

// countdown; O cancels. Returns 1 when it ran out.
static int countdown(const char *what, int seconds) {
    for (int s = seconds; s > 0; s--) {
        say(C_WARN, "%s in %d s... (CIRCLE cancels)", what, s);
        for (int f = 0; f < 60; f++) {
            if (pressed() & SCE_CTRL_CIRCLE)
                return 0;
            wait_frames(1);
        }
    }
    return 1;
}

static void wait_any(void) {
    say(C_DIM, "Press any button.");
    pressed();
    while (!pressed())
        wait_frames(1);
}

int main(void) {
    fb_init();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);

    if (!load_payload()) {
        clear();
        say(C_TITLE, "divasplit installer %s", VERSION);
        say(C_ERR, "The installer's files are missing (%s). Reinstall the VPK.", PAYLOAD_DIR);
        wait_any();
        sceKernelExitProcess(0);
        return 0;
    }

    state_t s;
    check(&s);
    show_status(&s);
    pressed();
    for (;;) {
        unsigned int p = pressed();
        if (p & SCE_CTRL_START)
            break;
        int r = -2;
        if (p & SCE_CTRL_CROSS) {
            r = install(&s);
        } else if ((p & SCE_CTRL_SQUARE) && g_prof.data) {
            say(C_WARN, "Press SQUARE again to %s the profiler, any other button cancels.",
                    s.prof_line ? "remove" : "install");
            unsigned int q;
            while (!(q = pressed()))
                wait_frames(1);
            r = (q & SCE_CTRL_SQUARE) ? prof_toggle(&s) : -3;
            // the profiler needs the plugin from the same build
            if (r == 1 && !s.prof_line && install(&s) < 0)
                r = -1;
        } else if (p & SCE_CTRL_TRIANGLE) {
            say(C_WARN, "Press TRIANGLE again to uninstall, any other button cancels.");
            unsigned int q;
            while (!(q = pressed()))
                wait_frames(1);
            if (q & SCE_CTRL_TRIANGLE)
                r = uninstall(&s);
            else
                r = -3;
        }
        if (r == 1) {
            if (countdown("Rebooting", 3))
                scePowerRequestColdReset();
            wait_any();
        } else if (r == 0) {
            say(C_TEXT, "Nothing needed a reboot.");
            wait_any();
        } else if (r == -1) {
            wait_any();
        }
        if (r != -2) {
            check(&s);
            show_status(&s);
        }
        wait_frames(1);
    }
    sceKernelExitProcess(0);
    return 0;
}

/*
 * Settings shared by the game plugin (divasplit.suprx) and the Quick Menu
 * plugin (divasplit_qm.suprx), stored in ux0:data/divasplit/settings.txt:
 *
 *   active=F2ND
 *   X preset=0 sync=0 avs=1 bar=1 rec=0 opt=0
 *   F preset=0 sync=0 avs=0 bar=1 rec=0 opt=0
 *   F2ND preset=1 sync=1 avs=1 bar=0 rec=0 opt=1
 *
 * "active" is the game that started last. The game plugin rereads the file
 * twice a second, so a change made in the Quick Menu applies right away.
 */
#ifndef DIVASPLIT_SETTINGS_H
#define DIVASPLIT_SETTINGS_H

#include <psp2/kernel/clib.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#define SETTINGS_DIR  "ux0:data/divasplit"
#define SETTINGS_FILE SETTINGS_DIR "/settings.txt"

enum { GAME_X, GAME_F, GAME_F2ND, SETTINGS_GAME_COUNT };
static const char *const SETTINGS_GAME_TAG[SETTINGS_GAME_COUNT] = {"X", "F", "F2ND"};
static const char *const SETTINGS_GAME_NAME[SETTINGS_GAME_COUNT] = {
    "Project DIVA X", "Project DIVA f", "Project DIVA f 2nd"};

// skip_3d/fx/setup are the render stage groups the preset skips; the game
// table in main.c maps them to each game's stage numbers
typedef struct {
    const char *name;
    unsigned char skip_3d, skip_fx, skip_setup;
    unsigned char period;   // run skipped stages 1 frame in `period`; 0 = never
} preset_t;

static const preset_t PRESETS[] = {
    {"OFF",        0, 0, 0, 1},
    {"3D 1/2",     1, 0, 0, 2},
    {"3D+FX 1/2",  1, 1, 0, 2},
    {"ALL3D 1/2",  1, 1, 1, 2},
    {"3D 1/3",     1, 0, 0, 3},
    {"NO 3D test", 1, 0, 0, 0},
};
#define PRESET_COUNT (int)(sizeof(PRESETS) / sizeof(PRESETS[0]))

typedef struct {
    int preset, sync, avs, bar, rec, opt;
    int drs, sharp;   // dynamic resolution, sharpening of the 3D image (f, f 2nd)
} game_settings_t;

typedef struct {
    int active;   // index into SETTINGS_GAME_TAG, -1 = none yet
    game_settings_t game[SETTINGS_GAME_COUNT];
} settings_t;

static inline void settings_defaults(settings_t *s) {
    s->active = -1;
    for (int i = 0; i < SETTINGS_GAME_COUNT; i++) {
        s->game[i].preset = 0;
        s->game[i].sync = 0;
        s->game[i].avs = 0;
        s->game[i].bar = 1;
        s->game[i].rec = 0;
        // the engine optimizations exist for f 2nd only (opt.c)
        s->game[i].opt = sceClibStrncmp(SETTINGS_GAME_TAG[i], "F2ND", 5) == 0;
        s->game[i].drs = 0;     // lowers the resolution: asked for, not by default
        s->game[i].sharp = 1;   // f 2nd: only below 640 wide (drs, or VitaGrafix IB); f: whenever stretched
    }
}

static inline int settings_tag_index(const char *p, int len) {
    for (int i = 0; i < SETTINGS_GAME_COUNT; i++)
        if ((int)sceClibStrnlen(SETTINGS_GAME_TAG[i], 8) == len && sceClibStrncmp(p, SETTINGS_GAME_TAG[i], len) == 0)
            return i;
    return -1;
}

// reads "key=<number>" from one line, returns 1 if found
static inline int settings_get_int(const char *line, const char *end, const char *key, int *out) {
    int klen = (int)sceClibStrnlen(key, 16);
    for (const char *p = line; p + klen < end; p++) {
        if ((p == line || p[-1] == ' ') && sceClibStrncmp(p, key, klen) == 0 && p[klen] == '=') {
            int v = 0;
            for (p += klen + 1; p < end && *p >= '0' && *p <= '9'; p++)
                v = v * 10 + (*p - '0');
            *out = v;
            return 1;
        }
    }
    return 0;
}

static inline void settings_load(settings_t *s) {
    char buf[1024];
    settings_defaults(s);
    SceUID fd = sceIoOpen(SETTINGS_FILE, SCE_O_RDONLY, 0);
    if (fd < 0)
        return;
    int len = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (len <= 0)
        return;
    buf[len] = '\0';

    const char *line = buf;
    while (line < buf + len) {
        const char *end = line;
        while (end < buf + len && *end != '\n' && *end != '\r')
            end++;
        if (end - line > 7 && sceClibStrncmp(line, "active=", 7) == 0) {
            s->active = settings_tag_index(line + 7, (int)(end - line - 7));
        } else {
            const char *sp = line;
            while (sp < end && *sp != ' ')
                sp++;
            int g = settings_tag_index(line, (int)(sp - line));
            if (g >= 0) {
                game_settings_t *gs = &s->game[g];
                settings_get_int(sp, end, "preset", &gs->preset);
                settings_get_int(sp, end, "sync", &gs->sync);
                settings_get_int(sp, end, "avs", &gs->avs);
                settings_get_int(sp, end, "bar", &gs->bar);
                settings_get_int(sp, end, "rec", &gs->rec);
                settings_get_int(sp, end, "opt", &gs->opt);
                settings_get_int(sp, end, "drs", &gs->drs);
                settings_get_int(sp, end, "sharp", &gs->sharp);
                if (gs->preset < 0 || gs->preset >= PRESET_COUNT)
                    gs->preset = 0;
            }
        }
        line = end + 1;
    }
}

static inline int settings_save(const settings_t *s) {
    char buf[1024];
    int n = 0;
    if (s->active >= 0)
        n += sceClibSnprintf(buf + n, sizeof(buf) - n, "active=%s\n", SETTINGS_GAME_TAG[s->active]);
    for (int i = 0; i < SETTINGS_GAME_COUNT; i++) {
        const game_settings_t *gs = &s->game[i];
        n += sceClibSnprintf(buf + n, sizeof(buf) - n, "%s preset=%d sync=%d avs=%d bar=%d rec=%d opt=%d drs=%d sharp=%d\n",
                SETTINGS_GAME_TAG[i], gs->preset, gs->sync, gs->avs, gs->bar, gs->rec, gs->opt, gs->drs, gs->sharp);
    }
    sceIoMkdir(SETTINGS_DIR, 0777);
    SceUID fd = sceIoOpen(SETTINGS_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0)
        return fd;
    sceIoWrite(fd, buf, n);
    sceIoClose(fd);
    return 0;
}

static inline int game_settings_equal(const game_settings_t *a, const game_settings_t *b) {
    return a->preset == b->preset && a->sync == b->sync && a->avs == b->avs
        && a->bar == b->bar && a->rec == b->rec && a->opt == b->opt && a->drs == b->drs && a->sharp == b->sharp;
}

#endif

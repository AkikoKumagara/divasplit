#ifndef DIVASPLIT_DRS_H
#define DIVASPLIT_DRS_H

#include <psp2/types.h>
#include <stdint.h>

// dynamic resolution of the 3D world (drs.c)
typedef struct {
    uint32_t stage0;     // render manager's per-frame setup (mgr): writes the 3D size
} drs_game_t;

int drs_init(SceUID modid, const drs_game_t *g);   // hooks at game start
void drs_shutdown(void);
void drs_set_range(int min_w, int max_w);          // widths; heights keep 960:544
// once per frame (main thread): last frame's time, its GPU and vblank waits (us)
void drs_frame(unsigned int frame_us, unsigned int gpu_us, unsigned int vs_us);
int drs_width(void);                               // 3D width used this frame (0: not in a PV)
int drs_diag(char *buf, int size);

#endif

#ifndef DIVASPLIT_DRS_H
#define DIVASPLIT_DRS_H

#include <psp2/types.h>
#include <stdint.h>

// dynamic resolution of the 3D world (drs.c)
typedef struct {
    uint32_t stage0;     // render manager's per-frame setup (mgr): writes the 3D size
    uint32_t scene_ret;  // return address of the FBO scene begin's sceGxmBeginScene
    uint32_t d_mgr;      // seg1 offset of the pointer to the render manager
} drs_game_t;

int drs_init(SceUID modid, uintptr_t seg0, uintptr_t seg1, const drs_game_t *g);   // hooks at game start
// main.c's sceGxmBeginScene hook: the valid region for the game's 3D, tonemap and stage-2
// scenes (only their image's area; the engine passes none, so the GPU processes all
// 1024x544). Returns 1 with *xmax / *ymax set when it applies.
int drs_scene_region(uint32_t ret_addr, const void *color, unsigned int *xmax, unsigned int *ymax);
void drs_shutdown(void);
void drs_set_range(int min_w, int max_w);          // widths; heights keep 960:544
// once per frame (main thread): last frame's time, its GPU and vblank waits (us)
void drs_frame(unsigned int frame_us, unsigned int gpu_us, unsigned int vs_us);
int drs_width(void);                               // 3D width used this frame (0: not in a PV)
int drs_diag(char *buf, int size);

#endif

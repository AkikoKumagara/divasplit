#ifndef DIVASPLIT_SHARPEN_H
#define DIVASPLIT_SHARPEN_H

#include <psp2/types.h>
#include <stdint.h>

// sharpening of the 3D image as the game stretches it to the screen (sharpen.c)
typedef struct {
    uint32_t blit;       // the engine's textured quad (mgr, uv[4], colour, z in s0)
    uint32_t blit_ret;   // its return address in the final blit (3D image -> framebuffer)
    uint32_t begin_ret;  // return address of the framebuffer scene's sceGxmBeginScene
    uint32_t d_gl;       // the engine's GL state "G" (seg1 offset)
    uint32_t tex_units;  // G + this: the engine's bound texture objects (SceGxmTexture at +8)
} sharpen_game_t;

// hooks the blit at game start, if the shaders are there
// (ux0:data/divasplit/shaders/, compiled by the installer); < 0: stays off
int sharpen_init(SceUID modid, uintptr_t seg0, uintptr_t seg1, const sharpen_game_t *g);
void sharpen_shutdown(void);
void sharpen_set_strength(int percent);   // 0..100
void sharpen_set_below(int width);        // sharpen only 3D images narrower than this
// main.c's sceGxmBeginScene hook, before the game's call: runs our pass when
// it is the framebuffer scene of stage 4 (returns 1 if it did)
int sharpen_before_scene(void *ctx, uint32_t ret_addr, int stage);
int sharpen_diag(char *buf, int size);

#endif

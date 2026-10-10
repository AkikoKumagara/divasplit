/*
 * Sharpening (f 2nd): AMD FSR 1 RCAS (shaders/rcas_f.cg) on the 3D image as
 * the game stretches it to the screen. Lowering the 3D resolution (VitaGrafix
 * IB) frees most of the GPU time a heavy PV needs; this brings back detail.
 *
 * The 3D world is tonemapped into an 8-bit 1024x544 texture (FBO 11; the
 * image fills its top-left w x h) and drawn to the framebuffer by one textured
 * quad: seg0+0x25E208(mgr, uv[4], colour, z), called at 0x261246. That
 * function sets the engine's GL state (ortho projection, depth / blend / cull
 * off, colour) and emits the quad, ending in one sceGxmDraw. We let it run as
 * it is and replace only that draw: our programs, the same texture (bilinear),
 * the same corners and UVs, the colour multiplied in. Everything we set
 * (programs, texture, uniforms, vertex stream) the engine sets again before
 * its next draw; viewport, region clip and cull mode stay the engine's.
 * At 1:1 (menus) the original draw runs.
 *
 * Our programs come from ux0:data/divasplit/shaders/, compiled on the Vita by
 * the installer (ur0:data/libshacccg.suprx). Without them nothing is hooked.
 */
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/clib.h>
#include <psp2/io/fcntl.h>
#include <psp2/gxm.h>
#include <taihen.h>

#include "opt.h"
#include "settings.h"
#include "sharpen.h"

#define SH_DIR SETTINGS_DIR "/shaders"
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define NID_LIB_GXM 0xF76B66BD
#define NID_DRAW    0xBC059AFC

typedef struct {
    SceUID uid;
    void *base;
    unsigned int size;
} block_t;

enum { B_HOST, B_PATCH, B_VUSSE, B_FUSSE, B_PROGS, B_QUAD, B_COUNT };
static block_t s_b[B_COUNT];
static int s_mapped[B_COUNT], s_vusse_map, s_fusse_map;
static SceClibMspace s_host;
static SceGxmShaderPatcher *s_patcher;
static const SceGxmProgram *s_vprog, *s_fprog;
static SceGxmShaderPatcherId s_vid, s_fid;
static SceGxmVertexProgram *s_vp;
static SceGxmFragmentProgram *s_fp;
static const SceGxmProgramParameter *s_u_rect, *s_u_k, *s_u_lim, *s_u_col;
static const float *s_quad;
static const uint16_t *s_idx;
static int s_state;   // 1 programs loaded and hooked, 2 GPU side ready too, < 0 why not

static const sharpen_game_t *s_game;
static uint8_t *s_g;
static uint32_t s_blit_ret;
static float s_strength = 0.5f;
enum { H_BLIT, H_DRAW, H_COUNT };
static tai_hook_ref_t s_ref[H_COUNT];
static SceUID s_hook[H_COUNT] = {-1, -1};

static int s_in_blit;           // the blit runs: its draw is ours
static float s_uv[4], s_col[4];
static SceGxmTexture s_tex;
static volatile unsigned int d_blits, d_ours, d_one, d_bad;

/* ---- memory and programs ---------------------------------------------- */

static int block_alloc(block_t *b, const char *name, SceKernelMemBlockType type, unsigned int size) {
    b->size = ALIGN(size, 4096);
    b->uid = sceKernelAllocMemBlock(name, type, b->size, NULL);
    if (b->uid < 0)
        return b->uid;
    sceKernelGetMemBlockBase(b->uid, &b->base);
    return 0;
}

static void block_free(block_t *b) {
    if (b->uid > 0)
        sceKernelFreeMemBlock(b->uid);
    b->uid = 0;
    b->base = NULL;
}

// uncached main memory the GPU reads
static int gpu_alloc(int i, const char *name, unsigned int size) {
    int r = block_alloc(&s_b[i], name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size);
    if (r < 0)
        return r;
    r = sceGxmMapMemory(s_b[i].base, s_b[i].size, SCE_GXM_MEMORY_ATTRIB_READ);
    s_mapped[i] = r >= 0;
    return r;
}

static void *host_alloc(void *user, SceSize size) {
    (void)user;
    return sceClibMspaceMalloc(s_host, size);
}

static void host_free(void *user, void *mem) {
    (void)user;
    sceClibMspaceFree(s_host, mem);
}

// a compiled program from SH_DIR into s_b[B_PROGS] at *off
static const SceGxmProgram *load_gxp(const char *name, unsigned int *off) {
    char path[128];
    sceClibSnprintf(path, sizeof(path), SH_DIR "/%s", name);
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0)
        return NULL;
    uint8_t *dst = (uint8_t *)s_b[B_PROGS].base + *off;
    int n = sceIoRead(fd, dst, s_b[B_PROGS].size - *off);
    sceIoClose(fd);
    if (n <= 0 || sceGxmProgramCheck((const SceGxmProgram *)dst) < 0)
        return NULL;
    *off = ALIGN(*off + (unsigned int)n, 16);
    return (const SceGxmProgram *)dst;
}

// at game start: the compiled programs
static int load(void) {
    int r;
    if ((r = block_alloc(&s_b[B_PROGS], "divasplit_sh_progs", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 16 * 1024)) < 0)
        return r;
    unsigned int off = 0;
    s_vprog = load_gxp("blit_v.gxp", &off);
    s_fprog = load_gxp("rcas_f.gxp", &off);
    if (!s_vprog || !s_fprog)
        return -3;   // not compiled (no libshacccg.suprx when installing)
    return 1;
}

// at the first blit (GXM is initialized by the game after our module_start:
// mapping memory or making a patcher before that fails with 0x805B0000)
static int gpu_setup(void) {
    int r;
    if ((r = block_alloc(&s_b[B_HOST], "divasplit_sh_host", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * 1024)) < 0)
        return r;
    if (!(s_host = sceClibMspaceCreate(s_b[B_HOST].base, s_b[B_HOST].size)))
        return -2;
    if ((r = gpu_alloc(B_PATCH, "divasplit_sh_patch", 64 * 1024)) < 0)
        return r;
    unsigned int voff, foff;
    if ((r = block_alloc(&s_b[B_VUSSE], "divasplit_sh_vusse", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 16 * 1024)) < 0
            || (r = sceGxmMapVertexUsseMemory(s_b[B_VUSSE].base, s_b[B_VUSSE].size, &voff)) < 0)
        return r;
    s_vusse_map = 1;
    if ((r = block_alloc(&s_b[B_FUSSE], "divasplit_sh_fusse", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 16 * 1024)) < 0
            || (r = sceGxmMapFragmentUsseMemory(s_b[B_FUSSE].base, s_b[B_FUSSE].size, &foff)) < 0)
        return r;
    s_fusse_map = 1;

    SceGxmShaderPatcherParams pp;
    sceClibMemset(&pp, 0, sizeof(pp));
    pp.hostAllocCallback = host_alloc;
    pp.hostFreeCallback = host_free;
    pp.bufferMem = s_b[B_PATCH].base;
    pp.bufferMemSize = s_b[B_PATCH].size;
    pp.vertexUsseMem = s_b[B_VUSSE].base;
    pp.vertexUsseMemSize = s_b[B_VUSSE].size;
    pp.vertexUsseOffset = voff;
    pp.fragmentUsseMem = s_b[B_FUSSE].base;
    pp.fragmentUsseMemSize = s_b[B_FUSSE].size;
    pp.fragmentUsseOffset = foff;
    if ((r = sceGxmShaderPatcherCreate(&pp, &s_patcher)) < 0
            || (r = sceGxmShaderPatcherRegisterProgram(s_patcher, s_vprog, &s_vid)) < 0
            || (r = sceGxmShaderPatcherRegisterProgram(s_patcher, s_fprog, &s_fid)) < 0)
        return r;

    const SceGxmProgramParameter *pos = sceGxmProgramFindParameterByName(s_vprog, "position");
    const SceGxmProgramParameter *t = sceGxmProgramFindParameterByName(s_vprog, "t");
    s_u_rect = sceGxmProgramFindParameterByName(s_vprog, "rect");
    s_u_k = sceGxmProgramFindParameterByName(s_fprog, "k");
    s_u_lim = sceGxmProgramFindParameterByName(s_fprog, "lim");
    s_u_col = sceGxmProgramFindParameterByName(s_fprog, "col");
    if (!pos || !t || !s_u_rect || !s_u_k || !s_u_lim || !s_u_col)
        return -4;
    SceGxmVertexAttribute attr[2];
    sceClibMemset(attr, 0, sizeof(attr));
    attr[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attr[0].componentCount = 2;
    attr[0].regIndex = (unsigned short)sceGxmProgramParameterGetResourceIndex(pos);
    attr[1].offset = 8;
    attr[1].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attr[1].componentCount = 2;
    attr[1].regIndex = (unsigned short)sceGxmProgramParameterGetResourceIndex(t);
    SceGxmVertexStream stream = {16, SCE_GXM_INDEX_SOURCE_INDEX_16BIT};
    if ((r = sceGxmShaderPatcherCreateVertexProgram(s_patcher, s_vid, attr, 2, &stream, 1, &s_vp)) < 0)
        return r;
    // the framebuffer: 8 bits per channel, no blending (the original blit has none)
    if ((r = sceGxmShaderPatcherCreateFragmentProgram(s_patcher, s_fid, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
            SCE_GXM_MULTISAMPLE_NONE, NULL, s_vprog, &s_fp)) < 0)
        return r;

    // the quad as a strip: clip-space corner, t (top-left = 0,0, like the game's (u0, v0))
    if ((r = gpu_alloc(B_QUAD, "divasplit_sh_quad", 4096)) < 0)
        return r;
    static const float Q[16] = {-1, 1, 0, 0,   1, 1, 1, 0,   -1, -1, 0, 1,   1, -1, 1, 1};
    float *q = (float *)s_b[B_QUAD].base;
    for (int i = 0; i < 16; i++)
        q[i] = Q[i];
    uint16_t *idx = (uint16_t *)(q + 16);
    for (int i = 0; i < 4; i++)
        idx[i] = (uint16_t)i;
    s_quad = q;
    s_idx = idx;
    return 1;
}

/* ---- the blit ----------------------------------------------------------- */

// our draw in place of the blit's: 0 = let the game's draw run
static int blit(SceGxmContext *ctx) {
    const uint8_t *obj = *(const uint8_t *const volatile *)(s_g + s_game->tex_units);   // unit 0
    if (!obj || !(*(const volatile uint32_t *)obj & 2) || !*(void *const volatile *)(obj + 0x30)) {
        d_bad++;
        return 0;
    }
    s_tex = *(const SceGxmTexture *)(obj + 8);
    SceGxmTextureFormat fmt = sceGxmTextureGetFormat(&s_tex);
    if (fmt != SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR && fmt != SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ARGB) {
        d_bad++;
        return 0;
    }
    float tw = (float)sceGxmTextureGetWidth(&s_tex), th = (float)sceGxmTextureGetHeight(&s_tex);
    float u0 = s_uv[0], v0 = s_uv[1], u1 = s_uv[2], v1 = s_uv[3];
    float iw = u1 > u0 ? (u1 - u0) * tw : (u0 - u1) * tw;
    if (iw >= 959.0f) {   // drawn 1:1 (menus): nothing to sharpen
        d_one++;
        return 0;
    }
    sceGxmTextureSetMinFilter(&s_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetMagFilter(&s_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);

    sceGxmSetVertexProgram(ctx, s_vp);
    sceGxmSetFragmentProgram(ctx, s_fp);
    sceGxmSetFragmentTexture(ctx, 0, &s_tex);
    void *vb = NULL, *fb = NULL;
    if (sceGxmReserveVertexDefaultUniformBuffer(ctx, &vb) < 0 || !vb
            || sceGxmReserveFragmentDefaultUniformBuffer(ctx, &fb) < 0 || !fb) {
        d_bad++;
        return 1;   // our programs are set: no draw at all this frame rather than a wrong one
    }
    sceGxmSetUniformDataF(vb, s_u_rect, 0, 4, s_uv);
    float hx = 0.5f / tw, hy = 0.5f / th;
    float k[4] = {1.0f / tw, 1.0f / th, s_strength, 0.0f};
    float lim[4] = {(u0 < u1 ? u0 : u1) + hx, (v0 < v1 ? v0 : v1) + hy,
                    (u0 < u1 ? u1 : u0) - hx, (v0 < v1 ? v1 : v0) - hy};
    sceGxmSetUniformDataF(fb, s_u_k, 0, 4, k);
    sceGxmSetUniformDataF(fb, s_u_lim, 0, 4, lim);
    sceGxmSetUniformDataF(fb, s_u_col, 0, 4, s_col);
    sceGxmSetVertexStream(ctx, 0, s_quad);
    TAI_CONTINUE(int, s_ref[H_DRAW], ctx, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, s_idx, 4);
    d_ours++;
    return 1;
}

// import hook, sceGxmDraw (installed at game start)
static int draw_hook(SceGxmContext *ctx, SceGxmPrimitiveType type, SceGxmIndexFormat ifmt,
        const void *idx, unsigned int n) {
    if (s_in_blit) {
        s_in_blit = 0;   // the blit's one draw
        if (blit(ctx))
            return 0;
    }
    return TAI_CONTINUE(int, s_ref[H_DRAW], ctx, type, ifmt, idx, n);
}

// seg0+0x25E208(mgr, uv[4], colour[4], z): z is a float in s0, so the next hook /
// original is called through a prototyped pointer (TAI_CONTINUE would pass a double)
typedef void (*blit_fn)(void *mgr, const float *uv, const float *col, float z);
static void blit_hook(void *mgr, const float *uv, const float *col, float z) {
    struct _tai_hook_user *h = (struct _tai_hook_user *)s_ref[H_BLIT];
    struct _tai_hook_user *nx = (struct _tai_hook_user *)h->next;
    blit_fn next = (blit_fn)(nx ? nx->func : h->old);
    uint32_t ra = (uint32_t)__builtin_return_address(0);
    if (ra != s_blit_ret || s_strength <= 0.0f || !opt_cut_on(OPT_SHARPEN) || !uv || !col) {
        next(mgr, uv, col, z);
        return;
    }
    if (s_state == 1) {   // first time: the GPU side, once
        int r = gpu_setup();
        s_state = r == 1 ? 2 : r < 0 ? r : -5;
    }
    if (s_state != 2) {
        next(mgr, uv, col, z);
        return;
    }
    for (int i = 0; i < 4; i++) {
        s_uv[i] = uv[i];
        s_col[i] = col[i];
    }
    d_blits++;
    s_in_blit = 1;
    next(mgr, uv, col, z);
    s_in_blit = 0;   // if the blit didn't draw after all
}

/* ---- install ------------------------------------------------------------ */

int sharpen_init(SceUID modid, uintptr_t seg0, uintptr_t seg1, const sharpen_game_t *g) {
    if (s_state != 0)
        return s_state;
    s_state = load();
    if (s_state != 1)
        return s_state;
    s_game = g;
    s_g = (uint8_t *)(seg1 + g->d_gl);
    s_blit_ret = (uint32_t)(seg0 + g->blit_ret) | 1;
    s_hook[H_DRAW] = taiHookFunctionImport(&s_ref[H_DRAW], TAI_MAIN_MODULE, NID_LIB_GXM, NID_DRAW, draw_hook);
    if (s_hook[H_DRAW] < 0)
        return s_state = s_hook[H_DRAW];
    s_hook[H_BLIT] = taiHookFunctionOffset(&s_ref[H_BLIT], modid, 0, g->blit, 1, blit_hook);
    if (s_hook[H_BLIT] < 0)
        return s_state = s_hook[H_BLIT];
    return s_state;
}

void sharpen_shutdown(void) {
    for (int i = H_COUNT - 1; i >= 0; i--) {   // the blit hook first: it arms the draw hook
        if (s_hook[i] >= 0)
            taiHookRelease(s_hook[i], s_ref[i]);
        s_hook[i] = -1;
    }
    if (s_patcher) {
        if (s_vp)
            sceGxmShaderPatcherReleaseVertexProgram(s_patcher, s_vp);
        if (s_fp)
            sceGxmShaderPatcherReleaseFragmentProgram(s_patcher, s_fp);
        sceGxmShaderPatcherDestroy(s_patcher);
    }
    s_patcher = NULL;
    if (s_vusse_map)
        sceGxmUnmapVertexUsseMemory(s_b[B_VUSSE].base);
    if (s_fusse_map)
        sceGxmUnmapFragmentUsseMemory(s_b[B_FUSSE].base);
    s_vusse_map = s_fusse_map = 0;
    for (int i = 0; i < B_COUNT; i++) {
        if (s_mapped[i])
            sceGxmUnmapMemory(s_b[i].base);
        s_mapped[i] = 0;
        block_free(&s_b[i]);
    }
    s_state = 0;
}

void sharpen_set_strength(int percent) {
    s_strength = (percent < 0 ? 0 : percent > 100 ? 100 : percent) / 100.0f;
}

int sharpen_diag(char *buf, int size) {
    return sceClibSnprintf(buf, size, "sharpen: state %d hooks %08X %08X strength %d; blits %u ours %u 1:1 %u bad %u",
            s_state, s_hook[H_BLIT], s_hook[H_DRAW], (int)(s_strength * 100.0f + 0.5f), d_blits, d_ours, d_one, d_bad);
}

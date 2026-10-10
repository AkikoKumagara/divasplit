/*
 * Sharpening (f 2nd): the 3D image, at its own resolution, just before the
 * game stretches it to the screen. Lowering the 3D resolution (VitaGrafix IB)
 * frees most of the GPU time a heavy PV needs; this brings back some detail.
 *
 * The 3D world is tonemapped into an 8-bit 1024x544 texture (FBO 11; the
 * image fills its top-left w x h) and drawn to the framebuffer by one textured
 * quad, seg0+0x25E208(mgr, uv[4], colour, z) called at 0x261246, inside the
 * framebuffer scene that 0x4E07CE begins (sceGxmBeginScene at 0x4E07FE).
 *
 *   - At each blit we note that texture (the engine's texture object bound to
 *     unit 0, SceGxmTexture at +8) and the image's area (the blit's UVs).
 *   - When stage 4 begins the framebuffer scene, we first run a scene of our
 *     own: shaders/sharpen_f.cg reads the image (5 taps) and writes a sharpened
 *     copy into our texture of the same size, format and layout; the valid
 *     region keeps the GPU to the w x h corner.
 *   - During the blit the engine's texture object points at our copy (its
 *     descriptor swapped for the call, then put back), so the game's own blit
 *     stretches the sharpened image.
 *
 * Our scene sets programs, texture, uniforms, vertex stream, viewport, region
 * clip, cull mode and depth; the blit, the next draw, sets all of them again.
 * Menus (drawn 1:1) are left alone. AMD's RCAS on the full-screen blit was
 * tried first: 12 ms of GPU time a frame on stock clocks, too much.
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

typedef struct {
    SceUID uid;
    void *base;
    unsigned int size;
} block_t;

enum { B_HOST, B_PATCH, B_VUSSE, B_FUSSE, B_PROGS, B_QUAD, B_COLOR, B_RTMEM, B_COUNT };
static block_t s_b[B_COUNT];
static int s_mapped[B_COUNT], s_vusse_map, s_fusse_map;
static SceClibMspace s_host;
static SceGxmShaderPatcher *s_patcher;
static const SceGxmProgram *s_vprog, *s_fprog;
static SceGxmShaderPatcherId s_vid, s_fid;
static SceGxmVertexProgram *s_vp;
static SceGxmFragmentProgram *s_fp;
static const SceGxmProgramParameter *s_u_rect, *s_u_k, *s_u_lim;
static const float *s_quad;
static const uint16_t *s_idx;
static int s_state;   // 1 programs loaded and hooked, 2 GPU side ready too, < 0 why not

static const sharpen_game_t *s_game;
static uint8_t *s_g;
static uint32_t s_blit_ret, s_begin_ret;
static float s_amount = 1.5f, s_over = 0.25f;
static tai_hook_ref_t s_ref;
static SceUID s_hook = -1;

static uint8_t *s_src;          // the engine's texture object of the 3D image (last blit)
static float s_uv[4];           // the image's area in it (last blit)
static uint8_t *s_done;         // our copy of s_src is ready for this frame's blit
static SceGxmRenderTarget *s_rt;
static SceGxmColorSurface s_surf;
static SceGxmTexture s_out;     // s_src's descriptor, our data
static unsigned int s_tw, s_th, s_tstride;
static SceGxmTextureFormat s_tfmt;
static volatile unsigned int d_passes, d_swaps, d_one, d_bad;
// why the last pass didn't run (diag): step, error, and the source texture
enum { W_OK, W_NOTEX, W_FORMAT, W_TYPE, W_SIZE, W_ALLOC, W_MAP, W_SURFACE, W_RTSIZE, W_RTALLOC, W_RT,
       W_AREA, W_BEGIN, W_UNIFORMS };
static int s_why, s_why_err, s_target_fail;
static unsigned int s_src_fmt, s_src_type, s_src_w, s_src_h;

static int why(int step, int err) {
    s_why = step;
    s_why_err = err;
    d_bad++;
    return 0;
}

/* ---- memory and programs ---------------------------------------------- */

static int block_alloc(block_t *b, const char *name, SceKernelMemBlockType type, unsigned int size,
        unsigned int align) {
    b->size = ALIGN(size, align);
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

static int gpu_map(int i, SceGxmMemoryAttribFlags attr) {
    int r = sceGxmMapMemory(s_b[i].base, s_b[i].size, attr);
    s_mapped[i] = r >= 0;
    return r;
}

// uncached main memory the GPU reads
static int gpu_alloc(int i, const char *name, unsigned int size) {
    int r = block_alloc(&s_b[i], name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, 4096);
    return r < 0 ? r : gpu_map(i, SCE_GXM_MEMORY_ATTRIB_READ);
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
    if ((r = block_alloc(&s_b[B_PROGS], "divasplit_sh_progs", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 16 * 1024, 4096)) < 0)
        return r;
    unsigned int off = 0;
    s_vprog = load_gxp("blit_v.gxp", &off);
    s_fprog = load_gxp("sharpen_f.gxp", &off);
    if (!s_vprog || !s_fprog)
        return -3;   // not compiled (no libshacccg.suprx when installing)
    return 1;
}

// on the first use (GXM is initialized by the game after our module_start:
// mapping memory or making a patcher before that fails with 0x805B0000)
static int gpu_setup(void) {
    int r;
    if ((r = block_alloc(&s_b[B_HOST], "divasplit_sh_host", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * 1024, 4096)) < 0)
        return r;
    if (!(s_host = sceClibMspaceCreate(s_b[B_HOST].base, s_b[B_HOST].size)))
        return -2;
    if ((r = gpu_alloc(B_PATCH, "divasplit_sh_patch", 64 * 1024)) < 0)
        return r;
    unsigned int voff, foff;
    if ((r = block_alloc(&s_b[B_VUSSE], "divasplit_sh_vusse", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 16 * 1024, 4096)) < 0
            || (r = sceGxmMapVertexUsseMemory(s_b[B_VUSSE].base, s_b[B_VUSSE].size, &voff)) < 0)
        return r;
    s_vusse_map = 1;
    if ((r = block_alloc(&s_b[B_FUSSE], "divasplit_sh_fusse", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 16 * 1024, 4096)) < 0
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
    if (!pos || !t || !s_u_rect || !s_u_k || !s_u_lim)
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
    if ((r = sceGxmShaderPatcherCreateFragmentProgram(s_patcher, s_fid, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
            SCE_GXM_MULTISAMPLE_NONE, NULL, s_vprog, &s_fp)) < 0)
        return r;

    // the quad as a strip: clip-space corner, t (top-left = 0,0 = the image's (u0, v0))
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

// our texture and render target like src: made once (the game never reallocates
// FBO 11); a failure is final (no allocating again every frame)
static int make_target(const SceGxmTexture *src) {
    unsigned int w = s_src_w, h = s_src_h;
    SceGxmTextureFormat fmt = (SceGxmTextureFormat)s_src_fmt;
    SceGxmTextureType type = (SceGxmTextureType)s_src_type;
    SceGxmColorFormat cf;
    if (fmt == SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR)
        cf = SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR;
    else if (fmt == SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ARGB)
        cf = SCE_GXM_COLOR_FORMAT_U8U8U8U8_ARGB;
    else
        return why(W_FORMAT, (int)fmt);
    if (type != SCE_GXM_TEXTURE_LINEAR && type != SCE_GXM_TEXTURE_LINEAR_STRIDED)
        return why(W_TYPE, (int)type);
    if (w < 16 || h < 16 || w > 2048 || h > 2048)
        return why(W_SIZE, (int)(w << 16 | h));
    unsigned int stride = type == SCE_GXM_TEXTURE_LINEAR_STRIDED ? sceGxmTextureGetStride(src) / 4 : ALIGN(w, 8);
    // video memory if there is any left, else main memory
    unsigned int size = stride * h * 4;
    int r = block_alloc(&s_b[B_COLOR], "divasplit_sh_color", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, 256 * 1024);
    if (r < 0 && (r = block_alloc(&s_b[B_COLOR], "divasplit_sh_color", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
            size, 4096)) < 0)
        return why(W_ALLOC, r);
    if ((r = gpu_map(B_COLOR, SCE_GXM_MEMORY_ATTRIB_RW)) < 0)
        return why(W_MAP, r);
    if ((r = sceGxmColorSurfaceInit(&s_surf, cf, SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
            SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, w, h, stride, s_b[B_COLOR].base)) < 0)
        return why(W_SURFACE, r);
    // the driver's memory as the game makes it (sceGxmGetRenderTargetMemSize, then a block)
    SceGxmRenderTargetParams rp;
    sceClibMemset(&rp, 0, sizeof(rp));
    rp.width = (uint16_t)w;
    rp.height = (uint16_t)h;
    rp.scenesPerFrame = 1;
    rp.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    unsigned int dsize = 0;
    if ((r = sceGxmGetRenderTargetMemSize(&rp, &dsize)) < 0)
        return why(W_RTSIZE, r);
    if ((r = block_alloc(&s_b[B_RTMEM], "divasplit_sh_rt", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, dsize, 4096)) < 0)
        return why(W_RTALLOC, r);
    rp.driverMemBlock = s_b[B_RTMEM].uid;
    if ((r = sceGxmCreateRenderTarget(&rp, &s_rt)) < 0) {
        s_rt = NULL;
        return why(W_RT, r);
    }
    s_tw = w;
    s_th = h;
    s_tfmt = fmt;
    s_tstride = stride;
    return 1;
}

static int ensure_target(const SceGxmTexture *src) {
    s_src_w = sceGxmTextureGetWidth(src);
    s_src_h = sceGxmTextureGetHeight(src);
    s_src_fmt = (unsigned int)sceGxmTextureGetFormat(src);
    s_src_type = (unsigned int)sceGxmTextureGetType(src);
    if (s_rt)   // the texture we were made for?
        return (s_src_w == s_tw && s_src_h == s_th && s_src_fmt == (unsigned int)s_tfmt) || why(W_SIZE, -1);
    if (s_target_fail)
        return why(s_why, s_why_err);
    if (make_target(src))
        return 1;
    s_target_fail = 1;
    // give back what was made
    if (s_mapped[B_COLOR])
        sceGxmUnmapMemory(s_b[B_COLOR].base);
    s_mapped[B_COLOR] = 0;
    block_free(&s_b[B_COLOR]);
    block_free(&s_b[B_RTMEM]);
    return 0;
}

/* ---- the pass and the blit ---------------------------------------------- */

static const SceGxmTexture *engine_tex(const uint8_t *obj) {
    if (!obj || !(*(const volatile uint32_t *)obj & 2) || !*(void *const volatile *)(obj + 0x30))
        return NULL;
    return (const SceGxmTexture *)(obj + 8);
}

// the sharpened copy of the image the last blit drew from: returns 1 if made
static int pass(SceGxmContext *ctx) {
    const SceGxmTexture *src = engine_tex(s_src);
    if (!src)
        return why(W_NOTEX, 0);
    if (!ensure_target(src))
        return 0;
    float u0 = s_uv[0], v0 = s_uv[1], u1 = s_uv[2], v1 = s_uv[3];
    float tw = (float)s_tw, th = (float)s_th;
    int x1 = (int)((u1 > u0 ? u1 : u0) * tw + 0.5f), y1 = (int)((v1 > v0 ? v1 : v0) * th + 0.5f);
    int x0 = (int)((u1 > u0 ? u0 : u1) * tw + 0.5f), y0 = (int)((v1 > v0 ? v0 : v1) * th + 0.5f);
    if (x1 - x0 >= 959) {   // drawn 1:1 (menus): nothing to sharpen
        d_one++;
        return 0;
    }
    if (x0 < 0 || y0 < 0 || x1 > (int)s_tw || y1 > (int)s_th || x1 - x0 < 16 || y1 - y0 < 16)
        return why(W_AREA, x1 << 16 | y1);
    SceGxmValidRegion vr = {(unsigned int)x1 - 1, (unsigned int)y1 - 1};   // from (0, 0): the image's corner
    int r = sceGxmBeginScene(ctx, 0, s_rt, &vr, NULL, NULL, &s_surf, NULL);
    if (r < 0)
        return why(W_BEGIN, r);
    // the image's area, rows from the top, the same in our copy as in the source
    float w = (float)(x1 - x0), h = (float)(y1 - y0);
    sceGxmSetViewport(ctx, x0 + w * 0.5f, w * 0.5f, y0 + h * 0.5f, -h * 0.5f, 0.5f, 0.5f);
    sceGxmSetRegionClip(ctx, SCE_GXM_REGION_CLIP_OUTSIDE, x0, y0, x1 - 1, y1 - 1);
    sceGxmSetCullMode(ctx, SCE_GXM_CULL_NONE);
    sceGxmSetFrontDepthFunc(ctx, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(ctx, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetVertexProgram(ctx, s_vp);
    sceGxmSetFragmentProgram(ctx, s_fp);
    sceGxmSetFragmentTexture(ctx, 0, src);
    void *vb = NULL, *fb = NULL;
    sceGxmReserveVertexDefaultUniformBuffer(ctx, &vb);
    sceGxmReserveFragmentDefaultUniformBuffer(ctx, &fb);
    if (vb && fb) {
        float rect[4] = {x0 / tw, y0 / th, x1 / tw, y1 / th};
        float hx = 0.5f / tw, hy = 0.5f / th;
        float k[4] = {1.0f / tw, 1.0f / th, s_amount, s_over};
        float lim[4] = {rect[0] + hx, rect[1] + hy, rect[2] - hx, rect[3] - hy};
        sceGxmSetUniformDataF(vb, s_u_rect, 0, 4, rect);
        sceGxmSetUniformDataF(fb, s_u_k, 0, 4, k);
        sceGxmSetUniformDataF(fb, s_u_lim, 0, 4, lim);
        sceGxmSetVertexStream(ctx, 0, s_quad);
        sceGxmDraw(ctx, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, s_idx, 4);
    }
    sceGxmEndScene(ctx, NULL, NULL);
    if (!vb || !fb)
        return why(W_UNIFORMS, 0);
    s_why = W_OK;
    s_out = *src;   // the engine's sampling settings, our pixels
    sceGxmTextureSetData(&s_out, s_b[B_COLOR].base);
    d_passes++;
    return 1;
}

int sharpen_before_scene(void *ctx, uint32_t ret_addr, int stage) {
    if (ret_addr != s_begin_ret || stage != 4 || s_state < 1 || !s_src || s_amount <= 0.0f
            || !opt_cut_on(OPT_SHARPEN))
        return 0;
    if (s_state == 1) {   // first time: the GPU side, once
        int r = gpu_setup();
        s_state = r == 1 ? 2 : r < 0 ? r : -5;
    }
    if (s_state != 2)
        return 0;
    s_done = pass((SceGxmContext *)ctx) ? s_src : NULL;
    return s_done != NULL;
}

// seg0+0x25E208(mgr, uv[4], colour[4], z): z is a float in s0, so the next hook /
// original is called through a prototyped pointer (TAI_CONTINUE would pass a double)
typedef void (*blit_fn)(void *mgr, const float *uv, const float *col, float z);
static void blit_hook(void *mgr, const float *uv, const float *col, float z) {
    struct _tai_hook_user *hu = (struct _tai_hook_user *)s_ref;
    struct _tai_hook_user *nx = (struct _tai_hook_user *)hu->next;
    blit_fn next = (blit_fn)(nx ? nx->func : hu->old);
    uint32_t ra = (uint32_t)__builtin_return_address(0);
    if (ra != s_blit_ret || !uv) {
        next(mgr, uv, col, z);
        return;
    }
    uint8_t *obj = *(uint8_t *const volatile *)(s_g + s_game->tex_units);   // unit 0: the 3D image
    uint8_t *done = s_done;
    s_done = NULL;
    s_src = obj;
    for (int i = 0; i < 4; i++)
        s_uv[i] = uv[i];
    if (!done || done != obj || !engine_tex(obj)) {
        next(mgr, uv, col, z);
        return;
    }
    // the engine binds obj+8 during the call: our copy, then its own again
    SceGxmTexture *t = (SceGxmTexture *)(obj + 8), saved = *t;
    *t = s_out;
    next(mgr, uv, col, z);
    *t = saved;
    d_swaps++;
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
    s_begin_ret = (uint32_t)(seg0 + g->begin_ret) | 1;
    s_hook = taiHookFunctionOffset(&s_ref, modid, 0, g->blit, 1, blit_hook);
    if (s_hook < 0)
        return s_state = s_hook;
    return s_state;
}

void sharpen_shutdown(void) {
    if (s_hook >= 0)
        taiHookRelease(s_hook, s_ref);
    s_hook = -1;
    s_src = s_done = NULL;
    if (s_rt)
        sceGxmDestroyRenderTarget(s_rt);
    s_rt = NULL;
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
    // 0..100 -> unsharp amount 0..2
    s_amount = (percent < 0 ? 0 : percent > 100 ? 100 : percent) / 50.0f;
}

int sharpen_diag(char *buf, int size) {
    return sceClibSnprintf(buf, size, "sharpen: state %d hook %08X amount %d%%; passes %u swaps %u 1:1 %u bad %u"
            " (step %d err %08X; image %ux%u fmt %08X type %08X)",
            s_state, s_hook, (int)(s_amount * 50.0f + 0.5f), d_passes, d_swaps, d_one, d_bad,
            s_why, (unsigned)s_why_err, s_src_w, s_src_h, s_src_fmt, s_src_type);
}

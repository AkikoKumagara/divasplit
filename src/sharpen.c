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
 *     unit 0, SceGxmTexture at +8). The image's area is this frame's: FBO 11's
 *     logical size in the render manager and its offset in the FBO table (the
 *     blit's UVs come from the same; taking the last blit's area made a ghost of
 *     stale pixels for a frame whenever the size grew).
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
 * Below the threshold (640 wide: dynamic resolution lowered it) the image is
 * sharpened, adaptive, shaders/cas_f.cg (AMD CAS's weighting: less on edges
 * that already have contrast, so stair-steps aren't hardened), or classic,
 * shaders/sharpen_f.cg (a clamped unsharp mask; also without cas_f). The style
 * adaptive + AA smooths it first, shaders/fxaa_f.cg (FXAA's console variant,
 * into our first buffer; CAS then reads that into a second, and AMD's order:
 * CAS after AA). The 3D is 4x MSAA already; FXAA is for the stretch of a low
 * resolution (alpha-cut edges and the like). At the threshold and up the image
 * is left alone. (Two adaptive passes were tried: "deep fried".)
 *
 * Our programs: the .gxp files in ux0:data/divasplit/shaders/ when the installer compiled
 * them there (ur0:data/libshacccg.suprx), else the copies built in
 * (src/shaders_gxp.h, compiled the same way, tools/gxp2h.py).
 */
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/clib.h>
#include <psp2/io/fcntl.h>
#include <psp2/gxm.h>
#include <taihen.h>

#include "opt.h"
#include "settings.h"
#include "sharpen.h"
#include "shaders_gxp.h"

#define SH_DIR SETTINGS_DIR "/shaders"
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

typedef struct {
    SceUID uid;
    void *base;
    unsigned int size;
} block_t;

enum { B_HOST, B_PATCH, B_VUSSE, B_FUSSE, B_PROGS, B_QUAD, B_COLOR, B_RTMEM, B_COLOR2, B_COUNT };
static block_t s_b[B_COUNT];
static int s_mapped[B_COUNT], s_vusse_map, s_fusse_map;
static SceClibMspace s_host;
static SceGxmShaderPatcher *s_patcher;
enum { P_CLASSIC, P_ADAPTIVE, P_FXAA, P_COUNT };   // the fragment programs
static const char *const P_FILE[P_COUNT] = {"sharpen_f.gxp", "cas_f.gxp", "fxaa_f.gxp"};
static const char *const P_NAME[P_COUNT] = {"classic", "adaptive", "fxaa"};
static const SceGxmProgram *s_vprog, *s_fprog[P_COUNT];
static SceGxmShaderPatcherId s_vid, s_fid[P_COUNT];
static SceGxmVertexProgram *s_vp;
static SceGxmFragmentProgram *s_fp[P_COUNT];
static const SceGxmProgramParameter *s_u_rect, *s_u_k[P_COUNT], *s_u_lim[P_COUNT];
// the setting (below the threshold): adaptive or classic sharpening, or adaptive + AA
// (FXAA, then adaptive sharpening)
enum { STYLE_ADAPTIVE, STYLE_CLASSIC, STYLE_ADAPTIVE_AA };
static const char *const STYLE_NAME[3] = {"adaptive", "classic", "adaptive + AA"};
static volatile int s_style = STYLE_ADAPTIVE;
static volatile unsigned int d_aa;   // FXAA passes
static int s_used = -1;                     // the program of the last pass
static const float *s_quad;
static const uint16_t *s_idx;
static int s_state;   // 1 programs loaded and hooked, 2 GPU side ready too, < 0 why not

static const sharpen_game_t *s_game;
static uint8_t *s_g;
static uintptr_t s_seg0, s_seg1;
static uint32_t s_blit_ret, s_begin_ret;
static float s_amount = 1.5f, s_over = 0.25f;
static float s_last_amount;     // with the stretch boost, the last pass's (diag)
static int s_below = 640;       // sharpen only images narrower than this (a mild stretch doesn't need it)
static int s_req_below;         // opt.txt's (0: the game's default)
static volatile int s_enabled = 1;   // the setting
static tai_hook_ref_t s_ref;
static SceUID s_hook = -1;

static uint8_t *s_src;          // the engine's texture object of the 3D image (last blit)
static float s_uv[4];           // the image's area in it (last blit)
static uint8_t *s_done;         // our copy of s_src is ready for this frame's blit
static SceGxmRenderTarget *s_rt;
static SceGxmColorSurface s_surf, s_surf2;   // our copy; the second pass's (adaptive + AA)
static SceGxmColorFormat s_cf;
static int s_second_fail;                    // no second buffer: no AA
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

static int s_builtin;   // programs taken from the plugin (no files in SH_DIR)

// a compiled program from SH_DIR, else the built-in copy, into s_b[B_PROGS] at *off
static const SceGxmProgram *load_gxp(const char *name, const uint8_t *builtin, unsigned int len,
        unsigned int *off) {
    char path[128];
    sceClibSnprintf(path, sizeof(path), SH_DIR "/%s", name);
    uint8_t *dst = (uint8_t *)s_b[B_PROGS].base + *off;
    int n = -1;
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        n = sceIoRead(fd, dst, s_b[B_PROGS].size - *off);
        sceIoClose(fd);
        if (n > 0 && sceGxmProgramCheck((const SceGxmProgram *)dst) < 0)
            n = -1;
    }
    if (n <= 0 && builtin && len <= s_b[B_PROGS].size - *off) {
        sceClibMemcpy(dst, builtin, len);
        n = (int)len;
        s_builtin++;
    }
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
    s_vprog = load_gxp("blit_v.gxp", GXP_BLIT_V, sizeof(GXP_BLIT_V), &off);
    s_fprog[P_CLASSIC] = load_gxp(P_FILE[P_CLASSIC], GXP_SHARPEN_F, sizeof(GXP_SHARPEN_F), &off);
#ifdef HAVE_GXP_CAS_F
    s_fprog[P_ADAPTIVE] = load_gxp(P_FILE[P_ADAPTIVE], GXP_CAS_F, sizeof(GXP_CAS_F), &off);
#else
    s_fprog[P_ADAPTIVE] = load_gxp(P_FILE[P_ADAPTIVE], NULL, 0, &off);   // only compiled by the installer yet
#endif
#ifdef HAVE_GXP_FXAA_F
    s_fprog[P_FXAA] = load_gxp(P_FILE[P_FXAA], GXP_FXAA_F, sizeof(GXP_FXAA_F), &off);
#else
    s_fprog[P_FXAA] = load_gxp(P_FILE[P_FXAA], NULL, 0, &off);
#endif
    if (!s_vprog || !s_fprog[P_CLASSIC])
        return -3;
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
            || (r = sceGxmShaderPatcherRegisterProgram(s_patcher, s_vprog, &s_vid)) < 0)
        return r;

    const SceGxmProgramParameter *pos = sceGxmProgramFindParameterByName(s_vprog, "position");
    const SceGxmProgramParameter *t = sceGxmProgramFindParameterByName(s_vprog, "t");
    s_u_rect = sceGxmProgramFindParameterByName(s_vprog, "rect");
    if (!pos || !t || !s_u_rect)
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
    // the styles: classic is needed, adaptive optional (classic is used without it)
    for (int i = 0; i < P_COUNT; i++) {
        if (!s_fprog[i])
            continue;
        const SceGxmProgram *fp = s_fprog[i];
        s_u_k[i] = sceGxmProgramFindParameterByName(fp, "k");
        s_u_lim[i] = sceGxmProgramFindParameterByName(fp, "lim");
        if (!s_u_k[i] || !s_u_lim[i])
            r = -4;
        else if ((r = sceGxmShaderPatcherRegisterProgram(s_patcher, fp, &s_fid[i])) >= 0
                && (r = sceGxmShaderPatcherCreateFragmentProgram(s_patcher, s_fid[i],
                        SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE, NULL, s_vprog, &s_fp[i])) >= 0)
            continue;
        s_fp[i] = NULL;
        if (i == P_CLASSIC)
            return r;
    }

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
    s_cf = cf;
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

// the second pass's buffer (adaptive + AA: FXAA's output -> CAS -> here), like the
// first, made on first use; a failure is final
static int ensure_second(void) {
    if (s_b[B_COLOR2].base)
        return 1;
    if (s_second_fail)
        return 0;
    unsigned int size = s_tstride * s_th * 4;
    int r = block_alloc(&s_b[B_COLOR2], "divasplit_sh_color2", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, 256 * 1024);
    if (r < 0)
        r = block_alloc(&s_b[B_COLOR2], "divasplit_sh_color2", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, 4096);
    if (r >= 0 && gpu_map(B_COLOR2, SCE_GXM_MEMORY_ATTRIB_RW) >= 0
            && sceGxmColorSurfaceInit(&s_surf2, s_cf, SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                    SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, s_tw, s_th, s_tstride, s_b[B_COLOR2].base) >= 0)
        return 1;
    s_second_fail = r < 0 ? r : -1;
    if (s_mapped[B_COLOR2])
        sceGxmUnmapMemory(s_b[B_COLOR2].base);
    s_mapped[B_COLOR2] = 0;
    block_free(&s_b[B_COLOR2]);
    return 0;
}

/* ---- the pass and the blit ---------------------------------------------- */

// an engine texture object (0x4E16E4, glBindTexture): word 0 bit 1 = initialized, the
// SceGxmTexture at +8; +0x30 is only "bound right now" (cleared when unbound, as FBO 11
// is by the time our pass runs, before the next frame's blit)
static const SceGxmTexture *engine_tex(const uint8_t *obj) {
    if (!obj || !(*(const volatile uint32_t *)obj & 2))
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
    // this frame's area: FBO 11's offset in its texture and its logical size
    const uint8_t *mgr = *(const uint8_t *const volatile *)(s_seg1 + s_game->d_mgr);
    if (!mgr)
        return why(W_AREA, 0);
    const volatile int *sz = (const volatile int *)(mgr + s_game->fbo11_size);
    float tw = (float)s_tw, th = (float)s_th;
    int x0 = 0, y0 = 0;
    if (s_game->fbo11) {
        const volatile int *ent = (const volatile int *)(s_seg0 + s_game->fbo11);
        x0 = ent[0x18 / 4];
        y0 = ent[0x1C / 4];
    }
    int x1 = x0 + sz[0], y1 = y0 + sz[1];
    // below the threshold: sharpened (adaptive + AA: smoothed first, then sharpened);
    // at it and up (f's stock 640, f 2nd's 720) and 1:1 (menus): left alone
    if (x1 - x0 >= 959 || x1 - x0 >= s_below || s_amount <= 0.0f) {
        d_one++;
        return 0;
    }
    int prog[2], n = 0;
    if (s_style == STYLE_ADAPTIVE_AA && s_fp[P_FXAA] && s_fp[P_ADAPTIVE] && ensure_second())
        prog[n++] = P_FXAA;
    prog[n++] = s_style != STYLE_CLASSIC && s_fp[P_ADAPTIVE] ? P_ADAPTIVE : P_CLASSIC;
    if (x0 < 0 || y0 < 0 || x1 > (int)s_tw || y1 > (int)s_th || x1 - x0 < 16 || y1 - y0 < 16)
        return why(W_AREA, x1 << 16 | y1);
    SceGxmValidRegion vr = {(unsigned int)x1 - 1, (unsigned int)y1 - 1};   // from (0, 0): the image's corner
    float w = (float)(x1 - x0), h = (float)(y1 - y0);
    // the more the image is stretched, the more it needs: the setting up to 1.5x
    // (f's stock 640, a mild stretch), x1.2 at 2x (480), at most x1.3
    float boost = 1.0f + 0.4f * (960.0f / w - 1.5f);
    boost = boost < 1.0f ? 1.0f : boost > 1.3f ? 1.3f : boost;
    float amount = s_amount * boost;
    s_last_amount = amount;
    // classic: the unsharp amount (0..2.6) and overshoot; adaptive: CAS's peak weight
    // (0..-0.2, its strongest: 1 + 4w stays > 0), the same sharpening as classic on
    // soft detail at the same setting
    float peak = -0.1f * amount < -0.2f ? -0.2f : -0.1f * amount;
    float rect[4] = {x0 / tw, y0 / th, x1 / tw, y1 / th};
    float hx = 0.5f / tw, hy = 0.5f / th;
    float lim[4] = {rect[0] + hx, rect[1] + hy, rect[2] - hx, rect[3] - hy};
    const SceGxmTexture *in = src;
    SceGxmTexture mid;   // the last pass's output: the engine's sampling settings, our pixels
    for (int i = 0; i < n; i++) {
        int p = prog[i];
        // FXAA: edges where the contrast is 1/8 of the local maximum, at least 0.05
        float k[4] = {1.0f / tw, 1.0f / th, p == P_FXAA ? 0.125f : p == P_ADAPTIVE ? peak : amount,
                p == P_FXAA ? 0.05f : p == P_ADAPTIVE ? 0.0f : s_over};
        int r = sceGxmBeginScene(ctx, 0, s_rt, &vr, NULL, NULL, i & 1 ? &s_surf2 : &s_surf, NULL);
        if (r < 0)
            return why(W_BEGIN, r);
        // the image's area, rows from the top, the same in our copies as in the source
        sceGxmSetViewport(ctx, x0 + w * 0.5f, w * 0.5f, y0 + h * 0.5f, -h * 0.5f, 0.5f, 0.5f);
        sceGxmSetRegionClip(ctx, SCE_GXM_REGION_CLIP_OUTSIDE, x0, y0, x1 - 1, y1 - 1);
        sceGxmSetCullMode(ctx, SCE_GXM_CULL_NONE);
        sceGxmSetFrontDepthFunc(ctx, SCE_GXM_DEPTH_FUNC_ALWAYS);
        sceGxmSetFrontDepthWriteEnable(ctx, SCE_GXM_DEPTH_WRITE_DISABLED);
        sceGxmSetVertexProgram(ctx, s_vp);
        sceGxmSetFragmentProgram(ctx, s_fp[p]);
        sceGxmSetFragmentTexture(ctx, 0, in);   // copies the descriptor: mid can change after
        void *vb = NULL, *fb = NULL;
        sceGxmReserveVertexDefaultUniformBuffer(ctx, &vb);
        sceGxmReserveFragmentDefaultUniformBuffer(ctx, &fb);
        if (vb && fb) {
            sceGxmSetUniformDataF(vb, s_u_rect, 0, 4, rect);
            sceGxmSetUniformDataF(fb, s_u_k[p], 0, 4, k);
            sceGxmSetUniformDataF(fb, s_u_lim[p], 0, 4, lim);
            sceGxmSetVertexStream(ctx, 0, s_quad);
            sceGxmDraw(ctx, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, s_idx, 4);
        }
        sceGxmEndScene(ctx, NULL, NULL);
        if (!vb || !fb)
            return why(W_UNIFORMS, 0);
        mid = *src;
        sceGxmTextureSetData(&mid, s_b[i & 1 ? B_COLOR2 : B_COLOR].base);
        in = &mid;
    }
    s_used = prog[n - 1];
    s_why = W_OK;
    s_out = mid;
    d_passes++;
    if (n > 1)
        d_aa++;
    return 1;
}

int sharpen_before_scene(void *ctx, uint32_t ret_addr, int stage) {
    if (ret_addr != s_begin_ret || stage != s_game->stage || s_state < 1 || !s_src || !s_enabled
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
    SceGxmTexture *t = (SceGxmTexture *)(obj + 8);
    if (s_game->bind_now) {
        // f: glBindTexture already gave GXM the engine's texture: ours for the
        // blit's draw, then the engine's again, as the engine believes it is
        SceGxmContext *ctx = *(SceGxmContext *const volatile *)(s_g + 0x3C);
        sceGxmSetFragmentTexture(ctx, 0, &s_out);
        next(mgr, uv, col, z);
        sceGxmSetFragmentTexture(ctx, 0, t);
    } else {
        // f 2nd: the engine binds obj+8 during the call: our copy, then its own again
        SceGxmTexture saved = *t;
        *t = s_out;
        next(mgr, uv, col, z);
        *t = saved;
    }
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
    sharpen_set_below(s_req_below);
    s_g = (uint8_t *)(seg1 + g->d_gl);
    s_seg0 = seg0;
    s_seg1 = seg1;
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
        for (int i = 0; i < P_COUNT; i++)
            if (s_fp[i])
                sceGxmShaderPatcherReleaseFragmentProgram(s_patcher, s_fp[i]);
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

void sharpen_set_enabled(int on) {
    s_enabled = on;
}

void sharpen_set_style(int style) {
    s_style = style == 1 ? STYLE_CLASSIC : style == 2 ? STYLE_ADAPTIVE_AA : STYLE_ADAPTIVE;
}

void sharpen_set_below(int width) {
    s_req_below = width;
    if (width <= 0)
        width = s_game && s_game->below ? s_game->below : 640;
    s_below = width < 16 ? 16 : width > 960 ? 960 : width;
}

void sharpen_set_strength(int percent) {
    // 0..100 -> unsharp amount 0..2
    s_amount = (percent < 0 ? 0 : percent > 100 ? 100 : percent) / 50.0f;
}

int sharpen_diag(char *buf, int size) {
    return sceClibSnprintf(buf, size, "sharpen: state %d hook %08X%s style %s (adaptive %s, fxaa %s, 2nd buffer %d, last %s) amount %d%% (last %d%%) below %d;"
            " passes %u (with fxaa %u) swaps %u wide %u bad %u (step %d err %08X; image %ux%u fmt %08X type %08X)",
            s_state, s_hook, s_builtin ? " built-in" : "", STYLE_NAME[s_style],
            !s_fprog[P_ADAPTIVE] ? "missing" : s_state < 2 ? "loaded" : s_fp[P_ADAPTIVE] ? "ready" : "failed",
            !s_fprog[P_FXAA] ? "missing" : s_state < 2 ? "loaded" : s_fp[P_FXAA] ? "ready" : "failed",
            s_b[B_COLOR2].base ? 1 : s_second_fail,
            s_used < 0 ? "-" : P_NAME[s_used],
            (int)(s_amount * 50.0f + 0.5f), (int)(s_last_amount * 50.0f + 0.5f), s_below, d_passes, d_aa, d_swaps,
            d_one, d_bad,
            s_why, (unsigned)s_why_err, s_src_w, s_src_h, s_src_fmt, s_src_type);
}

#ifndef DIVASPLIT_OPT_H
#define DIVASPLIT_OPT_H

#include <psp2/types.h>
#include <stdint.h>

// f 2nd main-thread cuts (opt.c): seg0 offsets, seg1 for sort0_flag
// every offset 0 = that cut doesn't exist for the game
typedef struct {
    uint32_t centre;              // sort-key centre of a skinned object (bone average)
    uint32_t mul44, inv44, xform; // the game's matrix helpers it uses
    uint32_t dirty_loop;          // dirty-flag clear loop, patched to a memset
    const uint8_t *dirty_orig;    // its original bytes: 32 (f 2nd) or 24 (f), cuts.h builds the patch
    unsigned int dirty_len;
    uint32_t list_sort;           // std::list::sort of the draw lists
    uint32_t sort0_flag;          // stage table byte: sort the opaque list (seg1 offset)
    const struct opt_r2 *r2;      // round 2 cuts (NULL: none)
} opt_cuts_t;

// a code patch that calls the plugin: the original bytes are checked, then
// head + movw/movt ip, fn + tail (blx ip; b.w back: 6 bytes) go in
typedef struct {
    uint32_t off;                 // seg0 offset (0: none)
    const uint8_t *orig;
    unsigned int norig;
    const uint8_t *head;
    unsigned int nhead;
    const uint8_t *tail;
} opt_callout_t;

// the engine's uniform system, for ureuse and matdup: G offsets unless noted
typedef struct {
    uint32_t t, f;                // the vertex and fragment uniform tables T and F
    uint32_t rec, rec_vprog, rec_entries, rec_count, rec_frag;   // program records: stride, fields
    uint32_t variant;             // the vertex program variant word (0: no variants)
    uint32_t mc;                  // matrix context stride per render context (0: one, at G)
    uint32_t mv, p, dprog, dtex;  // in it: MV / P pointers, program / texture matrix dirty bytes
    uint32_t mat_src, mat_t, mat_f, mat_arg;   // material colours: source, copies in T and F, getter's arg
    uint32_t mvp_t;               // T offset of the MVP the per-draw setup stores
    uint32_t mvp_dup;             // T offset of the duplicate the matrix upload computes
    uint32_t fenv_int;            // seg1 offset of an int the flush stores as a float at F+0x280 (0: none)
    int setdata;                  // entries {param, source float, count} for SetUniformDataF (f),
                                  // else {destination byte, source byte, bytes | flag} (f 2nd)
} opt_ulayout_t;

// round 2 cuts (opt.c "Round 2"): seg0 offsets unless named d_ (seg1 offsets)
typedef struct opt_r2 {
    uint32_t predraw;        // per-draw uniform setup: MVP, flush, vertex binding
    uint32_t predraw_ret2d;  // its return address when a 2D draw calls it
    uint32_t fbind;          // fragment uniform binding
    uint32_t flush_tex;      // fragment textures, the flush's last call
    uint32_t reserve_ret;    // return address of the vertex reserve in vbind
    opt_callout_t mat;       // the flush's ten material copies
    uint32_t mat_get;        // the material getter they call
    opt_callout_t mvp;       // the duplicate MVP in the matrix upload
    uint32_t transpose;      // 4x4 transpose (out, in)
    uint32_t palette;        // bone palette upload (bones, n)
    uint32_t texparam;       // texParameteri (target, pname, value)
    uint32_t keyset_loop;    // ROB_BASE key-set loop (RobThreads)
    uint32_t h2f;            // half -> float
    uint32_t dsc_trace;      // DSC trace-log branch (2 bytes)
    uint32_t stub_tls, stub_set_vp, stub_uaddr, stub_vaddr;   // the game's import stubs (ARM)
    uint32_t d_gl;           // the engine's GL state "G"
    uint32_t d_tls_render;   // TLS descriptor of the render-context index (0: one context)
    opt_ulayout_t u;         // the uniform system's layout
} opt_r2_t;

// which cuts OPT switches on (ux0:data/divasplit/opt.txt)
enum {
    OPT_CENTRE = 1 << 0,
    OPT_DIRTY = 1 << 1,
    OPT_LSORT = 1 << 2,
    OPT_NOSORT0 = 1 << 3,   // changes draw order: off by default
    OPT_PIN = 1 << 4,       // core pinning (threadview.c): measured no faster
    OPT_CHECK = 1 << 5,     // compare every centre / reused uniform block with the game's (else 1 in 64 / 1024)
    // round 2
    OPT_UREUSE = 1 << 6,    // 2D: reuse the previous identical vertex uniform block, skip the flush
    OPT_MATDUP = 1 << 7,    // flush: two 80-byte material copies, MVP copied instead of recomputed
    OPT_PALETTE = 1 << 8,   // bone palette upload without 3 setter calls per bone
    OPT_TEXWRAP = 1 << 9,   // texParameteri wrap modes without the generic dispatch
    OPT_KEYSET = 1 << 10,   // ROB_BASE key-set loop inlined (RobThreads)
    OPT_H2F = 1 << 11,      // half -> float replaced (patched at start, if set then)
    OPT_NOLOG = 1 << 12,    // DSC trace log skipped (patched at start, if set then)
    OPT_SHARPEN = 1 << 13,  // sharpening of the 3D image as it is stretched to the screen (sharpen.c)
    OPT_DRS = 1 << 14,      // dynamic resolution of the 3D world (drs.c)
    OPT_REGION = 1 << 15,   // valid regions for the 3D / tonemap / stage-2 scenes (drs.c)
};
#define OPT_DEFAULT (OPT_CENTRE | OPT_DIRTY | OPT_LSORT | OPT_UREUSE | OPT_MATDUP | OPT_PALETTE \
        | OPT_TEXWRAP | OPT_KEYSET | OPT_H2F | OPT_NOLOG | OPT_SHARPEN | OPT_DRS | OPT_REGION)

// engine optimizations (opt.c); *on switches them at run time
int opt_init(SceUID modid, uintptr_t seg0, uintptr_t seg1, uint32_t matmul_off,
        const opt_cuts_t *cuts, volatile int *on);
void opt_shutdown(void);
void opt_set_mask(unsigned int mask);
int opt_cut_on(unsigned int bit);     // OPT on for this cut (and its A/B arm): for cuts outside opt.c
void opt_set_abmask(unsigned int mask);   // cuts the A/B compares; the others stay on (0: all)
void opt_set_mode_ptr(volatile const int *mode);   // the OPT setting (2 = A/B)
unsigned int opt_mask(void);
void opt_frame(void);                 // main thread, frame start: applies patches / switches
int opt_diag(char *buf, int size);    // cumulative counters, for diag.txt
int opt_diag2(char *buf, int size, int part);   // round 2 counters (part 0..3), for diag.txt
void opt_set_scenes_tracked(int ok);  // before opt_init: Begin/EndScene call probe_scene() (ureuse needs it)
void opt_set_stage_ptr(volatile const int *stage);   // main.c's current render stage (counters per stage)

// uniform reuse (see opt.c) and its counters
typedef struct {
    uint32_t calls, bytes;      // shader binding memcpys
    uint32_t groups, group_bytes;   // blocks (one per draw) with opt on
    uint32_t same, same_bytes;  // blocks that reused the previous GPU copy
    uint32_t setdata;           // SetUniformDataF into a staged block
    uint32_t overflow;          // writes past the staging buffer (dropped; must stay 0)
} probe_stats_t;

// hooks the shader binding's memcpy / reserve / SetUniformDataF and sceGxmDraw;
// offsets are seg0 offsets of the return addresses of the first three calls.
// Call from module_start, after opt_init: patching these hot imports while the
// game runs crashes it.
int probe_install(uint32_t text_base, uint32_t memcpy_ret, uint32_t reserve_ret, uint32_t setdata_ret);
void probe_enable(int on);                                // keep counters (recording)
void probe_scene(void);                                   // a scene started or ended
void probe_take(probe_stats_t *out);                      // read and reset
int probe_diag(char *buf, int size);                      // cumulative self-checks, for diag.txt

#endif

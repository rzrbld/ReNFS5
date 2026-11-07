/*
 * d3d7.c - guest-visible Direct3D7 COM facade for NFS5's renderer plugin
 * Drivers/dx7z.dll, backed by the GLES2 fixed-function emulation in
 * runtime/render/.
 *
 * Scope split with win32hle: DirectDraw7 and its surfaces are implemented by
 * hle-eng in third_party/pcrecomp/runtime/win32hle/ddraw.c (software guest
 * memory surfaces, palettes, flipping), with GPU ownership hooks into this
 * module.  dx7z creates its IDirectDraw7 through DirectDrawCreateEx, asks it for
 * an IDirect3D7 with QueryInterface, creates the 3D device, and hands
 * IDirectDrawSurface7 back buffers, depth buffers and textures to the device.
 * This file owns everything from IDirect3D7 down. DirectDraw hooks delegate
 * GPU fills, flips, and explicit CPU pixel access to this backend.  Nothing is registered as a
 * Win32 import shim; the objects are built lazily on first QI.
 *
 * Guest objects live in guest memory (hle_alloc via hle_com_new): [vtable VA]
 * [refcount] [kind] [host index].  The guest never sees a host pointer.  COM
 * methods are entered as stdcall shims (win32hle.h: A32/RET), dispatch on the
 * per-interface vtable built by hle_com_vtable from the *_shims[] tables below,
 * which enumerate the public DirectX 7 ABI in order (see d3d7_abi.h).
 *
 * Evidence used to prioritise the implementation (analyst, verified from the
 * lifted dx7z + Porsche.exe):
 *   - Geometry arrives as D3DTLVERTEX only: FVF 0x1C4 (stride 32) and
 *     0x2C4 (stride 40, two texture coords), never lit XYZ.
 *   - The single flush path is IDirect3DDevice7::DrawIndexedPrimitive.
 *   - The device methods dx7z calls are Clear, SetViewport, SetMaterial,
 *     SetRenderState, SetTexture, SetTextureStageState, DrawIndexedPrimitive;
 *     there is no BeginScene/EndScene and no state-block use.
 *   - Presenting is IDirectDrawSurface7::Flip (fullscreen) or Blt (windowed),
 *     ddraw delegates fullscreen flips to GPU target exchange and direct SDL
 *     presentation. CPU reads and software copies resolve pixels through
 *     d3d7_flush_render_target(); ordinary flips do not read pixels back.
 * Everything else is present as a real method or an explicit, logged,
 * counted stub: a "success without doing anything" prints once and appears in
 * the unimplemented table of NFS5_STATS=1 (d3d7_stats_report()).
 *
 * Environment:
 *   NFS5_TRACE_D3D7=1       one stderr line per COM call (+ counts at exit)
 *   NFS5_STATS=1            draw/upload/readback/frame-time report every
 *                           NFS5_STATS_FRAMES frames (default 120)
 *   NFS5_SHOTS=1            PNG of every Nth presented frame to
 *                           build/shots/frame_%05u.png
 *   NFS5_SHOTS_EVERY=N      capture interval (default 60)
 *   NFS5_TEX_REUPLOAD=1     re-upload every used texture once per frame
 *                           (diagnostic for stale-texture bugs)
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

#include "win32hle.h"
#include "d3d7.h"

#include "d3d7_abi.h"
#include "render/renderer.h"

/* ---- HRESULTs/errors ---------------------------------------------------- */
#define S_OK 0u
#define S_FALSE 1u
#define E_NOINTERFACE 0x80004002u
#define E_FAIL 0x80004005u
#define E_NOTIMPL 0x80004001u
#define E_INVALIDARG 0x80070057u
#define D3DERR_INVALIDCALL 0x8876086cu
#define D3DERR_NOTFOUND 0x8876086du
#define D3DERR_INVALIDPARAMS 0x80070057u
#define DDERR_NOTFOUND 0x887600FFu
#define DD_OK 0u

#define MAX_DEVICES 4
#define MAX_VB 64
#define MAX_TEX2 256
#define MAX_TEXCACHE 1024
#define MAX_SCRATCH_BYTES (2048u * 2048u * 4u)

/* ---- forward declarations of the win32hle API we rely on ---------------- */
extern int hle_dd_surface_pixels(uint32_t obj, uint8_t **px, int *w, int *h, int *pitch, int *bpp);
extern int hle_dd_frame(const uint8_t **px, int *w, int *h, int *pitch, int *bpp,
                        const uint32_t **palette, unsigned *seq);
extern void (*hle_dd_primary_hook)(void);
/* hle-eng adds this to ddraw.c per our agreed seam; weak so a staged build
 * still links (then we fall back to deriving masks from bpp). */
extern int hle_dd_surface_format(uint32_t obj, int *bpp, uint32_t *rmask, uint32_t *gmask,
                                 uint32_t *bmask, uint32_t *amask, int *paletted)
    __attribute__((weak));
/* Optional profiler frame boundary (runtime/profiling/nfs5_prof.c, perf-eng).
 * Weak so this links whether or not the Lead-owned Makefile includes the
 * profiler; its own branch is a no-op unless NFS5_PROFILE is set. */
extern void nfs5_frame_mark(void) __attribute__((weak));
/* perf-eng's phase/counter API (runtime/profiling/nfs5_prof.h), weak for the
 * same reason as nfs5_frame_mark above. */
extern void nfs5_acct_enter(int) __attribute__((weak));
extern void nfs5_acct_leave(int) __attribute__((weak));
static void (*nfs5_acct_enter_p)(int) = NULL;
static void (*nfs5_acct_leave_p)(int) = NULL;
static void acct_resolve(void) {
    if (!nfs5_acct_enter_p && nfs5_acct_enter) {
        nfs5_acct_enter_p = nfs5_acct_enter;
        nfs5_acct_leave_p = nfs5_acct_leave;
    }
}
extern int  nfs5_prof_phase_slot(const char *name) __attribute__((weak));
extern void nfs5_prof_phase_begin(int slot) __attribute__((weak));
extern void nfs5_prof_phase_end(int slot) __attribute__((weak));
extern int  nfs5_prof_counter_slot(const char *name) __attribute__((weak));
extern void nfs5_prof_counter_add(int slot, uint64_t v) __attribute__((weak));
static int g_phase_flush = -2, g_phase_readback = -2, g_ctr_texbytes = -2, g_ctr_readbytes = -2;
static void prof_init_slots(void) {
    if (g_phase_flush != -2) return;
    g_phase_flush = g_phase_readback = g_ctr_texbytes = g_ctr_readbytes = -1;
    if (nfs5_prof_phase_slot) {
        g_phase_flush = nfs5_prof_phase_slot("render_flush");
        g_phase_readback = nfs5_prof_phase_slot("render_readback");
        g_ctr_texbytes = nfs5_prof_counter_slot("tex_upload_bytes");
        g_ctr_readbytes = nfs5_prof_counter_slot("readback_bytes");
    }
}
static void prof_begin(int *slot, const char *name) {
    prof_init_slots();
    if (*slot == -2) *slot = nfs5_prof_phase_slot ? nfs5_prof_phase_slot(name) : -1;
    if (nfs5_prof_phase_begin && *slot >= 0) nfs5_prof_phase_begin(*slot);
}
static void prof_end(int slot) {
    if (nfs5_prof_phase_end && slot >= 0) nfs5_prof_phase_end(slot);
}
static void prof_counter(int slot, uint64_t v) {
    if (v && nfs5_prof_counter_add && slot >= 0) nfs5_prof_counter_add(slot, v);
}

/* Public vtable builder (also used by the surface-QI face below). */
void ensure_vtables_pub(void);

/* ======================================================================== */
/* diagnostics: call counters, trace, unimplemented table                   */
/* ======================================================================== */
#define NSTATS 512
typedef struct { const char *name; uint64_t count; int stub; int printed; } CallStat;
static CallStat g_stats[NSTATS];
static int g_stats_n;
static int g_trace = -1;
static int g_stats_on = -1;
static uint64_t g_call_total;
static uint64_t g_prep_ns;   /* NFS5_DRAWPROF: device_draw CPU time */
static uint64_t g_up_ns, g_up_n, g_up_bytes, g_rb_ns, g_rb_n, g_fl_ns, g_fl_n;
static inline uint64_t now_ns_d3d(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static uint64_t g_evseq;
static int trace_on(void);
static int drawtrace_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("NFS5_DRAWTRACE"); v = e && *e && strcmp(e, "0") != 0; }
    return v;
}
static long long drawtrace_max(void) {
    static long long v = -1;
    if (v < 0) { const char *e = getenv("NFS5_DRAWTRACE_MAX"); v = e ? atoll(e) : 200000; if (v <= 0) v = 200000; }
    return v;
}
static void evlog(const char *fmt, ...) {
    if (!trace_on()) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[ev %6llu] ", (unsigned long long)++g_evseq);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc(10, stderr);
}
static int g_unimplemented;

static int trace_on(void) {
    if (g_trace < 0) {
        const char *v = getenv("NFS5_TRACE_D3D7");
        g_trace = v && *v && strcmp(v, "0") != 0;
    }
    return g_trace;
}
static int stats_on(void) {
    if (g_stats_on < 0) {
        const char *v = getenv("NFS5_STATS");
        g_stats_on = v && *v && strcmp(v, "0") != 0;
    }
    return g_stats_on;
}
static CallStat *stat_for(const char *name) {
    for (int i = 0; i < g_stats_n; i++) if (g_stats[i].name == name) return &g_stats[i];
    if (g_stats_n >= NSTATS) return &g_stats[NSTATS - 1];
    CallStat *s = &g_stats[g_stats_n++];
    s->name = name;
    return s;
}
static void count_call(const char *name) {
    stat_for(name)->count++;
    g_call_total++;
}
/* Implemented method: count it, and trace its name. Extra trace text is
 * composed by the caller after this returns. */
#define ENTER(name) do { static CallStat *_cs; if (!_cs) _cs = stat_for(name); _cs->count++; g_call_total++; \
    if (trace_on()) fprintf(stderr, "[d3d7] call %s\n", name); } while (0)

/* A method that is intentionally a no-op/stub but returns success: visible in
 * the trace once, counted, and listed in the unimplemented table. */
static void stub_ok(const char *name) {
    CallStat *s = stat_for(name);
    s->count++;
    s->stub = 1;
    if (!s->printed) {
        s->printed = 1;
        g_unimplemented++;
        fprintf(stderr, "[d3d7] stub (success, no effect): %s\n", name);
    }
    g_call_total++;
}
/* A method that cannot be implemented (returns E_NOTIMPL but is never silently
 * successful). */
static void stub_fail(const char *name) {
    CallStat *s = stat_for(name);
    s->count++;
    s->stub = 2;
    if (!s->printed) {
        s->printed = 1;
        g_unimplemented++;
        fprintf(stderr, "[d3d7] stub (E_NOTIMPL): %s\n", name);
    }
    g_call_total++;
}

void d3d7_stats_report(void) {
    fprintf(stderr, "[d3d7] === Direct3D7 call table (total %llu calls) ===\n",
            (unsigned long long)g_call_total);
    for (int i = 0; i < g_stats_n; i++)
        if (g_stats[i].count)
            fprintf(stderr, "[d3d7] %-58s %8llu%s\n", g_stats[i].name,
                    (unsigned long long)g_stats[i].count,
                    g_stats[i].stub == 1 ? "  [stub-success]" : g_stats[i].stub == 2 ? "  [stub-notimpl]" : "");
    fprintf(stderr, "[d3d7] %d unimplemented method(s)\n", g_unimplemented);
    if (r_ready()) fprintf(stderr, "[d3d7] renderer: %s\n", r_context_info());
}

/* ======================================================================== */
/* guest helpers                                                            */
/* ======================================================================== */
static uint32_t new_com(uint32_t vt, int kind, int idx) {
    uint32_t o = hle_com_new(vt, 16);
    MEM32(o + 8) = (uint32_t)kind;
    MEM32(o + 12) = (uint32_t)idx;
    return o;
}
static inline uint32_t obj_kind(uint32_t o) { return MEM32(o + 8); }
static inline uint32_t obj_idx(uint32_t o) { return MEM32(o + 12); }
static inline uint32_t obj_self(void) { return A32(0); }

/* Call a guest COM method (this + nargs real arguments) from inside a shim.
 * hle_call_guest pushes args[0] first after the return address, so args[0]
 * must be `this`. */
static uint32_t guest_vcall(uint32_t self, int slot, int nargs, const uint32_t *args) {
    uint32_t a[10];
    a[0] = self;
    for (int i = 0; i < nargs && i < 9; i++) a[1 + i] = args[i];
    return hle_call_guest(MEM32(MEM32(self) + (uint32_t)slot * 4u), nargs + 1, a);
}

/* ---- guest surface accessors ------------------------------------------- */
typedef struct {
    uint8_t *px;
    int w, h, pitch, bpp;
    uint32_t mask[4];       /* r, g, b, a */
    int paletted;
} SurfInfo;

static void default_masks(int bpp, uint32_t m[4]) {
    m[0] = m[1] = m[2] = m[3] = 0;
    if (bpp == 8) { m[0] = m[1] = m[2] = 0xFF; m[3] = 0; }
    else if (bpp == 16) { m[0] = 0xF800; m[1] = 0x07E0; m[2] = 0x001F; m[3] = 0; }
    else if (bpp == 24) { m[0] = 0xFF0000; m[1] = 0x00FF00; m[2] = 0x0000FF; m[3] = 0; }
    else { m[0] = 0x00FF0000; m[1] = 0x0000FF00; m[2] = 0x000000FF; m[3] = 0xFF000000; }
}

static int surface_info(uint32_t surf, SurfInfo *si) {
    if (!surf) return 0;
    memset(si, 0, sizeof *si);
    if (!hle_dd_surface_pixels(surf, &si->px, &si->w, &si->h, &si->pitch, &si->bpp)) return 0;
    if (!si->px || si->w <= 0 || si->h <= 0 || si->pitch <= 0) return 0;
    default_masks(si->bpp, si->mask);
    if (hle_dd_surface_format) {
        int bpp = si->bpp, pal = 0;
        uint32_t r, g, b, a;
        if (hle_dd_surface_format(surf, &bpp, &r, &g, &b, &a, &pal)) {
            si->bpp = bpp;
            si->paletted = pal;
            if (r || g || b) { si->mask[0] = r; si->mask[1] = g; si->mask[2] = b; si->mask[3] = a; }
        }
    }
    si->paletted = si->paletted || si->bpp == 8;
    return 1;
}

/* Palette of an 8-bit surface through the public IDirectDrawSurface7 /
 * IDirectDrawPalette methods (no ddraw internals needed).  Falls back to a
 * 3-3-2-ish grey ramp only if the surface has no palette. */
static int surface_palette(uint32_t surf, uint32_t out[256]) {
    for (int i = 0; i < 256; i++) {
        uint32_t v = (uint32_t)i;
        out[i] = 0xFF000000u | ((v & 0xE0) << 16) | ((v & 0x1C) << 11) | ((v & 0x03) << 6);
    }
    if (!surf) return 0;
    uint32_t mark = hle_scratch_mark();
    uint32_t out_pal = hle_scratch(4);
    uint32_t args[1] = { out_pal };
    uint32_t hr = guest_vcall(surf, DDS7_GetPalette, 1, args);
    uint32_t pal = MEM32(out_pal);
    if (hr == DD_OK && pal) {
        uint32_t entries = hle_scratch(256 * 4);
        uint32_t eargs[4] = { 0, 0, 256, entries };
        hr = guest_vcall(pal, 4, 4, eargs);      /* IDirectDrawPalette::GetEntries */
        if (hr == DD_OK) {
            const uint8_t *p = (const uint8_t *)hle_ptr(entries);
            for (int i = 0; i < 256; i++)
                out[i] = 0xFF000000u | ((uint32_t)p[i * 4 + 0] << 16) |
                         ((uint32_t)p[i * 4 + 1] << 8) | (uint32_t)p[i * 4 + 2];
            hle_scratch_release(mark);
            return 1;
        }
    }
    hle_scratch_release(mark);
    return 0;
}

/* DDCOLORKEY for the source-blt key of a surface, if it has one. */
static int surface_colorkey(uint32_t surf, uint32_t *lo, uint32_t *hi) {
    if (!surf) return 0;
    uint32_t mark = hle_scratch_mark();
    uint32_t out = hle_scratch(8);
    uint32_t args[2] = { DDCKEY_SRCBLT, out };
    uint32_t hr = guest_vcall(surf, DDS7_GetColorKey, 2, args);
    int ok = 0;
    if (hr == DD_OK) {
        *lo = MEM32(out);
        *hi = MEM32(out + 4);
        ok = 1;
    }
    hle_scratch_release(mark);
    return ok;
}

/* ---- pixel format conversion ------------------------------------------- */
static inline uint32_t mask_bits(uint32_t mask) { return mask ? (uint32_t)__builtin_popcount(mask) : 0; }
static inline uint32_t extract_mask(uint32_t v, uint32_t mask) {
    if (!mask) return 255u;
    uint32_t shift = (uint32_t)__builtin_ctz(mask);
    uint32_t bits = mask_bits(mask);
    uint32_t val = (v & mask) >> shift;
    if (bits == 8) return val;
    return (val * 255u) / ((1u << bits) - 1u);
}
static inline uint32_t pack_mask(uint32_t c, uint32_t mask) {   /* c is 0..255 */
    if (!mask) return 0;
    uint32_t shift = (uint32_t)__builtin_ctz(mask);
    uint32_t bits = mask_bits(mask);
    uint32_t max = (1u << bits) - 1u;
    uint32_t v = (c * max + 127u) / 255u;
    return (v << shift) & mask;
}
static inline uint32_t guest_pixel(const SurfInfo *si, int x, int y) {
    const uint8_t *p = si->px + (size_t)y * (size_t)si->pitch + (size_t)x * (size_t)(si->bpp / 8);
    switch (si->bpp) {
    case 8: return *p;
    case 16: return *(const uint16_t *)p;
    case 24: return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    default: return *(const uint32_t *)p;
    }
}
static inline void guest_put_pixel(const SurfInfo *si, int x, int y, uint32_t v) {
    uint8_t *p = si->px + (size_t)y * (size_t)si->pitch + (size_t)x * (size_t)(si->bpp / 8);
    switch (si->bpp) {
    case 8: *p = (uint8_t)v; break;
    case 16: *(uint16_t *)p = (uint16_t)v; break;
    case 24: p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); break;
    default: *(uint32_t *)p = v; break;
    }
}

/* Scratch buffers (host side; never guest memory). */
static uint8_t *g_rgba;
static uint16_t *g_rgb565;
static size_t g_rgba_cap, g_rgb565_cap;
static int ensure_rgba(size_t bytes) {
    if (bytes <= g_rgba_cap) return 1;
    uint8_t *p = (uint8_t *)realloc(g_rgba, bytes);
    if (!p) return 0;
    g_rgba = p;
    g_rgba_cap = bytes;
    return 1;
}
static RTLVertex *g_verts;
static size_t g_verts_cap;
static int ensure_verts(size_t n) {
    if (n <= g_verts_cap) return 1;
    RTLVertex *p = (RTLVertex *)realloc(g_verts, n * sizeof *p);
    if (!p) return 0;
    g_verts = p;
    g_verts_cap = n;
    return 1;
}
static RLitVertex *g_litverts;
static size_t g_litverts_cap;
static int ensure_litverts(size_t n) {
    if (n <= g_litverts_cap) return 1;
    RLitVertex *p = (RLitVertex *)realloc(g_litverts, n * sizeof *p);
    if (!p) return 0;
    g_litverts = p;
    g_litverts_cap = n;
    return 1;
}
static uint32_t *g_palette;   /* 256 entries */

/* guest surface -> RGBA8 (rows in guest order; no flip anywhere) */
/* ---- fast conversions -------------------------------------------------
 * 640x480x565 <-> RGBA8 happens once per presented D3D7 frame in both
 * directions; the generic per-pixel mask extraction (32-bit divides) measured
 * ~2 ms/frame.  16bpp (the only DDraw mode the game uses) now goes through
 * tables: 65536-entry value -> RGBA for readback, and 256-entry per-component
 * value tables for the reverse direction.  Table construction uses the exact
 * same extract_mask()/pack_mask() maths, so results are bit-identical to the
 * generic path (verified by NFS5_CONV_SELFTEST=1). */
static uint32_t g_lut16[65536];
static uint32_t g_lut16_key[4];
static int g_lut16_ready;
static void lut16_build(const uint32_t *mask) {
    for (uint32_t v = 0; v < 65536; v++) {
        uint32_t r = extract_mask(v, mask[0]);
        uint32_t g = extract_mask(v, mask[1]);
        uint32_t b = extract_mask(v, mask[2]);
        uint32_t a = mask[3] ? extract_mask(v, mask[3]) : 255u;
        g_lut16[v] = r | (g << 8) | (b << 16) | (a << 24);
    }
    memcpy(g_lut16_key, mask, sizeof g_lut16_key);
    g_lut16_ready = 1;
}
static inline int lut16_ready_for(const uint32_t *mask) {
    return g_lut16_ready && memcmp(g_lut16_key, mask, sizeof g_lut16_key) == 0;
}
static uint16_t g_pack16[4][256];   /* per masked component, value 0..255 -> masked word */
static uint32_t g_pack16_key[4];
static int g_pack16_ready;
static void pack16_build(const uint32_t *mask) {
    for (int c = 0; c < 4; c++) {
        for (int v = 0; v < 256; v++) g_pack16[c][v] = (uint16_t)pack_mask((uint32_t)v, mask[c]);
    }
    memcpy(g_pack16_key, mask, sizeof g_pack16_key);
    g_pack16_ready = 1;
}
static inline int pack16_ready_for(const uint32_t *mask) {
    return g_pack16_ready && memcmp(g_pack16_key, mask, sizeof g_pack16_key) == 0;
}

static int convert_to_rgba(const SurfInfo *si, const uint32_t *pal, uint8_t *out) {
    size_t n = (size_t)si->w * (size_t)si->h;
    if (!ensure_rgba(n * 4)) return 0;
    uint32_t *rgba = (uint32_t *)out;
    if (si->bpp == 16) {
        if (!lut16_ready_for(si->mask)) lut16_build(si->mask);
        for (int y = 0; y < si->h; y++) {
            const uint16_t *src = (const uint16_t *)(si->px + (size_t)y * (size_t)si->pitch);
            uint32_t *dst = rgba + (size_t)y * (size_t)si->w;
            for (int x = 0; x < si->w; x++) dst[x] = g_lut16[src[x]];
        }
        return 1;
    }
    if (si->bpp == 8 && pal) {
        uint32_t pt[256];
        for (int i = 0; i < 256; i++) {
            uint32_t c = pal[i];
            pt[i] = ((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16) | ((c >> 24) & 0xFF) << 24;
        }
        for (int y = 0; y < si->h; y++) {
            const uint8_t *src = si->px + (size_t)y * (size_t)si->pitch;
            uint32_t *dst = rgba + (size_t)y * (size_t)si->w;
            for (int x = 0; x < si->w; x++) dst[x] = pt[src[x]];
        }
        return 1;
    }
    if (si->bpp == 32 && si->mask[0] == 0x00FF0000u && si->mask[1] == 0x0000FF00u &&
        si->mask[2] == 0x000000FFu && (si->mask[3] == 0 || si->mask[3] == 0xFF000000u)) {
        for (int y = 0; y < si->h; y++) {
            const uint32_t *src = (const uint32_t *)(si->px + (size_t)y * (size_t)si->pitch);
            uint32_t *dst = rgba + (size_t)y * (size_t)si->w;
            for (int x = 0; x < si->w; x++) {
                uint32_t v = src[x];
                dst[x] = ((v >> 16) & 0xFFu) | (v & 0x0000FF00u) |
                         ((v & 0x000000FFu) << 16) | (si->mask[3] ? (v & 0xFF000000u) : 0xFF000000u);
            }
        }
        return 1;
    }
    for (int y = 0; y < si->h; y++) {
        for (int x = 0; x < si->w; x++) {
            uint32_t v = guest_pixel(si, x, y);
            uint32_t r, g, b, a;
            r = extract_mask(v, si->mask[0]);
            g = extract_mask(v, si->mask[1]);
            b = extract_mask(v, si->mask[2]);
            a = si->mask[3] ? extract_mask(v, si->mask[3]) : 255u;
            rgba[(size_t)y * (size_t)si->w + (size_t)x] = r | (g << 8) | (b << 16) | (a << 24);
        }
    }
    return 1;
}

/* RGBA8 -> guest surface (rows in guest order) */
static void convert_from_rgba(const SurfInfo *si, const uint8_t *rgba_in) {
    const uint32_t *rgba = (const uint32_t *)rgba_in;
    if (si->bpp == 16) {
        if (!pack16_ready_for(si->mask)) pack16_build(si->mask);
        const uint16_t *tr = g_pack16[0], *tg = g_pack16[1], *tb = g_pack16[2], *ta = g_pack16[3];
        for (int y = 0; y < si->h; y++) {
            const uint32_t *src = rgba + (size_t)y * (size_t)si->w;
            uint16_t *dst = (uint16_t *)(si->px + (size_t)y * (size_t)si->pitch);
            for (int x = 0; x < si->w; x++) {
                uint32_t c = src[x];
                dst[x] = (uint16_t)(tr[c & 0xFF] | tg[(c >> 8) & 0xFF] | tb[(c >> 16) & 0xFF] |
                                    (si->mask[3] ? ta[c >> 24] : 0));
            }
        }
        return;
    }
    for (int y = 0; y < si->h; y++) {
        for (int x = 0; x < si->w; x++) {
            uint32_t c = rgba[(size_t)y * (size_t)si->w + (size_t)x];
            uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = c >> 24;
            uint32_t v;
            if (si->bpp == 8) {
                /* nearest palette match: 3-3-2 index approximation */
                v = ((r & 0xE0) >> 5) << 5 | ((g & 0xE0) >> 5) << 2 | (b >> 6);
            } else {
                v = pack_mask(r, si->mask[0]) | pack_mask(g, si->mask[1]) | pack_mask(b, si->mask[2]);
                if (si->mask[3]) v |= pack_mask(a, si->mask[3]);
            }
            guest_put_pixel(si, x, y, v);
        }
    }
}

/* Env-gated equivalence check: the LUT paths must be bit-identical to the
 * generic mask maths (NFS5_CONV_SELFTEST=1). */
static void conv_selftest(void) {
    static const uint32_t formats[4][4] = {
        {0x0000F800u, 0x000007E0u, 0x0000001Fu, 0},              /* RGB565 */
        {0x00007C00u, 0x000003E0u, 0x0000001Fu, 0},              /* RGB555 */
        {0x00000F00u, 0x000000F0u, 0x0000000Fu, 0x0000F000u},    /* ARGB4444 */
        {0x00007C00u, 0x000003E0u, 0x0000001Fu, 0x00008000u},    /* ARGB1555 */
    };
    int bad = 0;
    for (int f = 0; f < 4; f++) {
        lut16_build(formats[f]);
        for (uint32_t v = 0; v < 65536; v++) {
            uint32_t r = extract_mask(v, formats[f][0]);
            uint32_t g = extract_mask(v, formats[f][1]);
            uint32_t b = extract_mask(v, formats[f][2]);
            uint32_t a = formats[f][3] ? extract_mask(v, formats[f][3]) : 255u;
            uint32_t want = r | (g << 8) | (b << 16) | (a << 24);
            if (g_lut16[v] != want) bad++;
        }
        pack16_build(formats[f]);
        for (int c = 0; c < 256; c++)
            for (int comp = 0; comp < 4; comp++)
                if (g_pack16[comp][c] != (uint16_t)pack_mask((uint32_t)c, formats[f][comp])) bad++;
    }
    fprintf(stderr, "[d3d7] conv selftest: %s (%d mismatches)\n", bad ? "FAIL" : "PASS", bad);
}


/* ======================================================================== */
/* object registries                                                        */
/* ======================================================================== */
enum { K_D3D7 = 1, K_DEVICE, K_TEX2, K_VB7, K_D3D3 };
static uint32_t g_vt_d3d7, g_vt_dev, g_vt_tex2, g_vt_vb7, g_vt_d3d3;

typedef struct { uint32_t self; int used; } D3d7Obj;

/* Texture cache: one entry per DirectDraw surface used as a D3D texture. The
 * guest surface memory is authoritative; we upload when ddraw reports a write
 * (d3d7_note_surface_dirty) or when the entry is new. */
typedef struct {
    uint32_t surf;
    int used;
    RTexture rt;
    int dirty;
    int has_alpha;
    int color_key;
    uint32_t ck_lo, ck_hi;
    uint64_t uploads;
} TexCache;

typedef struct {
    uint32_t self, d3d_obj;
    int used;
    uint32_t rt_surface;
    int rt_w, rt_h, rt_bpp;
    int rt_dirty;          /* GL has content the guest surface does not */
    int rt_guest_dirty;    /* guest memory was written by ddraw (2D) */
    int rt_full_clear_pending;
    uint32_t rs[D3D7_RS_COUNT];
    uint32_t tss[8][D3D7_TSS_COUNT];
    float trans[32][16];
    float material[17];
    D3D7Light lights[8];
    uint32_t light_on;
    uint32_t viewport[6];
    uint32_t stage_surf[8];
    uint32_t clip_plane[8][4];
    D3D7ClipStatus clip;
    int in_scene;
    int frame_started;
    uint64_t frames, draws, verts;
    /* draw fast path: state is rebuilt/interned only when a setter changed it */
    uint32_t sgen, built_gen;
    int state_valid, state_id;
    unsigned state_epoch;
    RState rst;
    uint32_t rt_ok_surf; int rt_ok_w, rt_ok_h, rt_ok_hint;
} Device;
#define STATE_CHANGED(d) ((d)->sgen++)

typedef struct { uint32_t self; int used; uint32_t surf; } Tex2Obj;
typedef struct { uint32_t self; int used; uint32_t fvf; uint32_t nverts; uint8_t *data; uint32_t locked; } VB7Obj;

static D3d7Obj g_d3d7[2];
static D3d7Obj g_d3d3[2];
static Device g_dev[MAX_DEVICES];
static Tex2Obj g_tex2[MAX_TEX2];
static VB7Obj g_vb[MAX_VB];
static TexCache g_texcache[MAX_TEXCACHE];
static int g_texcache_used;

static TexCache *texcache_find(uint32_t surf);
static Device *active_device(void) {
    for (int i = 0; i < MAX_DEVICES; i++) if (g_dev[i].used) return &g_dev[i];
    return NULL;
}
static Device *device_from_guest(uint32_t o) {
    if (!o || obj_kind(o) != K_DEVICE) return NULL;
    uint32_t i = obj_idx(o);
    return i < MAX_DEVICES && g_dev[i].used ? &g_dev[i] : NULL;
}

/* ======================================================================== */
/* render target handling                                                   */
/* ======================================================================== */
static uint32_t g_frame_no;
static uint64_t g_frame_draws, g_frame_verts;
static uint64_t g_flush_count;
static double g_last_flush_ms __attribute__((unused));
static uint64_t g_total_frames;      /* all completed D3D7 frames */
static uint64_t g_d3d7_frames;       /* frames this facade wrote back */
static uint64_t g_present_frames;    /* frames ddraw presented (primary seq) */
static uint64_t g_shots;

static int shots_enabled(void) {
    const char *v = getenv("NFS5_SHOTS");
    return v && *v && strcmp(v, "0") != 0;
}
static int shots_every(void) {
    const char *v = getenv("NFS5_SHOTS_EVERY");
    int n = v ? atoi(v) : 60;
    return n > 0 ? n : 60;
}
/* Screenshot directory: NFS5_SHOTS_DIR, else $NFS5_ROOT/../shots (so the dev
 * root build/run writes build/shots), else ./build/shots.  The game chdirs
 * into its data directory, which is why this is not a plain relative path. */
static char g_shots_dir[512];
static const char *shots_dir(void) {
    if (g_shots_dir[0]) return g_shots_dir;
    const char *env = getenv("NFS5_SHOTS_DIR");
    if (env && *env) {
        snprintf(g_shots_dir, sizeof g_shots_dir, "%s", env);
    } else {
        const char *root = getenv("NFS5_ROOT");
        if (root && *root) snprintf(g_shots_dir, sizeof g_shots_dir, "%s/../shots", root);
        else snprintf(g_shots_dir, sizeof g_shots_dir, "build/shots");
    }
    return g_shots_dir;
}
static void mkdir_shots(void) {
    const char *dir = shots_dir();
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(tmp, 0777);
        *p = '/';
    }
    mkdir(tmp, 0777);
}

/* ---------------------------------------------------------- frame boundary
 * Two counters, because the game has two presentation paths:
 *  - g_d3d7_frames: frames this facade completed (GL -> guest pixels);
 *  - g_present_frames: frames DirectDraw actually presented (primary seq
 *    changed: Flip or a Blt onto the primary).  The front end presents mostly
 *    through ddraw, so stats/shots key off the latter.
 * d3d7_flush_render_target() polls the ddraw primary sequence, which also
 * dedupes against an optional direct d3d7_note_present() call from ddraw. */
static unsigned g_last_present_seq;
static void ddraw_present_tick(void);
static void present_tick(Device *d) {
    /* Frame boundary: ddraw is about to read/copy the surface (Flip/Blt/Lock),
     * so the GL frame is complete. */
    g_flush_count++;
    g_d3d7_frames++;
    if (d->frame_started) {
        r_frame_end();
        d->frame_started = 0;
    }
    ddraw_present_tick();
}

/* Called by ddraw (weak) when it flips/copies the primary, and polled from
 * d3d7_flush_render_target so it also works without that hook. */
void d3d7_note_present(void) { ddraw_present_tick(); }

static void ddraw_present_tick(void) {
    if (!hle_dd_frame) return;
    const uint8_t *px; int w, h, pitch, bpp; const uint32_t *pal; unsigned seq = 0;
    if (!hle_dd_frame(&px, &w, &h, &pitch, &bpp, &pal, &seq)) return;
    /* NFS5_FRAME_CAP is enforced by winmm on the game's frame-clock timer (the
     * timer that reschedules itself every frame), so the whole guest loop slows
     * to the chosen rate instead of only the present path. */
    int new_present = (seq != g_last_present_seq);
    if (new_present) {
        g_last_present_seq = seq; g_present_frames++;
        if (getenv("NFS5_FRAMECLOCK")) {
            static unsigned n; static uint32_t t0;
            uint32_t now = hle_ticks_ms();
            if (!t0) t0 = now;
            if (++n % 500 == 0) fprintf(stderr, "[present] %u presents %u ms -> %.2f/s\n", n, now - t0, n * 1000.0 / (now - t0));
        }
        /* One frame mark per *presented* frame: present_tick() runs per flush
         * and can run twice per presented frame, which doubled the profiler's
         * fps before this moved here. */
        if (nfs5_frame_mark) nfs5_frame_mark();
        extern void nfs5_acct_frame(void) __attribute__((weak));
        if (nfs5_acct_frame) nfs5_acct_frame();
    }
    Device *d = active_device();
    static int drawprof = -1;
    if (drawprof < 0) drawprof = getenv("NFS5_DRAWPROF") != NULL;
    if (new_present && drawprof && (g_present_frames % 30) == 0) {
        extern uint64_t g_r_exec_ns, g_r_exec_calls;
        static uint64_t l_prep, l_exec, l_execn, l_draws, l_calls, l_gl, l_binds, l_fr;
        static uint64_t l_upns, l_upn, l_upb, l_rbns, l_rbn;
        static uint32_t l_ms;
        RStats st; r_get_stats(&st);
        uint32_t now = hle_ticks_ms();
        uint64_t fr = g_present_frames - l_fr;
        if (l_fr && fr) {
            double f = (double)fr;
            fprintf(stderr, "[drawprof] %.1f fps | per frame: dx_draws %.0f gl_draws %.0f binds %.0f com_calls %.0f "
                    "| prep %.2f ms exec %.2f ms (%.1f flushes)\n",
                    fr * 1000.0 / (now - l_ms), (g_frame_draws - l_draws) / f, (st.draws - l_gl) / f,
                    (st.tex_binds - l_binds) / f, (g_call_total - l_calls) / f,
                    (g_prep_ns - l_prep) / f / 1e6, (g_r_exec_ns - l_exec) / f / 1e6, (g_r_exec_calls - l_execn) / f);
            extern uint64_t g_r_swap_ns, g_r_swaps, g_r_wait_ns, g_r_batch_ns;
            static uint64_t l_sw, l_wt, l_bt;
            fprintf(stderr, "[drawprof]   worker busy %.2f ms/f (swap %.2f ms/f) | caller waited %.2f ms/f\n",
                    (g_r_batch_ns - l_bt) / f / 1e6, (g_r_swap_ns - l_sw) / f / 1e6, (g_r_wait_ns - l_wt) / f / 1e6);
            l_sw = g_r_swap_ns; l_wt = g_r_wait_ns; l_bt = g_r_batch_ns;
            extern uint64_t g_r_texonly_breaks, g_r_state_breaks;
            static uint64_t l_tb, l_sb;
            fprintf(stderr, "[drawprof]   splits: texture-only %.0f/f state %.0f/f\n",
                    (g_r_texonly_breaks - l_tb) / f, (g_r_state_breaks - l_sb) / f);
            l_tb = g_r_texonly_breaks; l_sb = g_r_state_breaks;
            fprintf(stderr, "[drawprof]   uploads %.1f/f %.0f px/f %.2f ms/f | readback-flush %.2f/f %.2f ms/f | wall=%.1fs\n",
                    (g_up_n - l_upn) / f, (g_up_bytes - l_upb) / f, (g_up_ns - l_upns) / f / 1e6,
                    (g_rb_n - l_rbn) / f, (g_rb_ns - l_rbns) / f / 1e6, now / 1000.0);
        }
        l_upns = g_up_ns; l_upn = g_up_n; l_upb = g_up_bytes; l_rbns = g_rb_ns; l_rbn = g_rb_n;
        l_prep = g_prep_ns; l_exec = g_r_exec_ns; l_execn = g_r_exec_calls; l_draws = g_frame_draws;
        l_calls = g_call_total; l_gl = st.draws; l_binds = st.tex_binds; l_fr = g_present_frames; l_ms = now;
    }
    if (new_present && stats_on()) {
        int every = 60;
        const char *v = getenv("NFS5_STATS_FRAMES");
        if (v && atoi(v) > 0) every = atoi(v);
        if (g_present_frames == 1 || (g_present_frames % (uint64_t)every) == 0) {
            RStats st;
            r_get_stats(&st);
            static uint32_t last_report_ms;
            static uint64_t last_report_frames;
            uint32_t report_ms=hle_ticks_ms();
            uint64_t count=g_present_frames-last_report_frames;
            double avg=last_report_ms && count ? (report_ms-last_report_ms)/(double)count : 0;
            last_report_ms=report_ms;last_report_frames=g_present_frames;
            static uint32_t t_stats0;
            if (!t_stats0) t_stats0 = hle_ticks_ms();
            fprintf(stderr, "[d3d7] presented %llu (d3d7 %llu): %.2f ms/frame (%.1f fps) t=%.2fs "
                            "draws=%llu verts=%llu tex_upload=%llu B readback=%llu B rt=%dx%d\n",
                    (unsigned long long)g_present_frames, (unsigned long long)g_d3d7_frames,
                    avg, avg > 0 ? 1000.0 / avg : 0.0, (hle_ticks_ms() - t_stats0) / 1000.0,
                    (unsigned long long)g_frame_draws, (unsigned long long)g_frame_verts,
                    (unsigned long long)st.tex_upload_bytes, (unsigned long long)st.readback_bytes,
                    d ? d->rt_w : 0, d ? d->rt_h : 0);
        }
    }
    if (shots_enabled()) {
        int every = shots_every();
        static int source_guest = -1;
        if (source_guest < 0) {
            const char *v = getenv("NFS5_SHOTS_SOURCE");
            source_guest = !(v && !strcasecmp(v, "gl"));   /* default: presented guest pixels */
        }
        /* Guest shots follow presented frames; GL shots follow completed D3D7
         * frames (which also happen while the game is not presenting yet). */
        static uint64_t last_shot_clock;
        static int shot_src = -1;
        uint64_t shot_clock = source_guest ? g_present_frames : g_d3d7_frames;
        if (shot_src < 0) shot_src = source_guest;
        if (shot_src != source_guest) { shot_src = source_guest; last_shot_clock = 0; }
        if (shot_clock > 0 && shot_clock != last_shot_clock &&
            (shot_clock % (uint64_t)every) == 0) {
            last_shot_clock = shot_clock;
            int ok = 0;
            if (source_guest && w > 0 && h > 0) {
                SurfInfo si;
                memset(&si, 0, sizeof si);
                si.px = (uint8_t *)px; si.w = w; si.h = h; si.pitch = pitch; si.bpp = bpp;
                default_masks(bpp, si.mask);
                if (ensure_rgba((size_t)w * h * 4) && convert_to_rgba(&si, pal, g_rgba)) ok = 1;
                if (ok) {
                    mkdir_shots();
                    char path[700];
                    snprintf(path, sizeof path, "%s/guest_%05llu.png", shots_dir(),
                             (unsigned long long)g_shots++);
                    if (r_write_png(path, g_rgba, w, h)) {
                        fprintf(stderr, "[d3d7] screenshot %s\n", path); ok = 1;
                    } else ok = 0;
                }
            }
            if (!source_guest || !ok) {
                SurfInfo si;
                if (d && d->rt_surface && surface_info(d->rt_surface, &si) &&
                    ensure_rgba((size_t)si.w * si.h * 4) && r_read_pixels(g_rgba, si.w, si.h)) {
                    mkdir_shots();
                    char path[700];
                    snprintf(path, sizeof path, "%s/frame_%05llu.png", shots_dir(),
                             (unsigned long long)g_shots++);
                    if (r_write_png(path, g_rgba, si.w, si.h))
                        fprintf(stderr, "[d3d7] screenshot %s\n", path);
                }
            }
        }
    }
}

/* Chain the ddraw primary-change callback: screen.c and the NFS5_DD_SHOTS
 * dumper both install one, and the front end presents through ddraw without
 * touching D3D7 at all.  Wrapping it makes NFS5_STATS/NFS5_SHOTS count real
 * presented frames while preserving whoever installed the previous hook. */
static void (*g_prev_primary_hook)(void);
static void d3d7_primary_changed(void) {
    ddraw_present_tick();
    if (g_prev_primary_hook) g_prev_primary_hook();
}
static void ensure_primary_hook(void) {
    static int selftest_done;
    if (!selftest_done) {
        selftest_done = 1;
        const char *v = getenv("NFS5_CONV_SELFTEST");
        if (v && *v && strcmp(v, "0") != 0) conv_selftest();
    }
    if (hle_dd_primary_hook == d3d7_primary_changed) return;
    g_prev_primary_hook = hle_dd_primary_hook;
    hle_dd_primary_hook = d3d7_primary_changed;
}

/* Push GL frame into the guest render-target surface. Called from
 * d3d7_flush_render_target() (ddraw Lock/Blt/Flip on a surface) and when the
 * device changes render target. */
/* GL context creation is deferred to the first thread that actually renders:
 * NFS5 runs its 3D work on a render thread, not the thread that created the
 * D3D7 device, and an EGL/SDL GL context is current on one thread only.  Every
 * GL entry point below first calls this, which lazily initialises the renderer
 * on the calling thread and (re)creates the FBO at the device's render-target
 * size. */
static int renderer_ready_for(Device *d) {
    if (!r_ready()) {
        if (!r_init()) {
            static int warned;
            if (!warned) { warned = 1; fprintf(stderr, "[d3d7] renderer init failed: %s\n", r_last_error()); }
            return 0;
        }
    }
    if (!d) return 1;
    if (d->rt_ok_surf == d->rt_surface && d->rt_ok_w == d->rt_w && d->rt_ok_h == d->rt_h &&
        r_target_width() == d->rt_w && r_target_height() == d->rt_h &&
        r_target_is_565() == d->rt_ok_hint)
        return 1;                                   /* unchanged since the last check */
    if (d->rt_w > 0 && d->rt_h > 0) {
        SurfInfo rsi;
        int t565 = 0;
        if (d->rt_surface && surface_info(d->rt_surface, &rsi))
            t565 = rsi.bpp == 16 && rsi.mask[0] == 0xf800 &&
                   rsi.mask[1] == 0x07e0 && rsi.mask[2] == 0x001f;
        static int rbinfo = -1;
        if (rbinfo < 0) rbinfo = getenv("NFS5_RBINFO") != NULL;
        if (rbinfo) {
            static int once;
            if (!once) { once = 1; fprintf(stderr, "[d3d7] rt surface info ok=%d bpp=%d masks=%#x/%#x/%#x t565=%d\n",
                d->rt_surface && surface_info(d->rt_surface, &rsi), rsi.bpp,
                rsi.mask[0], rsi.mask[1], rsi.mask[2], t565); }
        }
        r_target_hint_565(t565);
        if (r_target_width() != d->rt_w || r_target_height() != d->rt_h ||
            r_target_is_565() != t565) {
            if (!r_target(d->rt_w, d->rt_h)) return 0;
        }
        d->rt_ok_surf = d->rt_surface; d->rt_ok_w = d->rt_w; d->rt_ok_h = d->rt_h;
        d->rt_ok_hint = r_target_is_565();
    }
    return 1;
}

static int device_flush_inner(Device *d);
static int device_flush(Device *d) {
    uint64_t t0 = now_ns_d3d();
    int r = device_flush_inner(d);
    if (r) { g_rb_ns += now_ns_d3d() - t0; g_rb_n++; }
    return r;
}
static int device_flush_inner(Device *d) {
    static int dbg = -1;
    if (dbg < 0) { const char *v = getenv("NFS5_FLUSHDEBUG"); dbg = v && *v && strcmp(v, "0") != 0; }
    if (dbg) {
        SurfInfo dsi;
        int ok = d ? surface_info(d->rt_surface, &dsi) : 0;
        fprintf(stderr, "[d3d7] flush? dev=%p rt=0x%08X dirty=%d guest_dirty=%d info_ok=%d %dx%d\n",
                (void *)d, d ? d->rt_surface : 0, d ? d->rt_dirty : 0, d ? d->rt_guest_dirty : 0,
                ok, ok ? dsi.w : 0, ok ? dsi.h : 0);
    }
    if (!d || !d->rt_surface || !d->rt_dirty) return 0;
    if(getenv("NFS5_RESOLVE_TRACE"))fprintf(stderr,"[resolve] target=%#x import=%s guest=%#x\n",d->rt_surface,g_cur_import?g_cur_import:"?",g_cur_func);
    if (!renderer_ready_for(d)) return 0;
    prof_begin(&g_phase_flush, "render_flush");
    SurfInfo si;
    if (!surface_info(d->rt_surface, &si)) { prof_end(g_phase_flush); return 0; }
    static int no_readback = -1;
    if (no_readback < 0) { const char *v = getenv("NFS5_NO_READBACK"); no_readback = v && *v && strcmp(v, "0") != 0; }
    if (no_readback) {
        /* Diagnostic: keep the frame in GL only.  If the presented DDraw
         * primary then shows game 2D content, the readback is what overwrites
         * it; if it is still black, the 2D/movie path is the problem. */
        d->rt_dirty = 0;
        present_tick(d);
        prof_end(g_phase_flush);
        return 1;
    }
    if (getenv("NFS5_RBINFO")) {
        static int once;
        if (!once) { once = 1; fprintf(stderr, "[d3d7] readback info: %dx%d bpp=%d pitch=%d\n", si.w, si.h, si.bpp, si.pitch); }
    }
    prof_begin(&g_phase_readback, "render_readback");
    acct_resolve();
    if (nfs5_acct_enter_p) nfs5_acct_enter_p(3);       /* ACCT_READBACK */
    int rb = 0, rb565 = 0, direct565 = 0;
    static int use565 = -1;
    if (use565 < 0) {
        const char *v = getenv("NFS5_NO_RB565");
        use565 = !(v && *v && strcmp(v, "0") != 0);
    }
    if (use565 && si.bpp == 16 && si.mask[0] == 0xf800 &&
        si.mask[1] == 0x07e0 && si.mask[2] == 0x001f) {
        /* Tightly packed guest surfaces can receive GL pixels directly. */
        size_t bytes = (size_t)si.w * (size_t)si.h * 2u;
        direct565 = si.pitch == si.w * 2;
        if (!direct565 && bytes > g_rgb565_cap) {
            uint16_t *p = realloc(g_rgb565, bytes);
            if (p) { g_rgb565 = p; g_rgb565_cap = bytes; }
        }
        if (direct565 || bytes <= g_rgb565_cap)
            rb565 = r_read_pixels_565(direct565 ? (uint16_t *)si.px : g_rgb565, si.w, si.h);
        if (rb565) rb = 1;
    }
    if (!rb565 && ensure_rgba((size_t)si.w * (size_t)si.h * 4))
        rb = r_read_pixels(g_rgba, si.w, si.h);
    if (nfs5_acct_leave_p) nfs5_acct_leave_p(3);
    prof_end(g_phase_readback);
    if (!rb) {
        fprintf(stderr, "[d3d7] readback failed: %s\n", r_last_error());
        prof_end(g_phase_flush);
        return 0;
    }
    prof_counter(g_ctr_readbytes, (uint64_t)si.w * (uint64_t)si.h * (rb565 ? 2u : 4u));
    if (rb565 && !direct565) {
        /* Packed 565 readback: row-copy into the guest 16bpp surface. */
        for (int y = 0; y < si.h; y++)
            memcpy(si.px + (size_t)y * (size_t)si.pitch, g_rgb565 + (size_t)y * (size_t)si.w,
                   (size_t)si.w * 2u);
    } else if (!rb565) {
        if (nfs5_acct_enter_p) nfs5_acct_enter_p(9);   /* ACCT_CONVERT */
        convert_from_rgba(&si, g_rgba);
        if (nfs5_acct_leave_p) nfs5_acct_leave_p(9);
    }
    prof_end(g_phase_flush);
    d->rt_dirty = 0;
    d->rt_guest_dirty = 0;
    /* Guest memory now holds the newest content: any texture cache entry for
     * this surface must be re-uploaded from it next time it is sampled. */
    { TexCache *tc = texcache_find(d->rt_surface); if (tc) tc->dirty = 1; }
    /* If the render target is the DirectDraw primary, tell the presenter. */
    {
        const uint8_t *ppx = NULL;
        int pw = 0, ph = 0, ppitch = 0, pbpp = 0;
        const uint32_t *pal = NULL;
        unsigned seq = 0;
        if (hle_dd_frame(&ppx, &pw, &ph, &ppitch, &pbpp, &pal, &seq) && ppx == si.px) {
            if (hle_dd_primary_hook) hle_dd_primary_hook();
        }
    }
    g_last_flush_ms = 0;
    present_tick(d);
    return 1;
}

static void device_sync_guest_to_gl(Device *d);

int d3d7_sync_surface(uint32_t surf) {
    Device *d=active_device();
    if(!d || d->rt_surface!=surf || !r_ready())return 0;
    r_submit();return 1;
}

/* DirectDraw colour fills of a GPU target are GPU clears, not CPU writes.
 * In particular, a full overwrite must never resolve the discarded pixels. */
int d3d7_fill_gpu(uint32_t surf, uint32_t color, int x, int y, int w, int h) {
    Device *d=active_device();
    if(!d || d->rt_surface!=surf || w<=0 || h<=0 || !renderer_ready_for(d))return 0;
    SurfInfo si;
    if(!surface_info(surf,&si) || si.bpp==8)return 0;
    int full=x==0 && y==0 && w==si.w && h==si.h;
    if(!full && d->rt_guest_dirty)device_sync_guest_to_gl(d);
    uint32_t rgba=0xff000000u | extract_mask(color,si.mask[0])<<16 |
                  extract_mask(color,si.mask[1])<<8 | extract_mask(color,si.mask[2]);
    if(full)r_clear(rgba,1,1,0);else r_clear_rect(rgba,1,1,0,x,y,w,h);
    d->rt_guest_dirty=0;d->rt_dirty=1;
    return 1;
}

static uint32_t g_gpu_front;
extern int hle_screen_gpu_available(void) __attribute__((weak));
int d3d7_flip_gpu(uint32_t front, uint32_t back) {
    Device *d = active_device();
    if (!d || d->rt_surface != back || !d->rt_dirty || d->rt_guest_dirty ||
        !hle_screen_gpu_available || !hle_screen_gpu_available() || getenv("NFS5_CPU_PRESENT")) return 0;
    SurfInfo si;
    if (!surface_info(front,&si) || si.bpp!=16 || si.w!=d->rt_w || si.h!=d->rt_h) return 0;
    if (!renderer_ready_for(d) || !r_snapshot()) return 0;
    g_gpu_front = front;
    present_tick(d);
    return 1;
}
int d3d7_present_gpu(int w, int h, int stretch, int fps) {
    if (!g_gpu_front) return 0;
    if (nfs5_acct_enter_p) nfs5_acct_enter_p(10);
    static int logged;
    if (!logged) { logged = 1; fprintf(stderr, "[screen] GPU present into %dx%d (stretch=%d)\n", w, h, stretch); }
    int shown = r_present_front(w,h,stretch,fps);
    if (nfs5_acct_leave_p) nfs5_acct_leave_p(10);
    return shown;
}

void d3d7_flush_render_target(uint32_t surf_obj) {
    if (surf_obj == g_gpu_front) {
        SurfInfo si;
        if (surface_info(surf_obj,&si) && ensure_rgba((size_t)si.w*si.h*4) && r_read_front(g_rgba,si.w,si.h))
            convert_from_rgba(&si, g_rgba);
        g_gpu_front = 0;
    }
    Device *d = active_device();
    if (!d || d->rt_surface != surf_obj) return;
    evlog("FLUSH-HOOK surf=0x%08X dirty=%d guest_dirty=%d", surf_obj, d->rt_dirty, d->rt_guest_dirty);
    device_flush(d);
}

void d3d7_note_surface_dirty(uint32_t surf_obj) {
    if (surf_obj == g_gpu_front) g_gpu_front = 0;
    {
        Device *d = active_device();
        evlog("WRITE surf=0x%08X%s tex_or_rt", surf_obj,
             (d && d->rt_surface == surf_obj) ? " RT" : "");
    }
    TexCache *tc = texcache_find(surf_obj);
    if (tc) tc->dirty = 1;
    Device *d = active_device();
    if (d && d->rt_surface == surf_obj) d->rt_guest_dirty = 1;
}

/* Guest memory wrote 2D pixels into our render target: pull them back into the
 * GL texture before more 3D draws (unless a full clear is coming, which would
 * overwrite them anyway - Clear() handles that). */
static void device_sync_guest_to_gl(Device *d) {
    if (!d->rt_guest_dirty) return;
    SurfInfo si;
    if (!surface_info(d->rt_surface, &si) || !r_ready()) { d->rt_guest_dirty = 0; return; }
    if (!ensure_rgba((size_t)si.w * (size_t)si.h * 4)) return;
    uint32_t pal[256];
    const uint32_t *palp = NULL;
    if (si.bpp == 8) { surface_palette(d->rt_surface, pal); palp = pal; }
    if (!convert_to_rgba(&si, palp, g_rgba)) return;
    r_target_load(g_rgba, si.w, si.h);
    d->rt_guest_dirty = 0;
    d->rt_dirty = 1;      /* guest content is now in GL too */
}

/* ======================================================================== */
/* texture cache                                                            */
/* ======================================================================== */
/* surface VA -> texcache index + 1 (entries are never removed) */
#define TC_HASH 4096
static uint16_t g_tc_hash[TC_HASH];
static inline unsigned tc_slot(uint32_t surf) { return (surf * 2654435761u) >> 20; }
static TexCache *texcache_find(uint32_t surf) {
    if (!surf) return NULL;
    for (unsigned k = tc_slot(surf);; k = (k + 1) & (TC_HASH - 1)) {
        unsigned v = g_tc_hash[k];
        if (!v) return NULL;
        if (g_texcache[v - 1].surf == surf && g_texcache[v - 1].used) return &g_texcache[v - 1];
    }
}
static TexCache *texcache_for(uint32_t surf, int create) {
    if (!surf) return NULL;
    TexCache *found = texcache_find(surf);
    if (found) return found;
    if (!create || g_texcache_used >= MAX_TEXCACHE) return NULL;
    unsigned k = tc_slot(surf);
    while (g_tc_hash[k]) k = (k + 1) & (TC_HASH - 1);
    g_tc_hash[k] = (uint16_t)(g_texcache_used + 1);
    TexCache *t = &g_texcache[g_texcache_used++];
    memset(t, 0, sizeof *t);
    t->surf = surf;
    t->used = 1;
    t->dirty = 1;
    return t;
}

static int texcache_upload_inner(TexCache *t);
static int texcache_upload(TexCache *t) {
    uint64_t t0 = now_ns_d3d();
    int r = texcache_upload_inner(t);
    g_up_ns += now_ns_d3d() - t0; g_up_n++;
    SurfInfo si;
    if (surface_info(t->surf, &si)) g_up_bytes += (uint64_t)si.w * si.h;
    return r;
}
static int texcache_upload_inner(TexCache *t) {
    static int dbg = -1;
    if (dbg < 0) { const char *v = getenv("NFS5_TEXTRACE"); dbg = v && *v && strcmp(v, "0") != 0; }
    SurfInfo si;
    if (!surface_info(t->surf, &si)) {
        if (dbg) fprintf(stderr, "[d3d7] texupload FAILED surface_info surf=0x%08X (MEM32(vt)=0x%08X)\n",
                         t->surf, t->surf ? MEM32(t->surf) : 0);
        return 0;
    }
    if (!ensure_rgba((size_t)si.w * (size_t)si.h * 4)) return 0;
    uint32_t pal[256];
    const uint32_t *palp = NULL;
    if (si.bpp == 8) { surface_palette(t->surf, pal); palp = pal; }
    if (getenv("NFS5_TEXTRACE")) {
        /* Raw guest pixels + format, to compare with what the game intended
         * (analyst: THRASH formats 3=555 4=565 5=888 6=8888 7=4444; startup
         * 128x128 loading textures are format 4, tight 256-byte rows). */
        unsigned w0 = 0, w1 = 0, w2 = 0, w3 = 0;
        if (si.px) {
            memcpy(&w0, si.px + 0, 4);
            memcpy(&w1, si.px + 4, 4);
            memcpy(&w2, si.px + (size_t)si.pitch, 4);
            memcpy(&w3, si.px + (size_t)(si.h / 2) * si.pitch + (size_t)(si.w / 2) * (si.bpp / 8), 4);
        }
        fprintf(stderr, "[d3d7] texupload surf=0x%08X %dx%d pitch=%d bpp=%d paletted=%d masks=%08X/%08X/%08X/%08X pal=%p px=%08X,%08X,%08X,%08X\n",
                t->surf, si.w, si.h, si.pitch, si.bpp, si.paletted,
                si.mask[0], si.mask[1], si.mask[2], si.mask[3], (void *)palp,
                w0, w1, w2, w3);
    }
    if (!convert_to_rgba(&si, palp, g_rgba)) return 0;
    if (!r_ready() && !r_init()) return 0;
    if (!t->rt.gl) t->rt = r_tex_create();
    if (!t->rt.gl) return 0;
    if (nfs5_acct_enter_p) nfs5_acct_enter_p(2);      /* ACCT_RUPLOAD */
    r_tex_upload(&t->rt, g_rgba, si.w, si.h);
    if (nfs5_acct_leave_p) nfs5_acct_leave_p(2);
    prof_counter(g_ctr_texbytes, (uint64_t)si.w * (uint64_t)si.h * 4u);
    t->has_alpha = si.mask[3] != 0;
    uint32_t lo = 0, hi = 0;
    t->color_key = surface_colorkey(t->surf, &lo, &hi);
    t->ck_lo = lo;
    t->ck_hi = hi;
    t->dirty = 0;
    t->uploads++;
    return 1;
}

/* ======================================================================== */
/* D3D state -> RState                                                      */
/* ======================================================================== */
static float as_float_bits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static void argb_float(uint32_t c, float out[4]) {
    out[0] = ((c >> 16) & 0xFF) / 255.0f;
    out[1] = ((c >> 8) & 0xFF) / 255.0f;
    out[2] = (c & 0xFF) / 255.0f;
    out[3] = ((c >> 24) & 0xFF) / 255.0f;
}

static void build_rstate(Device *d, RState *s) {
    memset(s, 0, sizeof *s);
    const uint32_t *rs = d->rs;
    s->width = (uint32_t)(d->rt_w > 0 ? d->rt_w : 640);
    s->height = (uint32_t)(d->rt_h > 0 ? d->rt_h : 480);
    s->vp_x = d->viewport[0];
    s->vp_y = d->viewport[1];
    s->vp_w = d->viewport[2] ? d->viewport[2] : s->width;
    s->vp_h = d->viewport[3] ? d->viewport[3] : s->height;
    s->vp_minz = as_float_bits(d->viewport[4]);
    s->vp_maxz = as_float_bits(d->viewport[5]);

    s->cull = rs[D3D7_RS_CULLMODE] ? (int)rs[D3D7_RS_CULLMODE] : D3D7_CULL_CCW;
    s->alpha_blend = rs[D3D7_RS_ALPHABLENDENABLE] != 0;
    s->src_blend = rs[D3D7_RS_SRCBLEND] ? (int)rs[D3D7_RS_SRCBLEND] : D3D7_BLEND_SRCALPHA;
    s->dst_blend = rs[D3D7_RS_DESTBLEND] ? (int)rs[D3D7_RS_DESTBLEND] : D3D7_BLEND_INVSRCALPHA;
    s->depth_test = (int)rs[D3D7_RS_ZENABLE];
    s->depth_write = rs[D3D7_RS_ZWRITEENABLE] != 0;
    s->depth_func = rs[D3D7_RS_ZFUNC] ? (int)rs[D3D7_RS_ZFUNC] : D3D7_CMP_LESSEQUAL;
    s->alpha_test = rs[D3D7_RS_ALPHATESTENABLE] != 0;
    s->alpha_func = rs[D3D7_RS_ALPHAFUNC] ? (int)rs[D3D7_RS_ALPHAFUNC] : D3D7_CMP_ALWAYS;
    s->alpha_ref = (int)(rs[D3D7_RS_ALPHAREF] & 0xFF);
    s->lastpixel = rs[D3D7_RS_LASTPIXEL] != 0;
    s->zbias = (int)rs[D3D7_RS_ZBIAS];
    s->shade = rs[D3D7_RS_SHADEMODE] ? (int)rs[D3D7_RS_SHADEMODE] : D3D7_SHADE_GOURAUD;
    s->specular_enable = rs[D3D7_RS_SPECULARENABLE] != 0;
    s->colorkey_enable = rs[D3D7_RS_COLORKEYENABLE] != 0;
    s->fog = rs[D3D7_RS_FOGENABLE] != 0;
    uint32_t fogtable = rs[D3D7_RS_FOGTABLEMODE];
    uint32_t fogvertex = rs[D3D7_RS_FOGVERTEXMODE];
    s->fog_mode = (int)(fogtable ? fogtable : fogvertex);
    if (s->fog && s->fog_mode == 0) s->fog_mode = D3D7_FOG_LINEAR;
    s->fog_vertex = fogvertex != 0;
    s->fog_color = rs[D3D7_RS_FOGCOLOR];
    s->fog_start = as_float_bits(rs[D3D7_RS_FOGSTART]);
    s->fog_end = as_float_bits(rs[D3D7_RS_FOGEND]);
    s->fog_density = as_float_bits(rs[D3D7_RS_FOGDENSITY]);
    s->lighting = rs[D3D7_RS_LIGHTING] != 0;
    s->vertex_color_source = (int)rs[D3D7_RS_DIFFUSEMATERIALSOURCE];
    s->material_diffuse[0] = d->material[0];  s->material_diffuse[1] = d->material[1];
    s->material_diffuse[2] = d->material[2];  s->material_diffuse[3] = d->material[3];
    s->material_ambient[0] = d->material[4];  s->material_ambient[1] = d->material[5];
    s->material_ambient[2] = d->material[6];  s->material_ambient[3] = d->material[7];
    s->material_specular[0] = d->material[8]; s->material_specular[1] = d->material[9];
    s->material_specular[2] = d->material[10]; s->material_specular[3] = d->material[11];
    s->material_emissive[0] = d->material[12]; s->material_emissive[1] = d->material[13];
    s->material_emissive[2] = d->material[14]; s->material_emissive[3] = d->material[15];
    s->material_power = d->material[16];
    argb_float(rs[D3D7_RS_AMBIENT], s->global_ambient);
    s->light_on = d->light_on;
    int n = 0;
    for (int i = 0; i < 8; i++) {
        if (!(d->light_on & (1u << i))) continue;
        const D3D7Light *l = &d->lights[i];
        RLight *rl = &s->lights[n++];
        rl->type = l->type;
        rl->diffuse[0] = l->diffuse.r; rl->diffuse[1] = l->diffuse.g;
        rl->diffuse[2] = l->diffuse.b; rl->diffuse[3] = l->diffuse.a;
        rl->specular[0] = l->specular.r; rl->specular[1] = l->specular.g;
        rl->specular[2] = l->specular.b; rl->specular[3] = l->specular.a;
        rl->ambient[0] = l->ambient.r; rl->ambient[1] = l->ambient.g;
        rl->ambient[2] = l->ambient.b; rl->ambient[3] = l->ambient.a;
        rl->position[0] = l->position.x; rl->position[1] = l->position.y;
        rl->position[2] = l->position.z;
        rl->direction[0] = l->direction.x; rl->direction[1] = l->direction.y;
        rl->direction[2] = l->direction.z;
        rl->range = l->range;
        rl->falloff = l->falloff > 0.0f ? l->falloff : 1.0f;
        if (n >= 8) break;
    }
    s->light_count = n;

    for (int st = 0; st < 2; st++) {
        const uint32_t *t = d->tss[st];
        s->colorop[st] = (int)t[D3D7_TSS_COLOROP];
        s->colorarg[st][0] = (int)t[D3D7_TSS_COLORARG1];
        s->colorarg[st][1] = (int)t[D3D7_TSS_COLORARG2];
        s->alphaop[st] = (int)t[D3D7_TSS_ALPHAOP];
        s->alphaarg[st][0] = (int)t[D3D7_TSS_ALPHAARG1];
        s->alphaarg[st][1] = (int)t[D3D7_TSS_ALPHAARG2];
        int addr = (int)t[D3D7_TSS_ADDRESS];
        s->address[st][0] = t[D3D7_TSS_ADDRESSU] ? (int)t[D3D7_TSS_ADDRESSU] : addr;
        s->address[st][1] = t[D3D7_TSS_ADDRESSV] ? (int)t[D3D7_TSS_ADDRESSV] : addr;
        s->mag_filter[st] = (int)t[D3D7_TSS_MAGFILTER];
        s->min_filter[st] = (int)t[D3D7_TSS_MINFILTER];
        s->mip_filter[st] = (int)t[D3D7_TSS_MIPFILTER];
        s->texgen[st] = (t[D3D7_TSS_TEXTURETRANSFORMFLAGS] & 0xF) >= 2 ? 1 : 0;
        if (s->texgen[st]) memcpy(s->texmat[st], d->trans[16 + st], 16 * sizeof(float));
        s->stage_on[st] = (d->stage_surf[st] != 0) &&
                          (s->colorop[st] != D3D7_TOP_DISABLE || s->alphaop[st] != D3D7_TOP_DISABLE);
    }
    s->texture_factor = d->rs[D3D7_RS_TEXTUREFACTOR];
}

/* Bind the device's two texture stages, uploading dirty textures and applying
 * sampler state.  Returns the GL names to hand to the renderer. */
static void bind_stage_textures(Device *d, const RState *s, GLuint out[2]) {
    out[0] = out[1] = 0;
    static int reupload_frame = -1;
    if (reupload_frame < 0) {
        const char *v = getenv("NFS5_TEX_REUPLOAD");
        reupload_frame = v && *v && strcmp(v, "0") != 0;
    }
    for (int st = 0; st < 2; st++) {
        uint32_t surf = d->stage_surf[st];
        if (!surf || !s->stage_on[st]) continue;
        TexCache *t = texcache_for(surf, 1);
        if (!t) continue;
        if (t->dirty || reupload_frame) texcache_upload(t);
        if (t->rt.gl) {
            r_tex_params(s, st, &t->rt);
            out[st] = t->rt.gl;
        }
        evlog("bind stage=%d surf=0x%08X uploads=%llu gl=%u", st, surf,
             (unsigned long long)t->uploads, t->rt.gl);
    }
    /* colour key comes from stage 0's surface */
    if (s->colorkey_enable) {
        TexCache *t = d->stage_surf[0] ? texcache_for(d->stage_surf[0], 0) : NULL;
        if (t && t->color_key) {
            /* convert the packed key to normalized RGBA using the surface's
               masks (best effort; both ends come from the same ddraw surface) */
            RState *mutable = (RState *)s;
            SurfInfo si;
            if (surface_info(d->stage_surf[0], &si)) {
                uint32_t lo = t->ck_lo, hi = t->ck_hi;
                mutable->colorkey_valid = 1;
                mutable->colorkey_lo[0] = extract_mask(lo, si.mask[0]) / 255.0f;
                mutable->colorkey_lo[1] = extract_mask(lo, si.mask[1]) / 255.0f;
                mutable->colorkey_lo[2] = extract_mask(lo, si.mask[2]) / 255.0f;
                mutable->colorkey_lo[3] = 1.0f;
                mutable->colorkey_hi[0] = extract_mask(hi, si.mask[0]) / 255.0f;
                mutable->colorkey_hi[1] = extract_mask(hi, si.mask[1]) / 255.0f;
                mutable->colorkey_hi[2] = extract_mask(hi, si.mask[2]) / 255.0f;
                mutable->colorkey_hi[3] = 1.0f;
            }
        }
    }
}

/* ======================================================================== */
/* vertex decoding and the draw path                                        */
/* ======================================================================== */
static void decode_color(uint32_t c, float out[4]) { argb_float(c, out); }

static int decode_tl_vertices(const uint8_t *base, uint32_t stride, uint32_t fvf,
                              uint32_t count, RTLVertex *out) {
    uint32_t ntex = (fvf >> D3D7_FVF_TEXCOUNT_SHIFT) & 0xF;
    uint32_t off = 16;
    if (fvf & D3D7_FVF_RESERVED1) off += 4;
    uint32_t color_off = 0, spec_off = 0, tex_off[2] = { 0, 0 }, tex_floats[2] = { 0, 0 };
    if (fvf & D3D7_FVF_DIFFUSE) { color_off = off; off += 4; }
    if (fvf & D3D7_FVF_SPECULAR) { spec_off = off; off += 4; }
    for (uint32_t i = 0; i < ntex && i < 2; i++) {
        tex_off[i] = off;
        tex_floats[i] = d3d7_fvf_coord_floats(fvf, i);
        off += tex_floats[i] * 4;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *p = base + (size_t)i * stride;
        const float *pf = (const float *)p;
        RTLVertex *v = &out[i];
        v->x = pf[0]; v->y = pf[1]; v->z = pf[2]; v->rhw = pf[3];
        decode_color(color_off ? *(const uint32_t *)(p + color_off) : 0xFFFFFFFFu, v->color);
        decode_color(spec_off ? *(const uint32_t *)(p + spec_off) : 0xFFFFFFFFu, v->spec);
        v->tex0[0] = v->tex0[1] = v->tex1[0] = v->tex1[1] = 0.0f;
        for (uint32_t t = 0; t < ntex && t < 2; t++) {
            const float *tp = (const float *)(p + tex_off[t]);
            float u = tp[0];
            float vv = tex_floats[t] >= 2 ? tp[1] : 0.0f;
            if (t == 0) { v->tex0[0] = u; v->tex0[1] = vv; }
            else { v->tex1[0] = u; v->tex1[1] = vv; }
        }
    }
    return 1;
}

static int decode_lit_vertices(const uint8_t *base, uint32_t stride, uint32_t fvf,
                               uint32_t count, RLitVertex *out) {
    uint32_t ntex = (fvf >> D3D7_FVF_TEXCOUNT_SHIFT) & 0xF;
    uint32_t off = 12;
    if (fvf & D3D7_FVF_RESERVED1) off += 4;
    uint32_t normal_off = 0, color_off = 0, spec_off = 0, tex_off[2] = { 0, 0 }, tex_floats[2] = { 0, 0 };
    if (fvf & D3D7_FVF_NORMAL) { normal_off = off; off += 12; }
    if (fvf & D3D7_FVF_DIFFUSE) { color_off = off; off += 4; }
    if (fvf & D3D7_FVF_SPECULAR) { spec_off = off; off += 4; }
    for (uint32_t i = 0; i < ntex && i < 2; i++) {
        tex_off[i] = off;
        tex_floats[i] = d3d7_fvf_coord_floats(fvf, i);
        off += tex_floats[i] * 4;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *p = base + (size_t)i * stride;
        const float *pf = (const float *)p;
        RLitVertex *v = &out[i];
        v->x = pf[0]; v->y = pf[1]; v->z = pf[2];
        if (normal_off) {
            const float *np = (const float *)(p + normal_off);
            v->normal[0] = np[0]; v->normal[1] = np[1]; v->normal[2] = np[2];
        } else {
            v->normal[0] = 0; v->normal[1] = 0; v->normal[2] = 1;
        }
        decode_color(color_off ? *(const uint32_t *)(p + color_off) : 0xFFFFFFFFu, v->color);
        decode_color(spec_off ? *(const uint32_t *)(p + spec_off) : 0xFFFFFFFFu, v->spec);
        v->tex0[0] = v->tex0[1] = v->tex1[0] = v->tex1[1] = 0.0f;
        for (uint32_t t = 0; t < ntex && t < 2; t++) {
            const float *tp = (const float *)(p + tex_off[t]);
            float u = tp[0];
            float vv = tex_floats[t] >= 2 ? tp[1] : 0.0f;
            if (t == 0) { v->tex0[0] = u; v->tex0[1] = vv; }
            else { v->tex1[0] = u; v->tex1[1] = vv; }
        }
    }
    return 1;
}

/* Unsupported-FVF reporting: once per distinct FVF, never silent. */
static void unsupported_fvf(uint32_t fvf) {
    static uint32_t seen[8];
    static int nseen;
    for (int i = 0; i < nseen; i++) if (seen[i] == fvf) return;
    if (nseen < 8) seen[nseen++] = fvf;
    fprintf(stderr, "[d3d7] unsupported FVF 0x%X (stride %u): draw skipped (reported once)\n",
            fvf, d3d7_fvf_stride(fvf));
    g_unimplemented++;
}

static void device_draw_inner(Device *d, uint32_t prim, uint32_t fvf, const uint8_t *vertices,
                              uint32_t vcount, const uint16_t *indices, uint32_t icount,
                              int indexed);
static void device_draw(Device *d, uint32_t prim, uint32_t fvf, const uint8_t *vertices,
                        uint32_t vcount, const uint16_t *indices, uint32_t icount,
                        int indexed) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    device_draw_inner(d, prim, fvf, vertices, vcount, indices, icount, indexed);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    g_prep_ns += (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000000ll + (t1.tv_nsec - t0.tv_nsec));
}
static void device_draw_inner(Device *d, uint32_t prim, uint32_t fvf, const uint8_t *vertices,
                              uint32_t vcount, const uint16_t *indices, uint32_t icount,
                              int indexed) {
    if (!d || !vertices || vcount == 0) return;
    uint32_t stride = d3d7_fvf_stride(fvf);
    if (!stride) { unsupported_fvf(fvf); return; }
    if (d->rt_w <= 0 || d->rt_h <= 0) {
        /* no render target yet: D3D7's device always has one, but be forgiving */
        return;
    }
    if (!renderer_ready_for(d)) return;
    if (d->rt_guest_dirty) device_sync_guest_to_gl(d);
    static int slow = -1;
    if (slow < 0) slow = getenv("NFS5_SLOW_DRAW") != NULL || drawtrace_on();
    if ((fvf == 0x1C4 || fvf == 0x2C4) && !slow) {
        /* THRASH's two TL formats: intern state only after a setter changed
         * it, and hand the raw guest vertices to the renderer. */
        if (!d->state_valid || d->built_gen != d->sgen || d->state_epoch != r_state_epoch()) {
            build_rstate(d, &d->rst);
            d->built_gen = d->sgen;
            d->state_valid = 1;
            d->state_id = r_state_intern(&d->rst);
            d->state_epoch = r_state_epoch();
        }
        GLuint tex[2];
        int id = d->state_id;
        if (d->rst.colorkey_enable) {
            RState ck = d->rst;                 /* key colour comes from the bound texture */
            bind_stage_textures(d, &ck, tex);
            id = r_state_intern(&ck);
        } else {
            bind_stage_textures(d, &d->rst, tex);
        }
        r_set_textures(tex[0], tex[1]);
        if (!d->frame_started) { r_frame_begin(); d->frame_started = 1; }
        acct_resolve();
        if (nfs5_acct_enter_p) nfs5_acct_enter_p(1);      /* ACCT_RDRAW */
        r_draw_tlv(id, vertices, (int)stride, (int)vcount, indexed ? indices : NULL,
                   indexed ? (int)icount : 0, (int)prim);
        if (nfs5_acct_leave_p) nfs5_acct_leave_p(1);
        d->rt_dirty = 1;
        d->draws++;
        d->verts += indexed ? icount : vcount;
        g_frame_draws++;
        g_frame_verts += indexed ? icount : vcount;
        return;
    }
    RState st;
    build_rstate(d, &st);
    GLuint tex[2];
    bind_stage_textures(d, &st, tex);
    r_set_textures(tex[0], tex[1]);

    if (!d->frame_started) { r_frame_begin(); d->frame_started = 1; }
    r_set_viewport((int)st.vp_x, (int)st.vp_y, (int)st.vp_w, (int)st.vp_h, st.vp_minz, st.vp_maxz);

    uint32_t position_fvf = fvf & D3D7_FVF_POSITION_MASK;
    if (position_fvf == D3D7_FVF_XYZRHW) {
        if (!ensure_verts(vcount)) return;
        if (!decode_tl_vertices(vertices, stride, fvf, vcount, g_verts)) return;
        if (drawtrace_on()) {
            static long long dtn;
            long long maxn = drawtrace_max();
            if (dtn++ < maxn) {
                float x0 = g_verts[0].x, y0 = g_verts[0].y, x1 = x0, y1 = y0;
                for (uint32_t k = 1; k < vcount; k++) {
                    if (g_verts[k].x < x0) x0 = g_verts[k].x;
                    if (g_verts[k].y < y0) y0 = g_verts[k].y;
                    if (g_verts[k].x > x1) x1 = g_verts[k].x;
                    if (g_verts[k].y > y1) y1 = g_verts[k].y;
                }
                fprintf(stderr, "[draw] f=%llu prim=%u n=%u bbox=%.1f,%.1f-%.1f,%.1f "
                        "tex=%u op=%u aop=%u fog=%d fogcol=%06X speca=%.3f cola=%.3f "
                        "tex0s=%.3f,%.3f d=%.2f,%.2f,%.2f,%.2f base_op=%u\n",
                        (unsigned long long)g_d3d7_frames, prim, vcount, x0, y0, x1, y1,
                        tex[0] != 0, (unsigned)st.colorop[0], (unsigned)st.alphaop[0],
                        st.fog, st.fog_color & 0xFFFFFFu, g_verts[0].spec[3], g_verts[0].color[3],
                        g_verts[0].tex0[0], g_verts[0].tex0[1],
                        g_verts[0].color[0], g_verts[0].color[1], g_verts[0].color[2],
                        (unsigned)st.colorop[0]);
            }
        }
        evlog("DrawIndexed prim=%u fvf=0x%X v=%u i=%u", prim, fvf, vcount, indexed ? icount : 0);
    acct_resolve();
    if (nfs5_acct_enter_p) nfs5_acct_enter_p(1);      /* ACCT_RDRAW */
    r_draw_tl(&st, g_verts, (int)vcount, indices, indexed ? (int)icount : 0, (int)prim);
    if (nfs5_acct_leave_p) nfs5_acct_leave_p(1);
    } else {
        if (!ensure_litverts(vcount)) return;
        if (!decode_lit_vertices(vertices, stride, fvf, vcount, g_litverts)) return;
        if (nfs5_acct_enter_p) nfs5_acct_enter_p(1);
        r_draw_lit(&st, g_litverts, (int)vcount, indices, indexed ? (int)icount : 0, (int)prim,
                   d->trans[D3D7_TS_WORLD], d->trans[D3D7_TS_VIEW], d->trans[D3D7_TS_PROJECTION]);
        if (nfs5_acct_leave_p) nfs5_acct_leave_p(1);
    }
    d->rt_dirty = 1;
    d->draws++;
    d->verts += indexed ? icount : vcount;
    g_frame_draws++;
    g_frame_verts += indexed ? icount : vcount;
}

/* ======================================================================== */
/* COM method shims - IUnknown                                              */
/* ======================================================================== */
static void com_query_interface(uint32_t self, const uint8_t *expect1, const uint8_t *expect2,
                                const char *name) {
    const uint8_t *iid = (const uint8_t *)APTR(1);
    char g[48];
    hle_guid_string(iid, g);
    ENTER(name);
    if (trace_on()) fprintf(stderr, "[d3d7] %s QI %s\n", name, g);
    if (d3d7_guid_is_zero(iid) || d3d7_guid_eq(iid, D3D7_IID_IUnknown) ||
        (expect1 && d3d7_guid_eq(iid, expect1)) || (expect2 && d3d7_guid_eq(iid, expect2))) {
        if (A32(2)) MEM32(A32(2)) = self;
        MEM32(self + 4)++;
        RET(S_OK, 3);
    }
    if (A32(2)) MEM32(A32(2)) = 0;
    RET(E_NOINTERFACE, 3);
}
static void com_addref(void) {
    uint32_t self = A32(0);
    RET(++MEM32(self + 4), 1);
}
static void com_release(int kind) {
    uint32_t self = A32(0);
    uint32_t n = --MEM32(self + 4);
    if (!n) {
        uint32_t i = obj_idx(self);
        switch (kind) {
        case K_DEVICE:
            if (i < MAX_DEVICES && g_dev[i].used) {
                device_flush(&g_dev[i]);
                g_dev[i].used = 0;
            }
            break;
        case K_VB7:
            if (i < MAX_VB && g_vb[i].used) {
                free(g_vb[i].data);
                g_vb[i].used = 0;
            }
            break;
        case K_TEX2:
            if (i < MAX_TEX2) g_tex2[i].used = 0;
            break;
        case K_D3D3:
            if (i < 2) g_d3d3[i].used = 0;
            break;
        default: break;
        }
        hle_com_free(self);
    }
    RET(n, 1);
}

/* ======================================================================== */
/* IDirect3D7                                                               */
/* ======================================================================== */
static D3d7Obj *d3d7_host(void) {
    for (int i = 0; i < 2; i++) if (g_d3d7[i].used) return &g_d3d7[i];
    return NULL;
}

/* Supported texture-stage ops, as D3DTEXOPCAPS bits (1 << (op-1)). */
#define D3DT7_OPCAPS ((1u << (D3D7_TOP_DISABLE - 1)) | (1u << (D3D7_TOP_SELECTARG1 - 1)) | \
                      (1u << (D3D7_TOP_SELECTARG2 - 1)) | (1u << (D3D7_TOP_MODULATE - 1)) | \
                      (1u << (D3D7_TOP_MODULATE2X - 1)) | (1u << (D3D7_TOP_MODULATE4X - 1)) | \
                      (1u << (D3D7_TOP_ADD - 1)) | (1u << (D3D7_TOP_ADDSIGNED - 1)) | \
                      (1u << (D3D7_TOP_ADDSIGNED2X - 1)) | (1u << (D3D7_TOP_SUBTRACT - 1)) | \
                      (1u << (D3D7_TOP_ADDSMOOTH - 1)) | (1u << (D3D7_TOP_BLENDDIFFUSEALPHA - 1)) | \
                      (1u << (D3D7_TOP_BLENDTEXTUREALPHA - 1)) | (1u << (D3D7_TOP_BLENDFACTORALPHA - 1)) | \
                      (1u << (D3D7_TOP_BLENDCURRENTALPHA - 1)))

static void fill_prim_caps(D3D7PrimCaps *p, uint32_t misc) {
    memset(p, 0, sizeof *p);
    p->dwSize = (uint32_t)sizeof *p;
    p->dwMiscCaps = misc;
    p->dwRasterCaps = D3D7_PRASTERCAPS_DITHER | D3D7_PRASTERCAPS_ZTEST |
                      D3D7_PRASTERCAPS_FOGVERTEX | D3D7_PRASTERCAPS_FOGTABLE |
                      D3D7_PRASTERCAPS_MIPMAPLODBIAS | D3D7_PRASTERCAPS_ZBIAS |
                      D3D7_PRASTERCAPS_WFOG | D3D7_PRASTERCAPS_ZFOG;
    p->dwZCmpCaps = 0xFF;
    p->dwSrcBlendCaps = 0x1FFF;
    p->dwDestBlendCaps = 0x1FFF;
    p->dwAlphaCmpCaps = 0xFF;
    p->dwShadeCaps = D3D7_PSHADECAPS_COLORGOURAUDRGB | D3D7_PSHADECAPS_SPECULARGOURAUDRGB |
                     D3D7_PSHADECAPS_ALPHAGOURAUDBLEND | D3D7_PSHADECAPS_FOGGOURAUD;
    p->dwTextureCaps = D3D7_PTEXTURECAPS_PERSPECTIVE | D3D7_PTEXTURECAPS_ALPHA |
                       D3D7_PTEXTURECAPS_TRANSPARENCY;
    p->dwTextureFilterCaps = D3D7_PTFILTERCAPS_MINFPOINT | D3D7_PTFILTERCAPS_MINFLINEAR |
                             D3D7_PTFILTERCAPS_MIPFPOINT | D3D7_PTFILTERCAPS_MIPFLINEAR |
                             D3D7_PTFILTERCAPS_MAGFPOINT | D3D7_PTFILTERCAPS_MAGFLINEAR;
    p->dwTextureBlendCaps = 0x000000FFu;
    p->dwTextureAddressCaps = D3D7_PTADDRESSCAPS_WRAP | D3D7_PTADDRESSCAPS_MIRROR |
                              D3D7_PTADDRESSCAPS_CLAMP | D3D7_PTADDRESSCAPS_INDEPENDENTUV;
}

static void fill_device_desc(D3D7DeviceDesc *desc) {
    memset(desc, 0, sizeof *desc);
    desc->dwDevCaps = D3D7_DEVCAPS_FLOATTLVERTEX | D3D7_DEVCAPS_EXECUTESYSTEMMEMORY |
                      D3D7_DEVCAPS_TLVERTEXSYSTEMMEMORY | D3D7_DEVCAPS_TLVERTEXVIDEOMEMORY |
                      D3D7_DEVCAPS_TEXTURESYSTEMMEMORY | D3D7_DEVCAPS_TEXTUREVIDEOMEMORY |
                      D3D7_DEVCAPS_DRAWPRIMTLVERTEX | D3D7_DEVCAPS_CANRENDERAFTERFLIP |
                      D3D7_DEVCAPS_DRAWPRIMITIVES2 | D3D7_DEVCAPS_DRAWPRIMITIVES2EX |
                      D3D7_DEVCAPS_HWRASTERIZATION;
    uint32_t misc = D3D7_PMISCCAPS_CULLNONE | D3D7_PMISCCAPS_CULLCW | D3D7_PMISCCAPS_CULLCCW;
    fill_prim_caps(&desc->dpcLineCaps, misc);
    fill_prim_caps(&desc->dpcTriCaps, misc);
    desc->dwDeviceRenderBitDepth = D3D7_DDBD_16 | D3D7_DDBD_32;
    desc->dwDeviceZBufferBitDepth = D3D7_DDBD_16 | D3D7_DDBD_24 | D3D7_DDBD_32;
    desc->dwMinTextureWidth = 1;
    desc->dwMinTextureHeight = 1;
    desc->dwMaxTextureWidth = 1024;
    desc->dwMaxTextureHeight = 1024;
    desc->dwMaxTextureRepeat = 1024;
    desc->dwMaxTextureAspectRatio = 1024;
    desc->dwMaxAnisotropy = 1;
    desc->dvGuardBandLeft = -4096.0f;
    desc->dvGuardBandTop = -4096.0f;
    desc->dvGuardBandRight = 4096.0f;
    desc->dvGuardBandBottom = 4096.0f;
    desc->dvExtentsAdjust = 0.0f;
    desc->dwStencilCaps = 0;
    desc->dwFVFCaps = 8 | D3D7_FVFCAPS_DONOTSTRIPELEMENTS;
    desc->dwTextureOpCaps = D3DT7_OPCAPS;
    desc->wMaxTextureBlendStages = 2;
    desc->wMaxSimultaneousTextures = 2;
    desc->dwMaxActiveLights = 8;
    desc->dvMaxVertexW = 1.0e10f;
    memcpy(desc->deviceGUID, D3D7_CLSID_IDirect3DHALDevice, 16);
    desc->wMaxUserClipPlanes = 0;
    desc->wMaxVertexBlendMatrices = 0;
    desc->dwVertexProcessingCaps = D3D7_VTXPCAPS_VERTEXFOG | D3D7_VTXPCAPS_DIRECTIONALLIGHTS |
                                   D3D7_VTXPCAPS_POSITIONALLIGHTS | D3D7_VTXPCAPS_LOCALVIEWER;
}

static const char *const k_device_desc_string = "SDL2/GLES2 Direct3D7 HAL device";
static const char *const k_device_name_string = "NFS5 GLES fixed-function";

static void d3d7_EnumDevices(void) {
    const char *name = "IDirect3D7::EnumDevices";
    ENTER(name);
    if (!A32(1)) RET(E_INVALIDARG, 3);
    uint32_t mark = hle_scratch_mark();
    uint32_t descv = hle_scratch(sizeof(D3D7DeviceDesc));
    uint32_t pathv = hle_scratch(64);
    uint32_t namv = hle_scratch(64);
    fill_device_desc((D3D7DeviceDesc *)hle_ptr(descv));
    snprintf((char *)hle_ptr(pathv), 64, "%s", k_device_desc_string);
    snprintf((char *)hle_ptr(namv), 64, "%s", k_device_name_string);
    uint32_t a[4] = { pathv, namv, descv, A32(2) };
    uint32_t more = hle_call_guest(A32(1), 4, a);
    hle_scratch_release(mark);
    RET(DD_OK, 3);   /* DDENUMRET_CANCEL/OK handled by the callback; one device */
}

static void d3d7_CreateDevice(void) {
    const char *name = "IDirect3D7::CreateDevice";
    ENTER(name);
    ensure_primary_hook();
    /* Diagnostic only: NFS5_D3D7_REFUSE=1 makes device creation fail so the
     * game's non-D3D7 fallback can be exercised (used to bisect a fault). */
    {
        static int refuse = -1;
        if (refuse < 0) { const char *v = getenv("NFS5_D3D7_REFUSE"); refuse = v && *v && strcmp(v, "0") != 0; }
        if (refuse) {
            if (A32(3)) MEM32(A32(3)) = 0;
            if (trace_on()) fprintf(stderr, "[d3d7] CreateDevice refused (NFS5_D3D7_REFUSE)\n");
            RET(E_NOINTERFACE, 4);
        }
    }
    const uint8_t *clsid = (const uint8_t *)APTR(1);
    uint32_t surf = A32(2);
    uint32_t out = A32(3);
    static const uint8_t *const accepted[] = {
        D3D7_CLSID_IDirect3DHALDevice
    };
    int ok = 0;
    for (unsigned i = 0; i < sizeof accepted / sizeof *accepted; i++)
        if (d3d7_guid_eq(clsid, accepted[i])) ok = 1;
    if (!ok) {
        if (trace_on()) fprintf(stderr, "[d3d7] CreateDevice: class not served -> E_NOINTERFACE\n");
        if (out) MEM32(out) = 0;
        RET(E_NOINTERFACE, 4);
    }
    int slot = -1;
    for (int i = 0; i < MAX_DEVICES; i++) if (!g_dev[i].used) { slot = i; break; }
    if (slot < 0) { if (out) MEM32(out) = 0; RET(E_FAIL, 4); }
    Device *d = &g_dev[slot];
    memset(d, 0, sizeof *d);
    d->used = 1;
    d->rt_surface = surf;
    d->rt_w = 640;
    d->rt_h = 480;
    d->rt_bpp = 16;
    if (surf) {
        SurfInfo si;
        if (surface_info(surf, &si)) { d->rt_w = si.w; d->rt_h = si.h; d->rt_bpp = si.bpp; }
    }
    /* render states: D3D7 defaults (d3dtypes.h / d3d7 rendering defaults) */
    d->rs[D3D7_RS_ZENABLE] = D3D7_ZB_TRUE;
    d->rs[D3D7_RS_ZWRITEENABLE] = 1;
    d->rs[D3D7_RS_ZFUNC] = D3D7_CMP_LESSEQUAL;
    d->rs[D3D7_RS_CULLMODE] = D3D7_CULL_CCW;
    d->rs[D3D7_RS_SRCBLEND] = D3D7_BLEND_SRCALPHA;
    d->rs[D3D7_RS_DESTBLEND] = D3D7_BLEND_INVSRCALPHA;
    d->rs[D3D7_RS_SHADEMODE] = D3D7_SHADE_GOURAUD;
    d->rs[D3D7_RS_ALPHAFUNC] = D3D7_CMP_ALWAYS;
    d->rs[D3D7_RS_LIGHTING] = 1;
    d->rs[D3D7_RS_AMBIENT] = 0xFF000000u;
    d->rs[D3D7_RS_TEXTUREFACTOR] = 0xFFFFFFFFu;
    d->rs[D3D7_RS_LASTPIXEL] = 1;
    d->rs[D3D7_RS_CLIPPING] = 1;
    /* texture-stage defaults */
    for (int st = 0; st < 8; st++) {
        d->tss[st][D3D7_TSS_COLOROP] = st == 0 ? D3D7_TOP_MODULATE : D3D7_TOP_DISABLE;
        d->tss[st][D3D7_TSS_COLORARG1] = st == 0 ? D3D7_TA_TEXTURE : D3D7_TA_CURRENT;
        d->tss[st][D3D7_TSS_COLORARG2] = st == 0 ? D3D7_TA_DIFFUSE : D3D7_TA_TEXTURE;
        d->tss[st][D3D7_TSS_ALPHAOP] = st == 0 ? D3D7_TOP_SELECTARG1 : D3D7_TOP_DISABLE;
        d->tss[st][D3D7_TSS_ALPHAARG1] = st == 0 ? D3D7_TA_TEXTURE : D3D7_TA_CURRENT;
        d->tss[st][D3D7_TSS_ALPHAARG2] = st == 0 ? D3D7_TA_DIFFUSE : D3D7_TA_TEXTURE;
        d->tss[st][D3D7_TSS_ADDRESS] = D3D7_TADDR_WRAP;
        d->tss[st][D3D7_TSS_MAGFILTER] = D3D7_TF_POINT;
        d->tss[st][D3D7_TSS_MINFILTER] = D3D7_TF_POINT;
        d->tss[st][D3D7_TSS_MIPFILTER] = D3D7_TF_NONE;
    }
    /* identity transforms */
    for (int i = 0; i < 32; i++) { memset(d->trans[i], 0, sizeof(float) * 16); d->trans[i][0] = d->trans[i][5] = d->trans[i][10] = d->trans[i][15] = 1.0f; }
    /* default material: mostly white diffuse */
    d->material[0] = d->material[1] = d->material[2] = d->material[3] = 1.0f;
    d->material[7] = 1.0f;   /* ambient.a */
    d->material[11] = 1.0f;  /* specular.a */
    d->material[16] = 1.0f;  /* power */
    d->viewport[0] = 0; d->viewport[1] = 0;
    d->viewport[2] = (uint32_t)d->rt_w; d->viewport[3] = (uint32_t)d->rt_h;
    {
        float z0 = 0.0f, z1 = 1.0f;
        memcpy(&d->viewport[4], &z0, 4);
        memcpy(&d->viewport[5], &z1, 4);
    }
    /* build the device object */
    uint32_t dev_obj = new_com(g_vt_dev, K_DEVICE, slot);
    d->self = dev_obj;
    d->d3d_obj = A32(0);
    MEM32(d->d3d_obj + 4)++;
    if (out) MEM32(out) = dev_obj;
    if (trace_on())
        fprintf(stderr, "[d3d7] CreateDevice OK: %dx%d %dbpp, rt=0x%08X\n", d->rt_w, d->rt_h, d->rt_bpp, surf);
    RET(S_OK, 4);
}

static void d3d7_CreateVertexBuffer(void) {
    const char *name = "IDirect3D7::CreateVertexBuffer";
    ENTER(name);
    uint32_t desc = A32(1), out = A32(2);
    (void)A32(3);   /* DWORD flags */
    int slot = -1;
    for (int i = 0; i < MAX_VB; i++) if (!g_vb[i].used) { slot = i; break; }
    if (slot < 0) { if (out) MEM32(out) = 0; RET(E_FAIL, 4); }
    uint32_t fvf = desc ? MEM32(desc + 8) : 0;
    uint32_t n = desc ? MEM32(desc + 12) : 0;
    uint32_t stride = d3d7_fvf_stride(fvf);
    if (!stride || !n || n > 65536) { if (out) MEM32(out) = 0; RET(D3DERR_INVALIDPARAMS, 4); }
    VB7Obj *v = &g_vb[slot];
    memset(v, 0, sizeof *v);
    v->used = 1;
    v->fvf = fvf;
    v->nverts = n;
    v->data = (uint8_t *)calloc((size_t)n, stride);
    if (!v->data) { v->used = 0; if (out) MEM32(out) = 0; RET(E_FAIL, 4); }
    uint32_t obj = new_com(g_vt_vb7, K_VB7, slot);
    v->self = obj;
    if (out) MEM32(out) = obj;
    if (trace_on()) fprintf(stderr, "[d3d7] CreateVertexBuffer fvf=0x%X n=%u -> 0x%08X\n", fvf, n, obj);
    RET(S_OK, 4);
}

static void enum_zbuffer_formats(uint32_t cb, uint32_t ctx) {
    if (!cb) return;
    static const uint32_t depths[] = { 16, 32, 24 };
    uint32_t mark = hle_scratch_mark();
    uint32_t pv = hle_scratch(32);
    for (unsigned i = 0; i < sizeof depths / sizeof *depths; i++) {
        memset(hle_ptr(pv), 0, 32);
        MEM32(pv + 0) = 32;                 /* dwSize */
        MEM32(pv + 4) = DDPF_ZBUFFER;       /* dwFlags */
        MEM32(pv + 8) = 0;                  /* dwFourCC */
        MEM32(pv + 12) = depths[i];         /* dwZBufferBitDepth */
        uint32_t a[2] = { pv, ctx };
        if (!hle_call_guest(cb, 2, a)) break;   /* callback returns 0: stop */
    }
    hle_scratch_release(mark);
}
static void d3d7_EnumZBufferFormats(void) {
    const char *name = "IDirect3D7::EnumZBufferFormats";
    ENTER(name);
    if (!A32(2)) RET(E_INVALIDARG, 4);
    enum_zbuffer_formats(A32(2), A32(3));
    RET(S_OK, 4);
}

static void d3d7_EvictManagedTextures(void) {
    stub_ok("IDirect3D7::EvictManagedTextures");
    RET(S_OK, 1);
}

static void d3d7_QueryInterface(void) {
    ensure_primary_hook();
    com_query_interface(A32(0), D3D7_IID_IDirect3D7, NULL, "IDirect3D7::QueryInterface");
}
static void d3d7_AddRef(void) { ENTER("IDirect3D7::AddRef"); com_addref(); }
static void d3d7_Release(void) { ENTER("IDirect3D7::Release"); com_release(K_D3D7); }

/* ======================================================================== */
/* IDirect3DDevice7                                                         */
/* ======================================================================== */
static void dev_QueryInterface(void) {
    com_query_interface(A32(0), D3D7_IID_IDirect3DDevice7, NULL, "IDirect3DDevice7::QueryInterface");
}
static void dev_AddRef(void) { ENTER("IDirect3DDevice7::AddRef"); com_addref(); }
static void dev_Release(void) { ENTER("IDirect3DDevice7::Release"); com_release(K_DEVICE); }

static void dev_GetCaps(void) {
    const char *name = "IDirect3DDevice7::GetCaps";
    ENTER(name);
    Device *d = device_from_guest(A32(0));
    if (!d || !A32(1)) RET(D3DERR_INVALIDCALL, 2);
    D3D7DeviceDesc *desc = (D3D7DeviceDesc *)APTR(1);
    fill_device_desc(desc);
    RET(S_OK, 2);
}

static void dev_EnumTextureFormats(void) {
    const char *name = "IDirect3DDevice7::EnumTextureFormats";
    ENTER(name);
    uint32_t cb = A32(1), ctx = A32(2);
    if (!cb) RET(E_INVALIDARG, 3);
    /* Formats only; dx7z maps by masks (THRASH texture type map):
     *  16bpp 555/565/4444, 24bpp 888 (top 24 bits of the masks), 32bpp ARGB8888.
     * 8-bit paletted is offered after those for completeness. */
    typedef struct { uint32_t flags, bits, r, g, b, a; } Fmt;
    static const Fmt fmts[] = {
        { DDPF_RGB, 16, 0xF800, 0x07E0, 0x001F, 0 },                       /* RGB565 */
        { DDPF_RGB, 16, 0x7C00, 0x03E0, 0x001F, 0 },                       /* RGB555 */
        { DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x0F00, 0x00F0, 0x000F, 0xF000 }, /* ARGB4444 */
        { DDPF_RGB, 24, 0xFF0000, 0x00FF00, 0x0000FF, 0 },                 /* RGB888 */
        { DDPF_RGB | DDPF_ALPHAPIXELS, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000 }, /* ARGB8888 */
        { DDPF_RGB | DDPF_PALETTEINDEXED8, 8, 0, 0, 0, 0 },                /* P8 */
    };
    uint32_t mark = hle_scratch_mark();
    uint32_t pv = hle_scratch(32);
    for (unsigned i = 0; i < sizeof fmts / sizeof *fmts; i++) {
        memset(hle_ptr(pv), 0, 32);
        MEM32(pv + 0) = 32;
        MEM32(pv + 4) = fmts[i].flags;
        MEM32(pv + 8) = 0;
        MEM32(pv + 12) = fmts[i].bits;
        MEM32(pv + 16) = fmts[i].r;
        MEM32(pv + 20) = fmts[i].g;
        MEM32(pv + 24) = fmts[i].b;
        MEM32(pv + 28) = fmts[i].a;
        uint32_t a[2] = { pv, ctx };
        if (!hle_call_guest(cb, 2, a)) break;
    }
    hle_scratch_release(mark);
    RET(S_OK, 3);
}

static void dev_BeginScene(void) {
    ENTER("IDirect3DDevice7::BeginScene");
    Device *d = device_from_guest(A32(0));
    if (d) d->in_scene = 1;
    evlog("BeginScene rt=0x%08X", d ? d->rt_surface : 0);
    RET(S_OK, 1);
}
static void dev_EndScene(void) {
    ENTER("IDirect3DDevice7::EndScene");
    Device *d = device_from_guest(A32(0));
    if (!d) RET(D3DERR_INVALIDCALL, 1);
    d->in_scene = 0;
    evlog("EndScene dirty=%d (no readback: see dev-ddraw flush hook)", d->rt_dirty);
    /* Do NOT read the GL frame back here.  On real hardware EndScene does not
     * move pixels to guest memory; only Flip/Lock/Blt (i.e. ddraw) needs them.
     * dx7z calls Begin/EndScene several times per presented frame, and a
     * 1.2 MB glReadPixels + 640x480 conversion each time was pure overhead:
     * the ddraw FLUSH hook does the readback exactly when the guest reads the
     * surface (we saw 2.78 ms/flush, ~2 ms of it conversion).  Keep the frame
     * open so the first ddraw hook completes it. */
    if (d->rt_dirty && d->frame_started) { r_frame_end(); d->frame_started = 0; }
    RET(S_OK, 1);
}
static void dev_GetDirect3D(void) {
    ENTER("IDirect3DDevice7::GetDirect3D");
    Device *d = device_from_guest(A32(0));
    if (!d || !A32(1)) RET(D3DERR_INVALIDCALL, 2);
    MEM32(A32(1)) = d->d3d_obj;
    MEM32(d->d3d_obj + 4)++;
    RET(S_OK, 2);
}

static void dev_SetRenderTarget(void) {
    const char *name = "IDirect3DDevice7::SetRenderTarget";
    ENTER(name);
    Device *d = device_from_guest(A32(0));
    uint32_t surf = A32(1);
    (void)A32(2);
    if (!d) RET(D3DERR_INVALIDCALL, 3);
    if (d->rt_surface != surf) {
        device_flush(d);      /* finish the previous target first */
        d->rt_surface = surf;
        d->rt_guest_dirty = 1; /* preserve whatever the new surface contains */
        d->rt_dirty = 0;
        if (surf) {
            SurfInfo si;
            if (surface_info(surf, &si)) {
                d->rt_w = si.w; d->rt_h = si.h; d->rt_bpp = si.bpp;
            }
        }
        d->viewport[2] = (uint32_t)d->rt_w;
        d->viewport[3] = (uint32_t)d->rt_h;
        STATE_CHANGED(d);
    }
    RET(S_OK, 3);
}
static void dev_GetRenderTarget(void) {
    ENTER("IDirect3DDevice7::GetRenderTarget");
    Device *d = device_from_guest(A32(0));
    if (!d || !A32(1)) RET(D3DERR_INVALIDCALL, 2);
    MEM32(A32(1)) = d->rt_surface;
    if (d->rt_surface) MEM32(d->rt_surface + 4)++;
    RET(S_OK, 2);
}

static void dev_Clear(void) {
    const char *name = "IDirect3DDevice7::Clear";
    ENTER(name);
    Device *d = device_from_guest(A32(0));
    uint32_t count = A32(1), rects = A32(2), flags = A32(3), color = A32(4);
    float z = as_float_bits(A32(5));
    (void)A32(6);   /* stencil */
    if (!d) RET(D3DERR_INVALIDCALL, 7);
    if (!renderer_ready_for(d)) RET(D3DERR_INVALIDCALL, 7);
    int cc = (flags & 0x1u) != 0;   /* D3DCLEAR_TARGET */
    int cz = (flags & 0x2u) != 0;   /* D3DCLEAR_ZBUFFER */
    evlog("Clear flags=0x%X color=0x%08X rt=0x%08X guest_dirty=%d", flags, color, d->rt_surface, d->rt_guest_dirty);
    int full = count == 0 || rects == 0;
    if (!full) {
        /* check whether the single rect covers the whole target */
        const int32_t *r = (const int32_t *)APTR(2);
        if (count == 1 && r[0] <= 0 && r[1] <= 0 && r[2] >= d->rt_w && r[3] >= d->rt_h) full = 1;
    }
    if (full && cc && cz) d->rt_guest_dirty = 0;   /* a full clear overwrites 2D content */
    else if (d->rt_guest_dirty) device_sync_guest_to_gl(d);
    if (full) {
        r_clear(color, z, cc, cz);
    } else {
        for (uint32_t i = 0; i < count; i++) {
            const int32_t *r = (const int32_t *)APTR(2) + i * 4;
            r_clear_rect(color, z, cc, cz, r[0], r[1], r[2] - r[0], r[3] - r[1]);
        }
    }
    d->rt_dirty = 1;
    RET(S_OK, 7);
}

static void dev_SetTransform(void) {
    ENTER("IDirect3DDevice7::SetTransform");
    Device *d = device_from_guest(A32(0));
    uint32_t state = A32(1), mat = A32(2);
    if (!d || state >= 32 || !mat) RET(D3DERR_INVALIDCALL, 3);
    memcpy(d->trans[state], APTR(2), 16 * sizeof(float));
    STATE_CHANGED(d);
    RET(S_OK, 3);
}
static void dev_GetTransform(void) {
    ENTER("IDirect3DDevice7::GetTransform");
    Device *d = device_from_guest(A32(0));
    uint32_t state = A32(1), mat = A32(2);
    if (!d || state >= 32 || !mat) RET(D3DERR_INVALIDCALL, 3);
    memcpy(APTR(2), d->trans[state], 16 * sizeof(float));
    RET(S_OK, 3);
}
static void dev_MultiplyTransform(void) {
    ENTER("IDirect3DDevice7::MultiplyTransform");
    Device *d = device_from_guest(A32(0));
    uint32_t state = A32(1), mat = A32(2);
    if (!d || state >= 32 || !mat) RET(D3DERR_INVALIDCALL, 3);
    const float *b = (const float *)APTR(2);
    float out[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float sum = 0;
            for (int k = 0; k < 4; k++) sum += d->trans[state][i * 4 + k] * b[k * 4 + j];
            out[i * 4 + j] = sum;
        }
    memcpy(d->trans[state], out, sizeof out);
    STATE_CHANGED(d);
    RET(S_OK, 3);
}

static void dev_SetViewport(void) {
    ENTER("IDirect3DDevice7::SetViewport");
    Device *d = device_from_guest(A32(0));
    uint32_t vp = A32(1);
    if (!d || !vp) RET(D3DERR_INVALIDCALL, 2);
    const uint32_t *v = (const uint32_t *)APTR(1);
    if (memcmp(d->viewport, v, sizeof d->viewport)) {
        memcpy(d->viewport, v, sizeof d->viewport);
        STATE_CHANGED(d);
    }
    RET(S_OK, 2);
}
static void dev_GetViewport(void) {
    ENTER("IDirect3DDevice7::GetViewport");
    Device *d = device_from_guest(A32(0));
    if (!d || !A32(1)) RET(D3DERR_INVALIDCALL, 2);
    memcpy(APTR(1), d->viewport, sizeof d->viewport);
    RET(S_OK, 2);
}

static void dev_SetMaterial(void) {
    ENTER("IDirect3DDevice7::SetMaterial");
    Device *d = device_from_guest(A32(0));
    if (!d || !A32(1)) RET(D3DERR_INVALIDCALL, 2);
    memcpy(d->material, APTR(1), sizeof d->material);
    STATE_CHANGED(d);
    RET(S_OK, 2);
}
static void dev_GetMaterial(void) {
    ENTER("IDirect3DDevice7::GetMaterial");
    Device *d = device_from_guest(A32(0));
    if (!d || !A32(1)) RET(D3DERR_INVALIDCALL, 2);
    memcpy(APTR(1), d->material, sizeof d->material);
    RET(S_OK, 2);
}
static void dev_SetLight(void) {
    ENTER("IDirect3DDevice7::SetLight");
    Device *d = device_from_guest(A32(0));
    uint32_t idx = A32(1);
    if (!d || idx >= 8 || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    memcpy(&d->lights[idx], APTR(2), sizeof(D3D7Light));
    STATE_CHANGED(d);
    RET(S_OK, 3);
}
static void dev_GetLight(void) {
    ENTER("IDirect3DDevice7::GetLight");
    Device *d = device_from_guest(A32(0));
    uint32_t idx = A32(1);
    if (!d || idx >= 8 || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    memcpy(APTR(2), &d->lights[idx], sizeof(D3D7Light));
    RET(S_OK, 3);
}
static void dev_SetRenderState(void) {
    const char *name = "IDirect3DDevice7::SetRenderState";
    ENTER(name);
    Device *d = device_from_guest(A32(0));
    uint32_t state = A32(1), value = A32(2);
    if (!d) RET(D3DERR_INVALIDCALL, 3);
    if (state < D3D7_RS_COUNT && d->rs[state] != value) { d->rs[state] = value; STATE_CHANGED(d); }
    if (trace_on()) fprintf(stderr, "[d3d7]   RS %u = 0x%08X\n", state, value);
    RET(S_OK, 3);
}
static void dev_GetRenderState(void) {
    ENTER("IDirect3DDevice7::GetRenderState");
    Device *d = device_from_guest(A32(0));
    uint32_t state = A32(1);
    if (!d || state >= D3D7_RS_COUNT || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    MEM32(A32(2)) = d->rs[state];
    RET(S_OK, 3);
}
static void dev_BeginStateBlock(void) { stub_ok("IDirect3DDevice7::BeginStateBlock"); RET(S_OK, 1); }
static void dev_EndStateBlock(void) {
    stub_ok("IDirect3DDevice7::EndStateBlock");
    if (A32(1)) MEM32(A32(1)) = 0;
    RET(S_OK, 2);
}
static void dev_PreLoad(void) {
    stub_ok("IDirect3DDevice7::PreLoad");
    RET(S_OK, 2);
}

static void dev_DrawPrimitive(void) {
    const char *name = "IDirect3DDevice7::DrawPrimitive";
    ENTER(name);
    Device *d = device_from_guest(A32(0));
    uint32_t prim = A32(1), fvf = A32(2), verts = A32(3), count = A32(4);
    (void)A32(5);
    if (!d || !verts || !count) RET(D3DERR_INVALIDCALL, 6);
    if (trace_on()) fprintf(stderr, "[d3d7]   DrawPrimitive prim=%u fvf=0x%X count=%u\n", prim, fvf, count);
    device_draw(d, prim, fvf, (const uint8_t *)APTR(3), count, NULL, 0, 0);
    RET(S_OK, 6);
}
static void dev_DrawIndexedPrimitive(void) {
    const char *name = "IDirect3DDevice7::DrawIndexedPrimitive";
    ENTER(name);
    Device *d = device_from_guest(A32(0));
    uint32_t prim = A32(1), fvf = A32(2), verts = A32(3), vcount = A32(4);
    uint32_t indices = A32(5), icount = A32(6);
    (void)A32(7);
    if (!d || !verts || !indices || !icount) RET(D3DERR_INVALIDCALL, 8);
    if (trace_on()) {
        static int n;
        if (n++ < 400)
            fprintf(stderr, "[d3d7]   DrawIndexedPrimitive prim=%u fvf=0x%X verts=%u idx=%u\n",
                    prim, fvf, vcount, icount);
    }
    device_draw(d, prim, fvf, (const uint8_t *)APTR(3), vcount, (const uint16_t *)APTR(5),
                icount, 1);
    RET(S_OK, 8);
}

static void dev_SetClipStatus(void) {
    stub_ok("IDirect3DDevice7::SetClipStatus");
    Device *d = device_from_guest(A32(0));
    if (d && A32(1)) memcpy(&d->clip, APTR(1), sizeof d->clip);
    RET(S_OK, 2);
}
static void dev_GetClipStatus(void) {
    stub_ok("IDirect3DDevice7::GetClipStatus");
    Device *d = device_from_guest(A32(0));
    if (!d || !A32(1)) RET(D3DERR_INVALIDCALL, 2);
    memset(APTR(1), 0, sizeof d->clip);
    RET(S_OK, 2);
}

/* D3DDRAWPRIMITIVESTRIDEDDATA: position is a pointer to the first vertex's
 * position; the other fields point at their component arrays. We rebuild the
 * canonical vertex array element by element. */
static int decode_strided_tl(const D3D7DrawPrimitiveStridedData *sd, uint32_t fvf,
                             uint32_t count, RTLVertex *out) {
    uint32_t ntex = (fvf >> D3D7_FVF_TEXCOUNT_SHIFT) & 0xF;
    const float *pos = (const float *)sd->position;
    const uint32_t *color = (const uint32_t *)sd->diffuse;
    const uint32_t *spec = (const uint32_t *)sd->specular;
    for (uint32_t i = 0; i < count; i++) {
        RTLVertex *v = &out[i];
        v->x = pos[i * 4 + 0]; v->y = pos[i * 4 + 1];
        v->z = pos[i * 4 + 2]; v->rhw = pos[i * 4 + 3];
        decode_color(color ? color[i] : 0xFFFFFFFFu, v->color);
        decode_color(spec ? spec[i] : 0xFFFFFFFFu, v->spec);
        v->tex0[0] = v->tex0[1] = v->tex1[0] = v->tex1[1] = 0;
        for (uint32_t t = 0; t < ntex && t < 2; t++) {
            const float *tc = (const float *)sd->textureCoords[t];
            if (!tc) continue;
            float u = tc[i * 2], vv = tc[i * 2 + 1];
            if (t == 0) { v->tex0[0] = u; v->tex0[1] = vv; }
            else { v->tex1[0] = u; v->tex1[1] = vv; }
        }
    }
    return 1;
}

static void dev_DrawPrimitiveStrided(void) {
    const char *name = "IDirect3DDevice7::DrawPrimitiveStrided";
    ENTER(name);
    Device *d = device_from_guest(A32(0));
    uint32_t prim = A32(1), fvf = A32(2), data = A32(3), count = A32(4);
    (void)A32(5);
    if (!d || !data || !count) RET(D3DERR_INVALIDCALL, 6);
    if (!ensure_verts(count)) RET(E_FAIL, 6);
    if (!decode_strided_tl((const D3D7DrawPrimitiveStridedData *)APTR(3), fvf, count, g_verts))
        RET(D3DERR_INVALIDCALL, 6);
    RState st;
    build_rstate(d, &st);
    GLuint tex[2];
    bind_stage_textures(d, &st, tex);
    r_set_textures(tex[0], tex[1]);
    if (!d->frame_started) { r_frame_begin(); d->frame_started = 1; }
    r_set_viewport((int)st.vp_x, (int)st.vp_y, (int)st.vp_w, (int)st.vp_h, st.vp_minz, st.vp_maxz);
    acct_resolve();
    if (nfs5_acct_enter_p) nfs5_acct_enter_p(1);
    r_draw_tl(&st, g_verts, (int)count, NULL, 0, (int)prim);
    if (nfs5_acct_leave_p) nfs5_acct_leave_p(1);
    d->rt_dirty = 1;
    RET(S_OK, 6);
}
static void dev_DrawIndexedPrimitiveStrided(void) {
    const char *name = "IDirect3DDevice7::DrawIndexedPrimitiveStrided";
    ENTER(name);
    Device *d = device_from_guest(A32(0));
    uint32_t prim = A32(1), fvf = A32(2), data = A32(3), count = A32(4);
    uint32_t indices = A32(5), icount = A32(6);
    (void)A32(7);
    if (!d || !data || !indices || !icount) RET(D3DERR_INVALIDCALL, 8);
    if (!ensure_verts(count) ||
        !decode_strided_tl((const D3D7DrawPrimitiveStridedData *)APTR(3), fvf, count, g_verts))
        RET(E_FAIL, 8);
    RState st;
    build_rstate(d, &st);
    GLuint tex[2];
    bind_stage_textures(d, &st, tex);
    r_set_textures(tex[0], tex[1]);
    if (!d->frame_started) { r_frame_begin(); d->frame_started = 1; }
    r_set_viewport((int)st.vp_x, (int)st.vp_y, (int)st.vp_w, (int)st.vp_h, st.vp_minz, st.vp_maxz);
    acct_resolve();
    if (nfs5_acct_enter_p) nfs5_acct_enter_p(1);
    r_draw_tl(&st, g_verts, (int)count, (const uint16_t *)APTR(5), (int)icount, (int)prim);
    if (nfs5_acct_leave_p) nfs5_acct_leave_p(1);
    d->rt_dirty = 1;
    RET(S_OK, 8);
}
static void dev_DrawPrimitiveVB(void) {
    stub_fail("IDirect3DDevice7::DrawPrimitiveVB");
    RET(E_NOTIMPL, 6);
}
static void dev_DrawIndexedPrimitiveVB(void) {
    stub_fail("IDirect3DDevice7::DrawIndexedPrimitiveVB");
    RET(E_NOTIMPL, 8);
}
static void dev_ComputeSphereVisibility(void) {
    const char *name = "IDirect3DDevice7::ComputeSphereVisibility";
    ENTER(name);
    uint32_t out = A32(5);
    if (out) MEM32(out) = 0;    /* 0: all spheres fully visible */
    RET(S_OK, 6);
}
static void dev_GetTexture(void) {
    ENTER("IDirect3DDevice7::GetTexture");
    Device *d = device_from_guest(A32(0));
    uint32_t stage = A32(1);
    if (!d || stage >= 8 || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    MEM32(A32(2)) = d->stage_surf[stage];
    if (d->stage_surf[stage]) MEM32(d->stage_surf[stage] + 4)++;
    RET(S_OK, 3);
}
static void dev_SetTexture(void) {
    const char *name = "IDirect3DDevice7::SetTexture";
    ENTER(name);
    Device *d = device_from_guest(A32(0));
    uint32_t stage = A32(1), surf = A32(2);
    if (!d || stage >= 8) RET(D3DERR_INVALIDCALL, 3);
    if (!d->stage_surf[stage] != !surf) STATE_CHANGED(d);   /* RState.stage_on */
    d->stage_surf[stage] = surf;
    if (trace_on()) {
        static int n;
        if (n++ < 200) fprintf(stderr, "[d3d7]   SetTexture stage=%u surface=0x%08X\n", stage, surf);
    }
    RET(S_OK, 3);
}
static void dev_GetTextureStageState(void) {
    ENTER("IDirect3DDevice7::GetTextureStageState");
    Device *d = device_from_guest(A32(0));
    uint32_t stage = A32(1), type = A32(2);
    if (!d || stage >= 8 || type >= D3D7_TSS_COUNT || !A32(3)) RET(D3DERR_INVALIDCALL, 4);
    MEM32(A32(3)) = d->tss[stage][type];
    RET(S_OK, 4);
}
static void dev_SetTextureStageState(void) {
    const char *name = "IDirect3DDevice7::SetTextureStageState";
    ENTER(name);
    Device *d = device_from_guest(A32(0));
    uint32_t stage = A32(1), type = A32(2), value = A32(3);
    if (!d || stage >= 8) RET(D3DERR_INVALIDCALL, 4);
    if (type < D3D7_TSS_COUNT && d->tss[stage][type] != value) { d->tss[stage][type] = value; STATE_CHANGED(d); }
    if (trace_on()) fprintf(stderr, "[d3d7]   TSS stage%u type %u = 0x%08X\n", stage, type, value);
    RET(S_OK, 4);
}
static void dev_ValidateDevice(void) {
    const char *name = "IDirect3DDevice7::ValidateDevice";
    ENTER(name);
    if (A32(1)) MEM32(A32(1)) = 1;
    RET(S_OK, 2);
}
static void dev_ApplyStateBlock(void) { stub_fail("IDirect3DDevice7::ApplyStateBlock"); RET(E_NOTIMPL, 2); }
static void dev_CaptureStateBlock(void) { stub_fail("IDirect3DDevice7::CaptureStateBlock"); RET(E_NOTIMPL, 2); }
static void dev_DeleteStateBlock(void) { stub_ok("IDirect3DDevice7::DeleteStateBlock"); RET(S_OK, 2); }
static void dev_CreateStateBlock(void) {
    stub_ok("IDirect3DDevice7::CreateStateBlock");
    if (A32(2)) MEM32(A32(2)) = 0;
    RET(S_OK, 3);
}
static void dev_Load(void) {
    /* IDirect3DDevice7::Load(dst, dstpt, src, srcrect, flags): copy between
     * surfaces of the same format (used for managed texture loads). */
    const char *name = "IDirect3DDevice7::Load";
    ENTER(name);
    Device *d = device_from_guest(A32(0));
    uint32_t dst_surf = A32(1);
    const int32_t *dstpt = (const int32_t *)APTR(2);
    uint32_t src_surf = A32(3);
    const int32_t *srcrect = (const int32_t *)APTR(4);
    (void)A32(5);
    if (!d || !dst_surf || !src_surf) RET(D3DERR_INVALIDCALL, 6);
    SurfInfo s, t;
    if (!surface_info(src_surf, &s) || !surface_info(dst_surf, &t)) RET(D3DERR_INVALIDCALL, 6);
    int sx = srcrect ? srcrect[0] : 0, sy = srcrect ? srcrect[1] : 0;
    int sw = srcrect ? srcrect[2] - srcrect[0] : s.w;
    int sh = srcrect ? srcrect[3] - srcrect[1] : s.h;
    int dx = dstpt ? dstpt[0] : 0, dy = dstpt ? dstpt[1] : 0;
    if (s.bpp != t.bpp) { stub_fail("IDirect3DDevice7::Load (format conversion)"); RET(E_NOTIMPL, 6); }
    for (int y = 0; y < sh; y++) {
        if (sy + y < 0 || sy + y >= s.h || dy + y < 0 || dy + y >= t.h) continue;
        for (int x = 0; x < sw; x++) {
            if (sx + x < 0 || sx + x >= s.w || dx + x < 0 || dx + x >= t.w) continue;
            guest_put_pixel(&t, dx + x, dy + y, guest_pixel(&s, sx + x, sy + y));
        }
    }
    d3d7_note_surface_dirty(dst_surf);
    RET(S_OK, 6);
}
static void dev_LightEnable(void) {
    ENTER("IDirect3DDevice7::LightEnable");
    Device *d = device_from_guest(A32(0));
    uint32_t idx = A32(1);
    if (!d || idx >= 32) RET(D3DERR_INVALIDCALL, 3);
    if (A32(2)) d->light_on |= 1u << idx;
    else d->light_on &= ~(1u << idx);
    STATE_CHANGED(d);
    RET(S_OK, 3);
}
static void dev_GetLightEnable(void) {
    ENTER("IDirect3DDevice7::GetLightEnable");
    Device *d = device_from_guest(A32(0));
    uint32_t idx = A32(1);
    if (!d || idx >= 32 || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    MEM32(A32(2)) = (d->light_on >> idx) & 1u;
    RET(S_OK, 3);
}
static void dev_SetClipPlane(void) {
    const char *name = "IDirect3DDevice7::SetClipPlane";
    ENTER(name);
    Device *d = device_from_guest(A32(0));
    uint32_t idx = A32(1);
    if (!d || idx >= 8 || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    memcpy(d->clip_plane[idx], APTR(2), 4 * sizeof(float));
    /* stored but not applied (the device advertises 0 user clip planes) */
    RET(S_OK, 3);
}
static void dev_GetClipPlane(void) {
    ENTER("IDirect3DDevice7::GetClipPlane");
    Device *d = device_from_guest(A32(0));
    uint32_t idx = A32(1);
    if (!d || idx >= 8 || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    memcpy(APTR(2), d->clip_plane[idx], 4 * sizeof(float));
    RET(S_OK, 3);
}
static void dev_GetInfo(void) {
    stub_fail("IDirect3DDevice7::GetInfo");
    RET(E_NOTIMPL, 4);
}

/* ======================================================================== */
/* IDirect3D3 - legacy face, needed by dx7z's THRASH_is DirectX version probe. */
/* dx7z QIs it from the DirectDraw4 object and only checks success, then      */
/* Release()s it (verified in lifted 0x100038B0/0x10003945), but the old      */
/* (pre-D3D7) THRASH renderer path also exists in the module, so the methods  */
/* are all present and either real (EnumDevices/EnumZBufferFormats) or an     */
/* explicit, logged E_NOTIMPL stub. The vtable order is IDirect3D3 from       */
/* d3d.h: IUnknown, EnumDevices, CreateLight, CreateMaterial, CreateViewport, */
/* FindDevice, CreateDevice(4 args), CreateVertexBuffer(4 args),             */
/* EnumZBufferFormats, EvictManagedTextures.                                 */
/* ======================================================================== */
static void fill_device_desc_v1(D3D7DeviceDescV1 *d) {
    memset(d, 0, sizeof *d);
    d->dwSize = (uint32_t)sizeof *d;
    d->dwFlags = D3D7_DDD_COLORMODEL | D3D7_DDD_DEVCAPS | D3D7_DDD_TRANSFORMCAPS |
                 D3D7_DDD_LIGHTINGCAPS | D3D7_DDD_BCLIPPING | D3D7_DDD_LINECAPS |
                 D3D7_DDD_TRICAPS | D3D7_DDD_DEVICERENDERBITDEPTH |
                 D3D7_DDD_DEVICEZBUFFERBITDEPTH | D3D7_DDD_MAXBUFFERSIZE | D3D7_DDD_MAXVERTEXCOUNT;
    d->dcmColorModel = D3D7_COLOR_MODEL_RGB;
    d->dwDevCaps = D3D7_DEVCAPS_FLOATTLVERTEX | D3D7_DEVCAPS_EXECUTESYSTEMMEMORY |
                   D3D7_DEVCAPS_TLVERTEXSYSTEMMEMORY | D3D7_DEVCAPS_TLVERTEXVIDEOMEMORY |
                   D3D7_DEVCAPS_TEXTURESYSTEMMEMORY | D3D7_DEVCAPS_TEXTUREVIDEOMEMORY |
                   D3D7_DEVCAPS_DRAWPRIMTLVERTEX | D3D7_DEVCAPS_CANRENDERAFTERFLIP |
                   D3D7_DEVCAPS_DRAWPRIMITIVES2 | D3D7_DEVCAPS_DRAWPRIMITIVES2EX |
                   D3D7_DEVCAPS_HWRASTERIZATION;
    d->dtcTransformCaps.dwSize = (uint32_t)sizeof(D3D7TransformCaps);
    d->dtcTransformCaps.dwCaps = D3D7_TRANSFORMCAPS_CLIP;
    d->bClipping = 1;
    d->dlcLightingCaps.dwSize = (uint32_t)sizeof(D3D7LightingCaps);
    d->dlcLightingCaps.dwCaps = D3D7_LIGHTCAPS_POINT | D3D7_LIGHTCAPS_SPOT | D3D7_LIGHTCAPS_DIRECTIONAL;
    d->dlcLightingCaps.dwLightingModel = D3D7_LIGHTINGMODEL_RGB;
    d->dlcLightingCaps.dwNumLights = 8;
    uint32_t misc = D3D7_PMISCCAPS_CULLNONE | D3D7_PMISCCAPS_CULLCW | D3D7_PMISCCAPS_CULLCCW;
    fill_prim_caps(&d->dpcLineCaps, misc);
    fill_prim_caps(&d->dpcTriCaps, misc);
    d->dwDeviceRenderBitDepth = D3D7_DDBD_16 | D3D7_DDBD_32;
    d->dwDeviceZBufferBitDepth = D3D7_DDBD_16 | D3D7_DDBD_24 | D3D7_DDBD_32;
    d->dwMaxBufferSize = 65535;
    d->dwMaxVertexCount = 65535;
    d->dwMinTextureWidth = 1; d->dwMinTextureHeight = 1;
    d->dwMaxTextureWidth = 1024; d->dwMaxTextureHeight = 1024;
    d->dwMaxTextureRepeat = 1024;
    d->dwMaxTextureAspectRatio = 1024;
    d->dwMaxAnisotropy = 1;
    d->dvGuardBandLeft = -4096.0f; d->dvGuardBandTop = -4096.0f;
    d->dvGuardBandRight = 4096.0f; d->dvGuardBandBottom = 4096.0f;
    d->dwFVFCaps = 8 | D3D7_FVFCAPS_DONOTSTRIPELEMENTS;
    d->dwTextureOpCaps = D3DT7_OPCAPS;
    d->wMaxTextureBlendStages = 2;
    d->wMaxSimultaneousTextures = 2;
}

static void d3d3_QueryInterface(void) {
    com_query_interface(A32(0), D3D7_IID_IDirect3D3, NULL, "IDirect3D3::QueryInterface");
}
static void d3d3_AddRef(void) { ENTER("IDirect3D3::AddRef"); com_addref(); }
static void d3d3_Release(void) { ENTER("IDirect3D3::Release"); com_release(K_D3D3); }

static void d3d3_EnumDevices(void) {
    const char *name = "IDirect3D3::EnumDevices";
    ENTER(name);
    uint32_t cb = A32(1), ctx = A32(2);
    if (!cb) RET(E_INVALIDARG, 3);
    uint32_t mark = hle_scratch_mark();
    uint32_t guidv = hle_scratch(16);
    uint32_t halv = hle_scratch(sizeof(D3D7DeviceDescV1));
    uint32_t helv = hle_scratch(sizeof(D3D7DeviceDescV1));
    uint32_t pathv = hle_scratch(64);
    uint32_t namv = hle_scratch(64);
    memcpy(hle_ptr(guidv), D3D7_CLSID_IDirect3DHALDevice, 16);
    fill_device_desc_v1((D3D7DeviceDescV1 *)hle_ptr(halv));
    memset(hle_ptr(helv), 0, sizeof(D3D7DeviceDescV1));
    ((D3D7DeviceDescV1 *)hle_ptr(helv))->dwSize = (uint32_t)sizeof(D3D7DeviceDescV1);
    snprintf((char *)hle_ptr(pathv), 64, "%s", k_device_desc_string);
    snprintf((char *)hle_ptr(namv), 64, "%s", k_device_name_string);
    uint32_t a[6] = { guidv, pathv, namv, halv, helv, ctx };
    hle_call_guest(cb, 6, a);
    hle_scratch_release(mark);
    RET(DD_OK, 3);
}
static void d3d3_CreateLight(void) {
    stub_fail("IDirect3D3::CreateLight");
    if (A32(1)) MEM32(A32(1)) = 0;
    RET(E_NOTIMPL, 3);
}
static void d3d3_CreateMaterial(void) {
    stub_fail("IDirect3D3::CreateMaterial");
    if (A32(1)) MEM32(A32(1)) = 0;
    RET(E_NOTIMPL, 3);
}
static void d3d3_CreateViewport(void) {
    stub_fail("IDirect3D3::CreateViewport");
    if (A32(1)) MEM32(A32(1)) = 0;
    RET(E_NOTIMPL, 3);
}
static void d3d3_FindDevice(void) {
    stub_fail("IDirect3D3::FindDevice");
    RET(E_NOTIMPL, 3);
}
static void d3d3_CreateDevice(void) {
    /* the pre-D3D7 device path is not emulated: dx7z always takes its D3D7
     * path when IDirect3D7 is available (which it is). Explicit failure.
     * IDirect3D3::CreateDevice(clsid, surface4, IDirect3DDevice3**, outer):
     * the out pointer is argument 2 (A32(2)), not the outer (A32(3)). */
    stub_fail("IDirect3D3::CreateDevice");
    if (A32(2)) MEM32(A32(2)) = 0;
    RET(E_NOTIMPL, 5);
}
static void d3d3_CreateVertexBuffer(void) {
    stub_fail("IDirect3D3::CreateVertexBuffer");
    if (A32(2)) MEM32(A32(2)) = 0;
    RET(E_NOTIMPL, 5);
}
static void d3d3_EnumZBufferFormats(void) {
    ENTER("IDirect3D3::EnumZBufferFormats");
    if (A32(2)) enum_zbuffer_formats(A32(2), A32(3));
    RET(S_OK, 4);
}
static void d3d3_EvictManagedTextures(void) {
    stub_ok("IDirect3D3::EvictManagedTextures");
    RET(S_OK, 1);
}

static const win32hle_shim g_shims_d3d3[] = {
    { "IDirect3D3::QueryInterface", d3d3_QueryInterface },
    { "IDirect3D3::AddRef", d3d3_AddRef },
    { "IDirect3D3::Release", d3d3_Release },
    { "IDirect3D3::EnumDevices", d3d3_EnumDevices },
    { "IDirect3D3::CreateLight", d3d3_CreateLight },
    { "IDirect3D3::CreateMaterial", d3d3_CreateMaterial },
    { "IDirect3D3::CreateViewport", d3d3_CreateViewport },
    { "IDirect3D3::FindDevice", d3d3_FindDevice },
    { "IDirect3D3::CreateDevice", d3d3_CreateDevice },
    { "IDirect3D3::CreateVertexBuffer", d3d3_CreateVertexBuffer },
    { "IDirect3D3::EnumZBufferFormats", d3d3_EnumZBufferFormats },
    { "IDirect3D3::EvictManagedTextures", d3d3_EvictManagedTextures },
    { 0, 0 }
};

static uint32_t d3d3_object(void) {
    ensure_vtables_pub();
    for (int i = 0; i < 2; i++) if (g_d3d3[i].used) return g_d3d3[i].self;
    for (int i = 0; i < 2; i++) {
        if (!g_d3d3[i].used) {
            g_d3d3[i].used = 1;
            g_d3d3[i].self = new_com(g_vt_d3d3, K_D3D3, i);
            return g_d3d3[i].self;
        }
    }
    return 0;
}

/* ======================================================================== */
/* IDirect3DTexture2 (surface QI face; dx7z never references its IID, kept   */
/* API-complete so a texture handle path cannot silently pass)              */
/* ======================================================================== */
static void tex2_QueryInterface(void) {
    com_query_interface(A32(0), D3D7_IID_IDirect3DTexture2, NULL,
                        "IDirect3DTexture2::QueryInterface");
}
static void tex2_AddRef(void) { ENTER("IDirect3DTexture2::AddRef"); com_addref(); }
static void tex2_Release(void) { ENTER("IDirect3DTexture2::Release"); com_release(K_TEX2); }
static void tex2_GetHandle(void) {
    ENTER("IDirect3DTexture2::GetHandle");
    uint32_t self = A32(0), out = A32(2);
    if (!out) RET(D3DERR_INVALIDCALL, 3);
    uint32_t idx = obj_idx(self);
    MEM32(out) = idx < MAX_TEX2 ? g_tex2[idx].surf : 0;   /* handle = surface VA */
    RET(S_OK, 3);
}
static void tex2_PaletteChanged(void) {
    ENTER("IDirect3DTexture2::PaletteChanged");
    uint32_t self = A32(0);
    uint32_t idx = obj_idx(self);
    if (idx < MAX_TEX2 && g_tex2[idx].surf) d3d7_note_surface_dirty(g_tex2[idx].surf);
    RET(S_OK, 3);
}
static void tex2_Load(void) {
    stub_ok("IDirect3DTexture2::Load");
    RET(S_OK, 2);
}

/* QI hook for surface objects (ddraw may forward surface QI here). Returns a
 * new IDirect3DTexture2 for IID_IDirect3DTexture2, 0 otherwise. */
uint32_t d3d7_surface_query_interface(uint32_t surf, const uint8_t *iid) {
    if (!iid || !d3d7_guid_eq(iid, D3D7_IID_IDirect3DTexture2)) return 0;
    ensure_vtables_pub();
    for (int i = 0; i < MAX_TEX2; i++) {
        if (g_tex2[i].used && g_tex2[i].surf == surf) return g_tex2[i].self;
    }
    for (int i = 0; i < MAX_TEX2; i++) {
        if (!g_tex2[i].used) {
            g_tex2[i].used = 1;
            g_tex2[i].surf = surf;
            g_tex2[i].self = new_com(g_vt_tex2, K_TEX2, i);
            return g_tex2[i].self;
        }
    }
    return 0;
}

/* ======================================================================== */
/* IDirect3DVertexBuffer7                                                   */
/* ======================================================================== */
static void vb7_QueryInterface(void) {
    com_query_interface(A32(0), NULL, NULL, "IDirect3DVertexBuffer7::QueryInterface");
}
static void vb7_AddRef(void) { ENTER("IDirect3DVertexBuffer7::AddRef"); com_addref(); }
static void vb7_Release(void) { ENTER("IDirect3DVertexBuffer7::Release"); com_release(K_VB7); }
static void vb7_Lock(void) {
    ENTER("IDirect3DVertexBuffer7::Lock");
    uint32_t self = A32(0), data = A32(2), size = A32(3);
    uint32_t i = obj_idx(self);
    if (i >= MAX_VB || !g_vb[i].used) RET(D3DERR_INVALIDCALL, 4);
    VB7Obj *v = &g_vb[i];
    uint32_t stride = d3d7_fvf_stride(v->fvf);
    if (data) MEM32(data) = v->data ? hle_va(v->data) : 0;
    if (size) MEM32(size) = stride * v->nverts;
    v->locked = 1;
    RET(S_OK, 4);
}
static void vb7_Unlock(void) {
    ENTER("IDirect3DVertexBuffer7::Unlock");
    /* IDirect3DVertexBuffer7::Unlock(THIS) = this only -> 1 stack dword */
    RET(S_OK, 1);
}
static void vb7_ProcessVertices(void) {
    stub_fail("IDirect3DVertexBuffer7::ProcessVertices");
    RET(E_NOTIMPL, 8);
}
static void vb7_GetVertexBufferDesc(void) {
    ENTER("IDirect3DVertexBuffer7::GetVertexBufferDesc");
    uint32_t self = A32(0), desc = A32(1);
    uint32_t i = obj_idx(self);
    if (i >= MAX_VB || !g_vb[i].used || !desc) RET(D3DERR_INVALIDCALL, 2);
    D3D7VertexBufferDesc *d = (D3D7VertexBufferDesc *)APTR(1);
    memset(d, 0, sizeof *d);
    d->dwSize = (uint32_t)sizeof *d;
    d->dwCaps = 0;
    d->dwFVF = g_vb[i].fvf;
    d->dwNumVertices = g_vb[i].nverts;
    RET(S_OK, 2);
}
static void vb7_Optimize(void) { stub_ok("IDirect3DVertexBuffer7::Optimize"); RET(S_OK, 3); }
static void vb7_ProcessVerticesStrided(void) {
    stub_fail("IDirect3DVertexBuffer7::ProcessVerticesStrided");
    RET(E_NOTIMPL, 8);
}

/* ======================================================================== */
/* vtables / shim tables / public entry points                              */
/* ======================================================================== */
static const win32hle_shim g_shims_d3d7[] = {
    { "IDirect3D7::QueryInterface", d3d7_QueryInterface },
    { "IDirect3D7::AddRef", d3d7_AddRef },
    { "IDirect3D7::Release", d3d7_Release },
    { "IDirect3D7::EnumDevices", d3d7_EnumDevices },
    { "IDirect3D7::CreateDevice", d3d7_CreateDevice },
    { "IDirect3D7::CreateVertexBuffer", d3d7_CreateVertexBuffer },
    { "IDirect3D7::EnumZBufferFormats", d3d7_EnumZBufferFormats },
    { "IDirect3D7::EvictManagedTextures", d3d7_EvictManagedTextures },
    { 0, 0 }
};

static const win32hle_shim g_shims_dev[] = {
    { "IDirect3DDevice7::QueryInterface", dev_QueryInterface },
    { "IDirect3DDevice7::AddRef", dev_AddRef },
    { "IDirect3DDevice7::Release", dev_Release },
    { "IDirect3DDevice7::GetCaps", dev_GetCaps },
    { "IDirect3DDevice7::EnumTextureFormats", dev_EnumTextureFormats },
    { "IDirect3DDevice7::BeginScene", dev_BeginScene },
    { "IDirect3DDevice7::EndScene", dev_EndScene },
    { "IDirect3DDevice7::GetDirect3D", dev_GetDirect3D },
    { "IDirect3DDevice7::SetRenderTarget", dev_SetRenderTarget },
    { "IDirect3DDevice7::GetRenderTarget", dev_GetRenderTarget },
    { "IDirect3DDevice7::Clear", dev_Clear },
    { "IDirect3DDevice7::SetTransform", dev_SetTransform },
    { "IDirect3DDevice7::GetTransform", dev_GetTransform },
    { "IDirect3DDevice7::SetViewport", dev_SetViewport },
    { "IDirect3DDevice7::MultiplyTransform", dev_MultiplyTransform },
    { "IDirect3DDevice7::GetViewport", dev_GetViewport },
    { "IDirect3DDevice7::SetMaterial", dev_SetMaterial },
    { "IDirect3DDevice7::GetMaterial", dev_GetMaterial },
    { "IDirect3DDevice7::SetLight", dev_SetLight },
    { "IDirect3DDevice7::GetLight", dev_GetLight },
    { "IDirect3DDevice7::SetRenderState", dev_SetRenderState },
    { "IDirect3DDevice7::GetRenderState", dev_GetRenderState },
    { "IDirect3DDevice7::BeginStateBlock", dev_BeginStateBlock },
    { "IDirect3DDevice7::EndStateBlock", dev_EndStateBlock },
    { "IDirect3DDevice7::PreLoad", dev_PreLoad },
    { "IDirect3DDevice7::DrawPrimitive", dev_DrawPrimitive },
    { "IDirect3DDevice7::DrawIndexedPrimitive", dev_DrawIndexedPrimitive },
    { "IDirect3DDevice7::SetClipStatus", dev_SetClipStatus },
    { "IDirect3DDevice7::GetClipStatus", dev_GetClipStatus },
    { "IDirect3DDevice7::DrawPrimitiveStrided", dev_DrawPrimitiveStrided },
    { "IDirect3DDevice7::DrawIndexedPrimitiveStrided", dev_DrawIndexedPrimitiveStrided },
    { "IDirect3DDevice7::DrawPrimitiveVB", dev_DrawPrimitiveVB },
    { "IDirect3DDevice7::DrawIndexedPrimitiveVB", dev_DrawIndexedPrimitiveVB },
    { "IDirect3DDevice7::ComputeSphereVisibility", dev_ComputeSphereVisibility },
    { "IDirect3DDevice7::GetTexture", dev_GetTexture },
    { "IDirect3DDevice7::SetTexture", dev_SetTexture },
    { "IDirect3DDevice7::GetTextureStageState", dev_GetTextureStageState },
    { "IDirect3DDevice7::SetTextureStageState", dev_SetTextureStageState },
    { "IDirect3DDevice7::ValidateDevice", dev_ValidateDevice },
    { "IDirect3DDevice7::ApplyStateBlock", dev_ApplyStateBlock },
    { "IDirect3DDevice7::CaptureStateBlock", dev_CaptureStateBlock },
    { "IDirect3DDevice7::DeleteStateBlock", dev_DeleteStateBlock },
    { "IDirect3DDevice7::CreateStateBlock", dev_CreateStateBlock },
    { "IDirect3DDevice7::Load", dev_Load },
    { "IDirect3DDevice7::LightEnable", dev_LightEnable },
    { "IDirect3DDevice7::GetLightEnable", dev_GetLightEnable },
    { "IDirect3DDevice7::SetClipPlane", dev_SetClipPlane },
    { "IDirect3DDevice7::GetClipPlane", dev_GetClipPlane },
    { "IDirect3DDevice7::GetInfo", dev_GetInfo },
    { 0, 0 }
};

static const win32hle_shim g_shims_tex2[] = {
    { "IDirect3DTexture2::QueryInterface", tex2_QueryInterface },
    { "IDirect3DTexture2::AddRef", tex2_AddRef },
    { "IDirect3DTexture2::Release", tex2_Release },
    { "IDirect3DTexture2::GetHandle", tex2_GetHandle },
    { "IDirect3DTexture2::PaletteChanged", tex2_PaletteChanged },
    { "IDirect3DTexture2::Load", tex2_Load },
    { 0, 0 }
};

static const win32hle_shim g_shims_vb7[] = {
    { "IDirect3DVertexBuffer7::QueryInterface", vb7_QueryInterface },
    { "IDirect3DVertexBuffer7::AddRef", vb7_AddRef },
    { "IDirect3DVertexBuffer7::Release", vb7_Release },
    { "IDirect3DVertexBuffer7::Lock", vb7_Lock },
    { "IDirect3DVertexBuffer7::Unlock", vb7_Unlock },
    { "IDirect3DVertexBuffer7::ProcessVertices", vb7_ProcessVertices },
    { "IDirect3DVertexBuffer7::GetVertexBufferDesc", vb7_GetVertexBufferDesc },
    { "IDirect3DVertexBuffer7::Optimize", vb7_Optimize },
    { "IDirect3DVertexBuffer7::ProcessVerticesStrided", vb7_ProcessVerticesStrided },
    { 0, 0 }
};

static int g_vt_ready;

static void stats_atexit(void) {
    /* Do not tear GL down at process exit: the host may already have shut SDL
     * down, and losing the context costs nothing. d3d7_shutdown() is available
     * for an explicit orderly stop (main.c/hle_on_exit). */
    if (stats_on() || trace_on()) d3d7_stats_report();
}
void ensure_vtables_pub(void) {
    if (g_vt_ready) return;
    g_vt_ready = 1;
    g_vt_d3d7 = hle_com_vtable(g_shims_d3d7);
    g_vt_dev = hle_com_vtable(g_shims_dev);
    g_vt_tex2 = hle_com_vtable(g_shims_tex2);
    g_vt_vb7 = hle_com_vtable(g_shims_vb7);
    g_vt_d3d3 = hle_com_vtable(g_shims_d3d3);
    atexit(stats_atexit);
    /* The renderer's own stats interface is independent of the HLE. */
    memset(g_stats, 0, sizeof g_stats);
    g_stats_n = 0;
}

static uint32_t d3d7_object(void) {
    ensure_vtables_pub();
    for (int i = 0; i < 2; i++)
        if (g_d3d7[i].used) return g_d3d7[i].self;
    for (int i = 0; i < 2; i++) {
        if (!g_d3d7[i].used) {
            g_d3d7[i].used = 1;
            g_d3d7[i].self = new_com(g_vt_d3d7, K_D3D7, i);
            return g_d3d7[i].self;
        }
    }
    return 0;
}

uint32_t d3d7_ddraw_query_interface(const uint8_t *iid) {
    if (!iid) return 0;
    if (d3d7_guid_eq(iid, D3D7_IID_IDirect3D7)) {
        if (trace_on()) fprintf(stderr, "[d3d7] ddraw QI IID_IDirect3D7 -> served\n");
        return d3d7_object();
    }
    if (d3d7_guid_eq(iid, D3D7_IID_IDirect3D3)) {
        /* dx7z's THRASH_is DirectX-version probe asks for IDirect3D3 on the
         * DirectDraw4 object; it only checks that the QI succeeds. */
        if (trace_on()) fprintf(stderr, "[d3d7] ddraw QI IID_IDirect3D3 -> served\n");
        return d3d3_object();
    }
    return 0;
}

int d3d7_active(void) { return active_device() != NULL; }

void d3d7_shutdown(void) {
    Device *d = active_device();
    if (d) {
        device_flush(d);
        d->used = 0;
    }
    for (int i = 0; i < g_texcache_used; i++)
        if (g_texcache[i].used && g_texcache[i].rt.gl) r_tex_delete(&g_texcache[i].rt);
    memset(g_texcache, 0, sizeof g_texcache);
    g_texcache_used = 0;
    if (r_ready()) r_shutdown();
    free(g_rgba); g_rgba = NULL; g_rgba_cap = 0;
    free(g_verts); g_verts = NULL; g_verts_cap = 0;
    free(g_litverts); g_litverts = NULL; g_litverts_cap = 0;
}

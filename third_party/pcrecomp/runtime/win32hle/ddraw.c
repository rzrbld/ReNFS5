/*
 * ddraw.c - DirectDraw in software: IDirectDraw (and IDirectDraw2), surfaces
 * (IDirectDrawSurface 1-3), palettes and clippers.
 *
 * Every surface is system memory the game Locks and draws into, or blits
 * between; Blt and BltFast do the copy, colour fill, source colour key and
 * stretch in C. The display is virtual: SetDisplayMode records the mode, and
 * the primary surface's pixels are what a presenter (screen.c) shows,
 * through hle_dd_frame. Nothing is exclusive and nothing touches a real
 * display mode.
 *
 * Each surface gets a 64 KB tail behind its last row: a 1990s blitter that
 * checks only where a block starts runs past the bottom (Tiberian Sun's movie
 * decoder does, by up to 4.6 KB), into slack Windows happens to leave.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "win32hle.h"

#define DD_OK                   0u
#define DDERR_INVALIDPARAMS     0x80070057u
#define DDERR_UNSUPPORTED       0x80004001u
#define DDERR_NOTFOUND          0x887600FFu
#define DDERR_NOPALETTEATTACHED 0x8876023Cu
#define DDERR_NOCLIPPERATTACHED 0x88760082u
#define DDERR_NOCOLORKEY        0x887600D7u
#define DDERR_NOTLOCKED         0x887602ACu
#define DDERR_GENERIC           0x80004005u
#define DDERR_SURFACEBUSY       0x887601B6u

#define DDSD_CAPS        0x00000001u
#define DDSD_HEIGHT      0x00000002u
#define DDSD_WIDTH       0x00000004u
#define DDSD_PITCH       0x00000008u
#define DDSD_BACKBUFFERCOUNT 0x00000020u
#define DDSD_MIPMAPCOUNT     0x00020000u
#define DDSD_LPSURFACE   0x00000800u
#define DDSD_PIXELFORMAT 0x00001000u
#define DDSD_CKSRCBLT    0x00010000u
#define DDSD_REFRESHRATE 0x00040000u
#define DDSCAPS_BACKBUFFER     0x00000004u
#define DDSCAPS_COMPLEX        0x00000008u
#define DDSCAPS_FLIP           0x00000010u
#define DDSCAPS_FRONTBUFFER    0x00000020u
#define DDSCAPS_OFFSCREENPLAIN 0x00000040u
#define DDSCAPS_PRIMARYSURFACE 0x00000200u
#define DDSCAPS_SYSTEMMEMORY   0x00000800u
#define DDSCAPS_VIDEOMEMORY    0x00004000u
#define DDSCAPS_VISIBLE        0x00008000u
#define DDSCAPS_LOCALVIDMEM    0x10000000u
#define DDSCAPS_TEXTURE        0x00001000u
#define DDSCAPS_3DDEVICE       0x00002000u
#define DDSCAPS_ZBUFFER        0x00020000u
#define DDSCAPS_MIPMAP         0x00400000u
#define DDSCAPS2_MIPMAPSUBLEVEL 0x00010000u
#define DDPF_ALPHAPIXELS       0x00000001u
#define DDPF_ZBUFFER           0x00000400u
#define DDPF_PALETTEINDEXED8   0x00000020u
#define DDPF_RGB               0x00000040u
#define DDBLT_COLORFILL        0x00000400u
#define DDBLT_KEYSRC           0x00008000u
#define DDBLT_KEYSRCOVERRIDE   0x00010000u
#define DDBLTFAST_SRCCOLORKEY  0x00000001u
#define DDCKEY_SRCBLT          0x00000008u
#define SURF_TAIL              (64u << 10)

/* The Direct3D7 renderer (runtime/d3d7.c, renderer-eng) answers the DD7 face's
 * IID_IDirect3D7 QueryInterface and hears about surface traffic, so a rendered
 * frame reaches the guest's render-target pixels before ddraw copies it. All
 * three are weak: without a renderer the ddraw layer builds and runs alone. */
extern uint32_t d3d7_ddraw_query_interface(const uint8_t *iid) __attribute__((weak));
extern void     d3d7_note_surface_dirty(uint32_t surf) __attribute__((weak));
extern int nfs5_dx7_sync_lock(uint32_t) __attribute__((weak));
extern int d3d7_sync_surface(uint32_t) __attribute__((weak));
extern int d3d7_fill_gpu(uint32_t,uint32_t,int,int,int,int) __attribute__((weak));
extern int d3d7_flip_gpu(uint32_t front, uint32_t back) __attribute__((weak));
extern void     d3d7_flush_render_target(uint32_t surf) __attribute__((weak));
extern void     nfs5_frame_mark(void) __attribute__((weak));    /* perf-eng: a frame was presented */
extern void     d3d7_note_present(void) __attribute__((weak));  /* renderer: one ddraw present */
#define D3D7_QI(iid)      (d3d7_ddraw_query_interface ? d3d7_ddraw_query_interface(iid) : 0)
#define D3D7_DIRTY(obj)   do { if (d3d7_note_surface_dirty) d3d7_note_surface_dirty(obj); } while (0)
#define D3D7_FLUSH(obj)   do { if (d3d7_flush_render_target) d3d7_flush_render_target(obj); } while (0)

/* ---- the display ---- */
static int g_mode_w = 640, g_mode_h = 480, g_mode_bpp = 16, g_mode_set;
static int g_trace = -1;
static int trace(void) { if (g_trace < 0) g_trace = getenv("HLE_DDTRACE") != NULL; return g_trace; }

typedef struct surf {
    uint32_t self;
    int gpu_sync_lock;
    int w, h, bpp, pitch;
    uint8_t *px;
    uint8_t *own_px;                     /* the pixels allocated here (px may be the game's, or swapped by Flip) */
    uint32_t caps;
    int primary, locks;
    struct surf *back;                   /* the flip chain's next */
    struct surf *attached[8];            /* zbuffers / mip levels / textures: NOT the flip chain */
    int n_attached;
    uint32_t palette, clipper;
    int ck_src; uint32_t ck_lo, ck_hi;
    uint32_t pf_flags;                   /* DDPF_* of the requested format */
    uint32_t rmask, gmask, bmask, amask;
    int paletted;
} surf_t;

typedef struct { uint32_t entries[256]; int refs; } pal_t;   /* PALETTEENTRY: r, g, b, flags */

static surf_t *g_primary;
static uint32_t g_vt_dd, g_vt_dd2, g_vt_dd7, g_vt_surf, g_vt_pal, g_vt_clip;
static uint32_t g_dd, g_dd2, g_dd7;                             /* the one DirectDraw object, both faces */
static volatile unsigned g_frame_seq;                    /* bumped when the primary may have changed */

/* The host state behind an object is in guest memory too (hle_alloc), so
 * the object holds its VA at +8. */
#define S(self) ((surf_t *)hle_ptr(MEM32((self) + 8)))
#define P(self) ((pal_t *)hle_ptr(MEM32((self) + 8)))

/* What the presenter shows: the primary's pixels and their format. 0 if
 * there is no primary yet. */
int hle_dd_frame(const uint8_t **px, int *w, int *h, int *pitch, int *bpp, const uint32_t **palette, unsigned *seq) {
    surf_t *s = g_primary;
    if (!s) return 0;
    *px = s->px, *w = s->w, *h = s->h, *pitch = s->pitch, *bpp = s->bpp;
    *palette = s->palette ? P(s->palette)->entries : NULL;
    if (seq) *seq = g_frame_seq;
    return 1;
}
/* Host video frame -> primary. The MAD movie player presents decoded frames
 * here while the guest thread is blocked inside its movie-file open. Nearest
 * scaling if the movie is not the primary size; fires the primary hook so the
 * screen layer / screenshot capture sees the frame. */
int hle_dd_push_565(const uint16_t *px, int w, int h) {
    surf_t *s = g_primary;
    if (!s || !px || w <= 0 || h <= 0 || s->bpp != 16) return 0;
    uint8_t *dst = s->px;
    for (int y = 0; y < s->h; y++) {
        int sy = (int)((long long)y * h / s->h);
        const uint16_t *src = px + (size_t)sy * (size_t)w;
        uint16_t *row = (uint16_t *)(dst + (size_t)y * (size_t)s->pitch);
        if (w == s->w) memcpy(row, src, (size_t)s->w * 2u);
        else for (int x = 0; x < s->w; x++) row[x] = src[(int)((long long)x * w / s->w)];
    }
    g_frame_seq++;
    if (hle_dd_primary_hook) hle_dd_primary_hook();
    return 1;
}

/* A surface object's pixels, for a host that reads the game's surfaces
 * (an HD layer, a recorder): 0 if obj is not one of ours. */
int hle_dd_surface_pixels(uint32_t obj, uint8_t **px, int *w, int *h, int *pitch, int *bpp) {
    if (!obj || !g_vt_surf || MEM32(obj) != g_vt_surf) return 0;
    surf_t *s = S(obj);
    *px = s->px, *w = s->w, *h = s->h, *pitch = s->pitch, *bpp = s->bpp;
    return 1;
}
void hle_dd_mode(int *w, int *h, int *bpp) { *w = g_mode_w, *h = g_mode_h, *bpp = g_mode_bpp; }
/* A surface's pixel format as created: the requested masks/flags are preserved,
 * so 565 vs 1555 vs 4444 vs 8888 vs palettized is known. 0 if not ours. */
int hle_dd_surface_format(uint32_t obj, int *bpp, uint32_t *rmask, uint32_t *gmask, uint32_t *bmask,
                          uint32_t *amask, int *paletted) {
    if (!obj || !g_vt_surf || MEM32(obj) != g_vt_surf) return 0;
    surf_t *s = S(obj);
    if (bpp) *bpp = s->bpp;
    if (rmask) *rmask = s->rmask;
    if (gmask) *gmask = s->gmask;
    if (bmask) *bmask = s->bmask;
    if (amask) *amask = s->amask;
    if (paletted) *paletted = s->paletted;
    return 1;
}
int hle_dd_surface_caps(uint32_t obj, uint32_t *caps) {
    if (!obj || !g_vt_surf || MEM32(obj) != g_vt_surf) return 0;
    if (caps) *caps = S(obj)->caps;
    return 1;
}

static void masks(int bpp, uint32_t *r, uint32_t *g, uint32_t *b) {
    if (bpp == 16) *r = 0xF800, *g = 0x07E0, *b = 0x001F;
    else if (bpp >= 24) *r = 0xFF0000, *g = 0x00FF00, *b = 0x0000FF;
    else *r = *g = *b = 0;
}
static void put_pf(uint32_t pf, int bpp) {
    uint32_t r, g, b;
    masks(bpp, &r, &g, &b);
    memset(hle_ptr(pf), 0, 32);
    MEM32(pf) = 32;
    MEM32(pf + 4) = bpp == 8 ? DDPF_RGB | DDPF_PALETTEINDEXED8 : DDPF_RGB;
    MEM32(pf + 12) = (uint32_t)bpp, MEM32(pf + 16) = r, MEM32(pf + 20) = g, MEM32(pf + 24) = b;
}
static void put_pf_desc(uint32_t pf, const surf_t *s) {
    memset(hle_ptr(pf), 0, 32);
    MEM32(pf) = 32;
    MEM32(pf + 4) = s->pf_flags ? s->pf_flags
                                : (s->bpp == 8 ? DDPF_RGB | DDPF_PALETTEINDEXED8 : DDPF_RGB);
    MEM32(pf + 12) = (uint32_t)s->bpp;
    MEM32(pf + 16) = s->rmask, MEM32(pf + 20) = s->gmask, MEM32(pf + 24) = s->bmask, MEM32(pf + 28) = s->amask;
}
static void put_desc(uint32_t d, surf_t *s) {
    uint32_t size = MEM32(d) >= 108 ? MEM32(d) : 108;
    memset(hle_ptr(d + 4), 0, size - 4);
    MEM32(d + 4) = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_LPSURFACE;
    MEM32(d + 8) = (uint32_t)s->h, MEM32(d + 12) = (uint32_t)s->w, MEM32(d + 16) = (uint32_t)s->pitch;
    MEM32(d + 36) = hle_va(s->px);
    if (s->ck_src) MEM32(d + 4) |= DDSD_CKSRCBLT, MEM32(d + 64) = s->ck_lo, MEM32(d + 68) = s->ck_hi;
    put_pf_desc(d + 72, s);
    MEM32(d + 104) = s->caps;
    if (s->caps & DDSCAPS_BACKBUFFER) MEM32(d + 104) |= DDSCAPS_BACKBUFFER;
}

static uint32_t new_surface(int w, int h, int bpp, uint32_t caps, int primary,
                            uint32_t pff, uint32_t rmask, uint32_t gmask, uint32_t bmask, uint32_t amask) {
    surf_t *s = (surf_t *)hle_alloc(sizeof *s);
    s->w = w, s->h = h, s->bpp = bpp, s->caps = caps, s->primary = primary;
    s->pitch = ((w * (bpp / 8)) + 3) & ~3;
    s->px = s->own_px = (uint8_t *)hle_alloc((uint32_t)s->pitch * (uint32_t)h + SURF_TAIL);
    if (!s->px) hle_fatal("no guest memory for a %dx%d surface", w, h);
    s->pf_flags = pff, s->paletted = (pff & DDPF_PALETTEINDEXED8) != 0;
    masks(bpp, &s->rmask, &s->gmask, &s->bmask);
    if (rmask || gmask || bmask) s->rmask = rmask, s->gmask = gmask, s->bmask = bmask;   /* the game's format wins */
    s->amask = amask;
    uint32_t o = hle_com_new(g_vt_surf, 12);
    MEM32(o + 8) = hle_va(s);
    s->self = o;
    return o;
}
static void free_surface(surf_t *s) {
    if (g_primary == s) g_primary = NULL, g_frame_seq++;
    for (int i = 0; i < s->n_attached; i++) {
        surf_t *a = s->attached[i];
        if (!a || !a->self) continue;
        uint32_t aself = a->self;
        if (--MEM32(aself + 4) == 0) { free_surface(a); hle_com_free(aself); }
    }
    if (s->palette) { uint32_t p = s->palette; if (--MEM32(p + 4) == 0) { hle_free(P(p)); hle_com_free(p); } }
    hle_free(s->own_px);
    hle_free(s);
}

/* ---- IDirectDraw ---- */
static uint32_t g_vt_gamma, g_gamma, g_vt_d3d3, g_d3d3;
static void vtables(void);
static void dd_QueryInterface(void) {                    /* (this, iid, &out) */
    const uint8_t *iid = (const uint8_t *)APTR(1);
    uint32_t d1 = *(const uint32_t *)iid;
    char s[40];
    hle_guid_string(iid, s);
    if (trace()) fprintf(stderr, "[ddraw] IDirectDraw::QueryInterface %s\n", s);
    if (d1 == 0x6C14DB80u || d1 == 0x00000000u) { MEM32(A32(2)) = g_dd, MEM32(g_dd + 4)++; RET(DD_OK, 3); }    /* IDirectDraw, IUnknown */
    if (d1 == 0xB3A6F3E0u) { MEM32(A32(2)) = g_dd2, MEM32(g_dd2 + 4)++; RET(DD_OK, 3); }                       /* IDirectDraw2 */
    if (d1 == 0x9C59509Au) { MEM32(A32(2)) = g_dd7, MEM32(g_dd7 + 4)++; RET(DD_OK, 3); }                       /* IDirectDraw4: the DD7 face is its prefix */
    if (d1 == 0x15E65EC0u) { vtables(); MEM32(A32(2)) = g_dd7, MEM32(g_dd7 + 4)++; RET(DD_OK, 3); }            /* IDirectDraw7 */
    {   /* IDirect3D7 and the rest of the DX7 renderer's interfaces (runtime/d3d7.c) */
        uint32_t extra = D3D7_QI(iid);
        if (extra) { MEM32(A32(2)) = extra; MEM32(extra + 4)++; RET(DD_OK, 3); }
    }
    if (d1 == 0x69C11C3Eu) {   /* IID_IDirectDrawGammaControl */
        vtables();
        MEM32(A32(2)) = g_gamma, MEM32(g_gamma + 4)++;
        RET(DD_OK, 3);
    }
    if (d1 == 0xBB223240u) {   /* IID_IDirect3D3: dx7z's SetDesktopMode probe
                                * (dx7init.c) enumerates Z formats through it */
        vtables();
        MEM32(A32(2)) = g_d3d3, MEM32(g_d3d3 + 4)++;
        if (trace()) fprintf(stderr, "[ddraw] IDirect3D3 QueryInterface -> probe object %#x\n", g_d3d3);
        RET(DD_OK, 3);
    }
    fprintf(stderr, "[ddraw] QueryInterface(%s): not served\n", s);
    MEM32(A32(2)) = 0;
    RET(0x80004002u, 3);
}
static void dd_Release(void) { uint32_t n = MEM32(A32(0) + 4) ? --MEM32(A32(0) + 4) : 0; RET(n, 1); }   /* the object lives on */
static void dd_Compact(void) { RET(DD_OK, 1); }
static void dd_CreateClipper(void) {                     /* (this, flags, &out, outer) */
    uint32_t o = hle_com_new(g_vt_clip, 12);
    MEM32(A32(2)) = o;
    RET(DD_OK, 4);
}
static void dd_CreatePalette(void) {                     /* (this, flags, entries, &out, outer) */
    pal_t *p = (pal_t *)hle_alloc(sizeof *p);
    if (A32(2)) memcpy(p->entries, APTR(2), 256 * 4);
    uint32_t o = hle_com_new(g_vt_pal, 12);
    MEM32(o + 8) = hle_va(p);
    MEM32(A32(3)) = o;
    RET(DD_OK, 5);
}
static void dd_CreateSurface(void) {                     /* (this, &DDSURFACEDESC(2), &out, outer) */
    uint32_t d = A32(1), fl = MEM32(d + 4), caps = fl & DDSD_CAPS ? MEM32(d + 104) : 0;
    int primary = (caps & DDSCAPS_PRIMARYSURFACE) != 0;
    int zbuffer = (caps & DDSCAPS_ZBUFFER) != 0;
    int texture = (caps & DDSCAPS_TEXTURE) != 0;
    int w = primary ? g_mode_w : (int)MEM32(d + 12), h = primary ? g_mode_h : (int)MEM32(d + 8);
    /* the pixel format is kept as the game asked for it: 565 vs 1555 vs 4444
     * vs 8888, palettized, or a Z buffer's format */
    uint32_t pff = 0, rm = 0, gm = 0, bm = 0, am = 0;
    int bpp = primary ? g_mode_bpp : g_mode_bpp;
    if (fl & DDSD_PIXELFORMAT) {
        pff = MEM32(d + 72 + 4);
        bpp = (int)MEM32(d + 72 + 12);
        rm = MEM32(d + 72 + 16), gm = MEM32(d + 72 + 20), bm = MEM32(d + 72 + 24), am = MEM32(d + 72 + 28);
    }
    if (bpp <= 0) bpp = g_mode_bpp;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)) {
        fprintf(stderr, "[ddraw] CreateSurface %dx%d %dbpp: no\n", w, h, bpp);
        RET(DDERR_INVALIDPARAMS, 4);
    }
    if (zbuffer) pff |= DDPF_ZBUFFER;
    /* The pixels are host memory, but the caps are the game's: a texture or a
     * 3D render target keeps TEXTURE/3DDEVICE/ZBUFFER/MIPMAP, and video-memory
     * surfaces keep VIDEOMEMORY (a game gates its accelerated path on it). */
    if (!(caps & (DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM))) caps |= DDSCAPS_SYSTEMMEMORY;
    if (!primary && !zbuffer && !texture && !(caps & DDSCAPS_3DDEVICE)) caps |= DDSCAPS_OFFSCREENPLAIN;
    uint32_t o = new_surface(w, h, bpp, primary ? caps | DDSCAPS_VISIBLE | DDSCAPS_FRONTBUFFER : caps, primary,
                             pff, rm, gm, bm, am);
    surf_t *s = S(o);
    if (fl & DDSD_CKSRCBLT) s->ck_src = 1, s->ck_lo = MEM32(d + 64), s->ck_hi = MEM32(d + 68);
    if (fl & DDSD_LPSURFACE && MEM32(d + 36)) {           /* the game's own memory */
        hle_free(s->own_px);
        s->own_px = NULL;
        s->px = (uint8_t *)hle_ptr(MEM32(d + 36));
        if (fl & DDSD_PITCH) s->pitch = (int)MEM32(d + 16);
    }
    if (primary) {
        /* The display is the newest primary that has a flip chain; without a
         * chain anywhere, the newest primary. dx7z makes its own primary with
         * a back buffer, and that is the one the 3D frames go to. */
        if ((fl & DDSD_BACKBUFFERCOUNT) && MEM32(d + 20)) {
            uint32_t b = new_surface(w, h, bpp, DDSCAPS_BACKBUFFER | (caps & (DDSCAPS_FLIP | DDSCAPS_COMPLEX |
                             DDSCAPS_VIDEOMEMORY | DDSCAPS_SYSTEMMEMORY)) | DDSCAPS_SYSTEMMEMORY, 0, pff, rm, gm, bm, am);
            s->back = S(b);
            S(b)->back = s;
        }
        if (s->back || !g_primary || !g_primary->back) {
            g_primary = s;
            g_frame_seq++;
        }
    }
    /* A texture created with DDSD_MIPMAPCOUNT gets its attached mip chain.
     * dx7z's THRASH mip fill (guest 0x1000C390) walks the chain with
     * GetAttachedSurface(caps=TEXTURE|MIPMAP) and does NOT check the HRESULT:
     * with no chain our NOTFOUND left the out pointer 0 and the next access
     * was a guest NULL read (fault at guest_base+0). Levels are owned by their
     * parent (count 1; free_surface drops them recursively), and
     * GetAttachedSurface AddRefs the level it hands out to the caller. */
    if ((fl & DDSD_MIPMAPCOUNT) && texture) {
        int levels = (int)MEM32(d + 24);
        surf_t *cur = s;
        for (int i = 1; i < levels && cur->n_attached < (int)(sizeof cur->attached / sizeof cur->attached[0]); i++) {
            int mw = w >> i, mh = h >> i;
            if (mw < 1) mw = 1;
            if (mh < 1) mh = 1;
            uint32_t mo = new_surface(mw, mh, bpp,
                                      (caps & ~(DDSCAPS_VISIBLE | DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER)) | DDSCAPS_MIPMAP,
                                      0, pff, rm, gm, bm, am);
            cur->attached[cur->n_attached++] = S(mo);
            cur = S(mo);
        }
        if (levels > 1 && trace())
            fprintf(stderr, "[ddraw] mip chain %dx%d levels=%d for surf 0x%08X\n", w, h, levels - 1, o);
    }
    if (trace() || primary)
        fprintf(stderr, "[ddraw] CreateSurface %dx%d %d bpp caps 0x%X pf 0x%X%s%s -> 0x%08X%s\n", w, h, bpp, caps, pff,
                primary ? " primary" : "", zbuffer ? " zbuffer" : texture ? " texture" : "", o,
                s == g_primary ? " (display)" : "");
    MEM32(A32(2)) = o;
    RET(DD_OK, 4);
}
static void dd_DuplicateSurface(void) { RET(DDERR_UNSUPPORTED, 3); }

/* The modes a game is offered: the common 4:3, 16:9 and 16:10 sizes up to 4K
 * and the usual ultrawides, and the display's own size (its logical size on
 * a Retina Mac), which the window shows at whole pixels. Nothing past
 * 3840x2160, the largest the games here are tested at. */
/* Port policy for the target device: the RG40XX-H panel is 640x480, and
 * rendering above it wastes the GPU and memory (NFS5 otherwise picks the
 * largest enumerated mode - it took 3840x2160 on a 4K desktop, which is not a
 * mode the handheld can present). We advertise exactly one mode: 640x480 by
 * default, overridable with NFS5_VIDEO_MODE=WIDTHxHEIGHT for testing. */
static void port_mode(int *w, int *h) {
    const char *v = getenv("NFS5_VIDEO_MODE");
    int pw = 640, ph = 480;
    if (v && *v) {
        int a = 0, b = 0;
        if (sscanf(v, "%dx%d", &a, &b) == 2 && a >= 320 && b >= 240 && a <= 1920 && b <= 1080) pw = a, ph = b;
    }
    *w = pw, *h = ph;
}
int hle_dd_modes(int dw, int dh, int (*out)[2]) {
    (void)dw; (void)dh;
    port_mode(&out[0][0], &out[0][1]);
    return 1;
}
static void dd_EnumDisplayModes(void) {                  /* (this, flags, desc filter, ctx, callback) */
    uint32_t cb = A32(4), filter = A32(2), ctx = A32(3), mark = hle_scratch_mark();
    uint32_t dva = hle_scratch(27 * 4);
    uint32_t *d = (uint32_t *)hle_ptr(dva);
    /* Exactly the port's mode, 16 bpp, plus 320x240x16 as the low fallback.
     * Advertising the host desktop size made the game pick 3840x2160 on a 4K
     * desktop (mode chosen by the game itself through GetDisplayMode/desktop
     * probing and a SetDisplayMode we used to accept verbatim). */
    int modes[HLE_DD_MAX_MODES][2] = { { 0, 0 }, { 320, 240 } };
    port_mode(&modes[0][0], &modes[0][1]);
    int n = modes[0][0] == 320 && modes[0][1] == 240 ? 1 : 2;
    const int bpps[2] = { 16, 16 };
    for (int bi = 0; bi < 1; bi++) {
        int bpp = bpps[bi];
        for (int i = 0; i < n; i++) {
            if (filter && (MEM32(filter + 4) & DDSD_WIDTH) && MEM32(filter + 12) != (uint32_t)modes[i][0]) continue;
            if (filter && (MEM32(filter + 4) & DDSD_HEIGHT) && MEM32(filter + 8) != (uint32_t)modes[i][1]) continue;
            if (filter && (MEM32(filter + 4) & DDSD_PIXELFORMAT) && MEM32(filter + 84) != (uint32_t)bpp) continue;
            memset(d, 0, 27 * 4);
            d[0] = 108;
            d[1] = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
            d[2] = (uint32_t)modes[i][1], d[3] = (uint32_t)modes[i][0], d[4] = (uint32_t)(modes[i][0] * bpp / 8);
            d[6] = 60;
            put_pf(dva + 18 * 4, bpp);
            uint32_t a[2] = { dva, ctx }, more = hle_call_guest(cb, 2, a);
            if (trace()) fprintf(stderr, "[ddraw] EnumDisplayModes -> 0x%08X: %dx%dx%d\n", cb, modes[i][0], modes[i][1], bpp);
            if (!more) { hle_scratch_release(mark); RET(DD_OK, 5); }   /* DDENUMRET_CANCEL */
        }
    }
    hle_scratch_release(mark);
    RET(DD_OK, 5);
}
static void dd_EnumSurfaces(void) { RET(DD_OK, 5); }
static void dd_FlipToGDISurface(void) { RET(DD_OK, 1); }
static void dd_GetCaps(void) {                           /* (this, &driver, &hel) */
    for (int k = 1; k <= 2; k++) {
        uint32_t c = A32(k);
        if (!c) continue;
        uint32_t size = MEM32(c) ? MEM32(c) : 316;
        if (size < 16) size = 16;
        memset(hle_ptr(c + 4), 0, size - 4);
        /* the blit/fill capabilities dx7z gates its accelerated path on */
        MEM32(c + 4) = 0x00000001u | 0x00000040u | 0x00000200u | 0x00100000u | 0x00400000u |
                       0x04000000u | 0x20000000u | 0x00008000u;   /* 3D, BLT, BLTSTRETCH, ZBLTS, COLORKEY,
                                                                     * BLTCOLORFILL, CANCLIP, PALETTE */
        MEM32(c + 8) = 0x00000200u;                      /* DDCAPS2_NONLOCALVIDMEM */
        MEM32(c + 12) = 0x00000200u;                     /* DDCKEYCAPS_SRCBLT */
        MEM32(c + 24) = 0x00000004u | 0x00000040u;       /* DDPCAPS_8BIT, PRIMARYSURFACE */
        MEM32(c + 56) = 0x00000400u | 0x00000100u;       /* DDBD_16, DDBD_32 z-buffer depths */
        MEM32(c + 60) = 256u << 20, MEM32(c + 64) = 192u << 20;   /* video memory: 256 MB, 192 free */
        /* ddsCaps is the struct's last field in every DDCAPS version */
        MEM32(c + size - 8) = DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_FLIP | DDSCAPS_COMPLEX |
                              DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY | DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER |
                              DDSCAPS_3DDEVICE | DDSCAPS_MIPMAP;
        MEM32(c + size - 4) = 0;                         /* DDSCAPS2 */
    }
    RET(DD_OK, 3);
}
static void dd_GetDisplayMode(void) {                    /* (this, &desc) */
    uint32_t d = A32(1);
    uint32_t size = MEM32(d) >= 108 ? MEM32(d) : 108;
    memset(hle_ptr(d + 4), 0, size - 4);
    MEM32(d + 4) = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    MEM32(d + 8) = (uint32_t)g_mode_h, MEM32(d + 12) = (uint32_t)g_mode_w, MEM32(d + 16) = (uint32_t)(g_mode_w * g_mode_bpp / 8);
    MEM32(d + 24) = 60;
    put_pf(d + 72, g_mode_bpp);
    RET(DD_OK, 2);
}
static void dd_GetFourCCCodes(void) { if (A32(1)) MEM32(A32(1)) = 0; RET(DD_OK, 3); }
static void dd_GetGDISurface(void) { if (!g_primary) RET(DDERR_NOTFOUND, 2); MEM32(A32(1)) = g_primary->self; MEM32(g_primary->self + 4)++; RET(DD_OK, 2); }
static void dd_GetMonitorFrequency(void) { if (A32(1)) MEM32(A32(1)) = 60; RET(DD_OK, 2); }
static void dd_GetScanLine(void) { if (A32(1)) MEM32(A32(1)) = (hle_ticks_ms() * 29) % (uint32_t)g_mode_h; RET(DD_OK, 2); }
static void dd_GetVerticalBlankStatus(void) { if (A32(1)) MEM32(A32(1)) = (hle_ticks_ms() % 16) == 0; RET(DD_OK, 2); }
static void dd_Initialize(void) { RET(DD_OK, 2); }
static void dd_RestoreDisplayMode(void) { RET(DD_OK, 1); }
static void dd_SetCooperativeLevel(void) { if (trace()) fprintf(stderr, "[ddraw] SetCooperativeLevel(0x%X)\n", A32(2)); RET(DD_OK, 3); }
/* Only the port's own modes are real: the target panel is 640x480 and a mode
 * above it wastes GPU/memory (the game asked for the 4K desktop once the host
 * advertised it). A request outside the set is accepted (DD_OK) but the actual
 * mode is clamped, which is what the primary surface and the renderer then
 * use; NFS5_VIDEO_MODE overrides the port mode. */
static void set_mode(int w, int h, int bpp) {
    int pw, ph;
    port_mode(&pw, &ph);
    if ((w != pw || h != ph) && !(w == 320 && h == 240)) {
        fprintf(stderr, "[ddraw] SetDisplayMode(%dx%dx%d): not a port mode, forcing %dx%dx16\n", w, h, bpp, pw, ph);
        w = pw, h = ph, bpp = 16;
    } else if (bpp != 16) {
        fprintf(stderr, "[ddraw] SetDisplayMode(%dx%dx%d): forcing 16 bpp\n", w, h, bpp);
        bpp = 16;
    }
    g_mode_w = w, g_mode_h = h, g_mode_bpp = bpp, g_mode_set = 1;
    hle_set_screen_size(w, h);
    fprintf(stderr, "[ddraw] SetDisplayMode(%dx%dx%d)\n", w, h, bpp);
}
static void dd_SetDisplayMode(void)  { set_mode((int)A32(1), (int)A32(2), (int)A32(3)); RET(DD_OK, 4); }
static void dd2_SetDisplayMode(void) { set_mode((int)A32(1), (int)A32(2), (int)A32(3)); RET(DD_OK, 6); }
static void dd_WaitForVerticalBlank(void) {              /* (this, flags, event): the next 60 Hz tick */
    uint32_t now = hle_ticks_ms(), wait = 16 - now % 16;
    hle_block_begin();
    struct timespec ts = { 0, (long)wait * 1000000L };
    nanosleep(&ts, NULL);
    hle_block_end();
    RET(DD_OK, 3);
}
static void dd2_GetAvailableVidMem(void) {               /* (this, &caps, &total, &free) */
    if (A32(2)) MEM32(A32(2)) = 256u << 20;
    if (A32(3)) MEM32(A32(3)) = 192u << 20;
    RET(DD_OK, 4);
}
/* ---- IDirectDrawGammaControl {69c11c3e-b46b-11d1-ad7a-00c04fc29b4e} ----
 * dx7z asks for it optionally; keep a 256-entry ramp (r=g=b linear by
 * default) so a game that applies gamma gets a sane answer. A future GLES
 * consumer can read gamma_ramp() and apply it. */
static uint16_t g_gammatab[3][256];
const uint16_t *hle_dd_gamma_ramp(int channel) { return g_gammatab[channel & 3]; }
/* The gamma-control interface is a persistent singleton in real DirectDraw:
 * dx7z QIs it for the primary surface once, keeps the pointer in a global and
 * uses it again on later display-swap notifications, sometimes after releasing
 * it once. Freeing it on Release let the guest heap reuse the block for a new
 * surface, so dx7z's saved pointer then had a surface vtable: at the first
 * race load it called vtable[3] as GetGammaRamp(this, flags, ramp) but got the
 * surface's AddAttachedSurface(this, surf) with three arguments pushed, which
 * left 4 bytes on the guest stack and crashed at guest 0x10003300 (write to
 * [lParam]). Keep the singleton allocated for the guest lifetime; the internal
 * count still tracks AddRef/Release. */
static void gm_Release(void) {
    uint32_t self = A32(0), n = MEM32(self + 4);
    if (n) n--;
    MEM32(self + 4) = n;
    RET(n, 1);
}
static void gm_GetGammaRamp(void) {                      /* (this, flags, &DDGAMMARAMP) */
    uint32_t p = A32(2);
    if (p) {
        for (int i = 0; i < 256; i++) {
            MEM16(p + (uint32_t)i * 2) = g_gammatab[0][i];
            MEM16(p + 512 + (uint32_t)i * 2) = g_gammatab[1][i];
            MEM16(p + 1024 + (uint32_t)i * 2) = g_gammatab[2][i];
        }
    }
    RET(DD_OK, 3);
}
static void gm_SetGammaRamp(void) {                      /* (this, flags, &DDGAMMARAMP) */
    uint32_t p = A32(2);
    if (p) {
        for (int i = 0; i < 256; i++) {
            g_gammatab[0][i] = MEM16(p + (uint32_t)i * 2);
            g_gammatab[1][i] = MEM16(p + 512 + (uint32_t)i * 2);
            g_gammatab[2][i] = MEM16(p + 1024 + (uint32_t)i * 2);
        }
        if (trace()) fprintf(stderr, "[ddraw] SetGammaRamp accepted\n");
    }
    RET(DD_OK, 3);
}
/* ---- IDirect3D3 fallback {bb223240-e72b-11d0-a9b4-00aa00c0993e} ----
 * dx7z's SetDesktopMode probe QIs IDirectDraw for IDirect3D3 and calls
 * EnumZBufferFormats(&IID_IDirect3DHALDevice, callback) (dx7init.c around
 * 0x1000228e) to pick a Z-buffer depth; refusing it aborts startup. The real
 * IDirect3D7 face is NOT a prefix of IDirect3D3 (Direct3D7 dropped
 * light/material/viewport and moved CreateDevice), so this needs its own
 * 12-slot vtable. Device creation stays the renderer's IDirect3D7 path; the
 * probe is answered here. Replace this with the renderer's IDirect3D3 when it
 * has one: the QI hook is tried first. */
static void d3_QueryInterface(void) {
    uint32_t d1 = A32(1) ? MEM32(A32(1)) : 0;
    if (d1 == 0 || d1 == 0xBB223240u) { MEM32(A32(2)) = A32(0); MEM32(A32(0) + 4)++; RET(DD_OK, 3); }
    if (A32(2)) MEM32(A32(2)) = 0;
    RET(0x80004002u, 3);
}
static void d3_EnumDevices(void) { RET(DD_OK, 3); }      /* no enumeration here: the 3D device comes from IDirect3D7 */
static void d3_notimpl2(void) { RET(DDERR_UNSUPPORTED, 3); }
static void d3_ok_1(void) { RET(DD_OK, 1); }
static void d3_CreateDevice(void) { if (A32(2)) MEM32(A32(2)) = 0; RET(DDERR_UNSUPPORTED, 5); }   /* this+rclsid+surface+out+outer */
static void d3_CreateVertexBuffer(void) { if (A32(2)) MEM32(A32(2)) = 0; RET(DDERR_UNSUPPORTED, 5); }   /* this+desc+out+flags+outer */
static void d3_EnumZBufferFormats(void) {                /* (this, &deviceIID, callback, ctx) */
    uint32_t cb = A32(2), ctx = A32(3);
    static const uint32_t depths[2] = { 16, 32 };
    if (!cb) RET(DDERR_INVALIDPARAMS, 4);
    uint32_t mark = hle_scratch_mark();
    for (int i = 0; i < 2; i++) {
        uint32_t pf = hle_scratch(32);
        memset(hle_ptr(pf), 0, 32);
        MEM32(pf + 0) = 32;
        MEM32(pf + 4) = 0x00000400u;                     /* DDPF_ZBUFFER */
        MEM32(pf + 12) = depths[i];                      /* dwZBufferBitDepth (union with dwRGBBitCount) */
        MEM32(pf + 16) = depths[i] == 16 ? 0x0000FFFFu : 0xFFFFFFFFu;   /* dwZBitMask */
        uint32_t a[2] = { pf, ctx };
        uint32_t more = hle_call_guest(cb, 2, a);
        if (trace()) fprintf(stderr, "[ddraw] IDirect3D3::EnumZBufferFormats -> %u bpp z\n", depths[i]);
        if (!more) break;                                /* D3DENUMRET_CANCEL */
    }
    hle_scratch_release(mark);
    RET(DD_OK, 4);
}

/* ---- IDirectDraw4/7 additions (slots 24-29) ---- */
static void dd7_GetSurfaceFromDC(void) {                 /* (this, hdc, &out) */
    if (A32(2)) MEM32(A32(2)) = 0;
    RET(DDERR_NOTFOUND, 3);
}
static void dd7_RestoreAllSurfaces(void) { RET(DD_OK, 1); }
static void dd7_TestCooperativeLevel(void) { RET(DD_OK, 1); }
static void dd7_GetDeviceIdentifier(void) {              /* (this, &DDDEVICEIDENTIFIER, flags) */
    uint32_t p = A32(1);
    if (!p) RET(DDERR_INVALIDPARAMS, 3);
    memset(hle_ptr(p), 0, 1064);
    snprintf((char *)hle_ptr(p), 512, "display");
    snprintf((char *)hle_ptr(p + 512), 512, "SDL2 software display");
    MEM32(p + 1024) = 0, MEM32(p + 1028) = 0x00050000u;   /* liDriverVersion 5.0 */
    RET(DD_OK, 3);
}
static void dd7_StartModeTest(void) { RET(DD_OK, 4); }    /* modes accepted: evaluate comes back immediate */
static void dd7_EvaluateMode(void) {                      /* (this, flags, &timeout) */
    if (A32(2)) MEM32(A32(2)) = 0;
    RET(DD_OK, 3);
}

/* ---- IDirectDrawSurface ---- */
static void sf_QueryInterface(void) {
    const uint8_t *iid = (const uint8_t *)APTR(1);
    uint32_t d1 = *(const uint32_t *)iid;
    /* IUnknown, Surface, Surface2/3/4/7: the same object (the vtable is a
     * prefix of the Surface7 layout, and dx7z only calls slots <= 32) */
    if (d1 == 0 || d1 == 0x6C14DB81u || d1 == 0x57805885u || d1 == 0xDA044E00u ||
        d1 == 0x0B2B8630u || d1 == 0x06675A80u) {
        MEM32(A32(2)) = A32(0); MEM32(A32(0) + 4)++;
        RET(DD_OK, 3);
    }
    if (d1 == 0x69C11C3Eu) {   /* IID_IDirectDrawGammaControl: dx7z asks the primary surface */
        vtables();
        MEM32(A32(2)) = g_gamma, MEM32(g_gamma + 4)++;
        RET(DD_OK, 3);
    }
    char s[40];
    hle_guid_string(iid, s);
    fprintf(stderr, "[ddraw] Surface::QueryInterface(%s): not served\n", s);
    MEM32(A32(2)) = 0;
    RET(0x80004002u, 3);
}
static void sf_Release(void) {
    uint32_t self = A32(0), n = --MEM32(self + 4);
    if (!n) {
        surf_t *s = S(self);
        if (s->back && s->back->back == s && s->primary) {
            /* Release of the front detaches the flip chain, but the back buffer
             * only dies with its own last reference: dx7z caches the back
             * buffer's Surface7 interface (its own AddRefs) and releases the
             * front independently, so freeing the back unconditionally here
             * left dx7z a stale interface pointer (then its later Release/AddRef
             * landed in reused/freed guest memory). */
            surf_t *b = s->back;
            uint32_t bo = b->self;
            s->back = NULL;
            b->back = NULL;
            if (MEM32(bo + 4) && --MEM32(bo + 4) == 0) { free_surface(b); hle_com_free(bo); }
        }
        free_surface(s);
        hle_com_free(self);
    }
    RET(n, 1);
}
static void sf_AddAttachedSurface(void) {
    surf_t *s = S(A32(0)), *b = A32(1) ? S(A32(1)) : NULL;
    if (!b || b == s) RET(DDERR_INVALIDPARAMS, 2);
    if (b->caps & DDSCAPS_BACKBUFFER) {                   /* the flip chain */
        s->back = b;
    } else {                                              /* zbuffer / texture / mip level */
        for (int i = 0; i < s->n_attached; i++) if (s->attached[i] == b) RET(DD_OK, 2);
        if (s->n_attached >= (int)(sizeof s->attached / sizeof s->attached[0])) RET(DDERR_GENERIC, 2);
        s->attached[s->n_attached++] = b;
    }
    MEM32(A32(1) + 4)++;
    if (trace()) fprintf(stderr, "[ddraw] AddAttachedSurface %#x <- %#x (caps 0x%X)\n", A32(0), A32(1), b->caps);
    RET(DD_OK, 2);
}
static void sf_AddOverlayDirtyRect(void) { RET(DDERR_UNSUPPORTED, 2); }

static inline uint32_t get_px(const surf_t *s, int x, int y) {
    const uint8_t *p = s->px + (size_t)y * (size_t)s->pitch + (size_t)x * (size_t)(s->bpp / 8);
    return s->bpp == 16 ? *(const uint16_t *)p : s->bpp == 8 ? *p : s->bpp == 32 ? *(const uint32_t *)p : (uint32_t)(p[0] | p[1] << 8 | p[2] << 16);
}
static inline void put_px(surf_t *s, int x, int y, uint32_t v) {
    uint8_t *p = s->px + (size_t)y * (size_t)s->pitch + (size_t)x * (size_t)(s->bpp / 8);
    if (s->bpp == 16) *(uint16_t *)p = (uint16_t)v;
    else if (s->bpp == 8) *p = (uint8_t)v;
    else if (s->bpp == 32) *(uint32_t *)p = v;
    else p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16);
}
static void get_rect(uint32_t r, const surf_t *s, int *l, int *t, int *rr, int *b) {
    if (r) *l = (int)MEM32(r), *t = (int)MEM32(r + 4), *rr = (int)MEM32(r + 8), *b = (int)MEM32(r + 12);
    else *l = 0, *t = 0, *rr = s->w, *b = s->h;
}
/* On Windows the primary is the screen: what the game puts there shows at
 * once, pumping messages or not. A presenter that wants the same hears of
 * each change here (screen.c). */
void (*hle_dd_primary_hook)(void);
static void mark(surf_t *d) {
    if (d != g_primary) return;
    if (getenv("NFS5_FRAMECLOCK")) {
        static unsigned n; static uint32_t t0;
        uint32_t now = hle_ticks_ms();
        if (!t0) t0 = now;
        if (++n % 500 == 0) fprintf(stderr, "[prim] %u primary marks %u ms -> %.2f/s\n", n, now - t0, n * 1000.0 / (now - t0));
    }
    g_frame_seq++;
    if (hle_dd_primary_hook) hle_dd_primary_hook();
}

/* The copy: same format at both ends (DirectDraw does not convert either),
 * clipped to both surfaces, stretched nearest-neighbour when the sizes
 * differ, and keyed when a source colour key applies. */
static void blit(surf_t *d, int dl, int dt, int dr, int db, surf_t *s, int sl, int st, int sr, int sb, int keyed, uint32_t klo, uint32_t khi) {
    int dw = dr - dl, dh = db - dt, sw = sr - sl, sh = sb - st;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    int bpp = d->bpp / 8;
    uint8_t *tmp = NULL;
    const uint8_t *spx = s->px;
    if (s == d) {                                        /* overlapping: copy the source out first */
        tmp = (uint8_t *)malloc((size_t)s->pitch * (size_t)s->h);
        memcpy(tmp, s->px, (size_t)s->pitch * (size_t)s->h);
        spx = tmp;
    }
    if (dw == sw && dh == sh && !keyed && s->bpp == d->bpp) {
        int x0 = dl < 0 ? -dl : 0, y0 = dt < 0 ? -dt : 0;
        if (sl + x0 < 0) x0 = -sl;
        if (st + y0 < 0) y0 = -st;
        int x1 = dw, y1 = dh;
        if (dl + x1 > d->w) x1 = d->w - dl;
        if (dt + y1 > d->h) y1 = d->h - dt;
        if (sl + x1 > s->w) x1 = s->w - sl;
        if (st + y1 > s->h) y1 = s->h - st;
        for (int y = y0; y < y1 && x1 > x0; y++)
            memcpy(d->px + (size_t)(dt + y) * (size_t)d->pitch + (size_t)(dl + x0) * (size_t)bpp,
                   spx + (size_t)(st + y) * (size_t)s->pitch + (size_t)(sl + x0) * (size_t)bpp, (size_t)(x1 - x0) * (size_t)bpp);
    } else {
        surf_t src = *s;
        src.px = (uint8_t *)spx;
        for (int y = 0; y < dh; y++) {
            int ty = dt + y, sy = st + y * sh / dh;
            if (ty < 0 || ty >= d->h || sy < 0 || sy >= s->h) continue;
            for (int x = 0; x < dw; x++) {
                int tx = dl + x, sx = sl + x * sw / dw;
                if (tx < 0 || tx >= d->w || sx < 0 || sx >= s->w) continue;
                uint32_t v = get_px(&src, sx, sy);
                if (keyed && v >= klo && v <= khi) continue;
                put_px(d, tx, ty, v);
            }
        }
    }
    free(tmp);
    mark(d);
}
static void fill(surf_t *d, int l, int t, int r, int b, uint32_t c) {
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > d->w) r = d->w;
    if (b > d->h) b = d->h;
    for (int y = t; y < b; y++) {
        if (d->bpp == 16) { uint16_t *p = (uint16_t *)(d->px + (size_t)y * (size_t)d->pitch) + l; for (int x = l; x < r; x++) *p++ = (uint16_t)c; }
        else if (d->bpp == 8) memset(d->px + (size_t)y * (size_t)d->pitch + l, (int)(c & 0xFF), (size_t)(r > l ? r - l : 0));
        else for (int x = l; x < r; x++) put_px(d, x, y, c);
    }
    mark(d);
}
static void sf_Blt(void) {                               /* (this, dstRect, src, srcRect, flags, fx) */
    surf_t *d = S(A32(0)), *s = A32(2) ? S(A32(2)) : NULL;
    uint32_t fl = A32(4), fx = A32(5);
    int dl, dt, dr, db;
    get_rect(A32(1), d, &dl, &dt, &dr, &db);
    if (fl & DDBLT_COLORFILL) {
        if(d3d7_fill_gpu && d3d7_fill_gpu(A32(0),fx?MEM32(fx+80):0,dl,dt,dr-dl,db-dt)) {mark(d);RET(DD_OK,6);}
        D3D7_FLUSH(A32(0));
        fill(d, dl, dt, dr, db, fx ? MEM32(fx + 80) : 0);
        RET(DD_OK, 6);
    }
    D3D7_FLUSH(A32(0));
    if(A32(2))D3D7_FLUSH(A32(2));
    if (!s) RET(DD_OK, 6);                               /* a raster op with no source: nothing to do here */
    int sl, st, sr, sb;
    get_rect(A32(3), s, &sl, &st, &sr, &sb);
    int keyed = 0;
    uint32_t lo = 0, hi = 0;
    if ((fl & DDBLT_KEYSRCOVERRIDE) && fx) keyed = 1, lo = MEM32(fx + 92), hi = MEM32(fx + 96);
    else if ((fl & DDBLT_KEYSRC) && s->ck_src) keyed = 1, lo = s->ck_lo, hi = s->ck_hi;
    if (hi < lo) hi = lo;
    blit(d, dl, dt, dr, db, s, sl, st, sr, sb, keyed, lo, hi);
    D3D7_DIRTY(A32(0));
    RET(DD_OK, 6);
}
static void sf_BltBatch(void) { RET(DDERR_UNSUPPORTED, 4); }
static void sf_BltFast(void) {                           /* (this, x, y, src, srcRect, trans) */
    D3D7_FLUSH(A32(0));
    if (A32(3)) D3D7_FLUSH(A32(3));
    surf_t *d = S(A32(0)), *s = A32(3) ? S(A32(3)) : NULL;
    if (!s) RET(DDERR_INVALIDPARAMS, 6);
    int sl, st, sr, sb, x = (int)A32(1), y = (int)A32(2);
    get_rect(A32(4), s, &sl, &st, &sr, &sb);
    int keyed = (A32(5) & DDBLTFAST_SRCCOLORKEY) && s->ck_src;
    uint32_t hi = s->ck_hi < s->ck_lo ? s->ck_lo : s->ck_hi;
    blit(d, x, y, x + (sr - sl), y + (sb - st), s, sl, st, sr, sb, keyed, s->ck_lo, hi);
    D3D7_DIRTY(A32(0));
    RET(DD_OK, 6);
}
static void sf_DeleteAttachedSurface(void) {             /* (this, flags, surface) */
    surf_t *s = S(A32(0)), *b = A32(2) ? S(A32(2)) : NULL;
    if (!b) RET(DDERR_INVALIDPARAMS, 3);
    if (s->back == b) s->back = NULL;
    for (int i = 0; i < s->n_attached; i++)
        if (s->attached[i] == b) { s->attached[i] = s->attached[--s->n_attached]; break; }
    RET(DD_OK, 3);
}
static void sf_EnumAttachedSurfaces(void) {              /* (this, ctx, callback) */
    surf_t *s = S(A32(0));
    uint32_t mark = hle_scratch_mark(), d = hle_scratch(27 * 4);
    MEM32(d) = 108;
    if (s->back) {
        put_desc(d, s->back);
        MEM32(s->back->self + 4)++;
        uint32_t a[3] = { s->back->self, d, A32(1) };
        hle_call_guest(A32(2), 3, a);
    }
    for (int i = 0; i < s->n_attached; i++) {
        put_desc(d, s->attached[i]);
        MEM32(s->attached[i]->self + 4)++;
        uint32_t a[3] = { s->attached[i]->self, d, A32(1) };
        hle_call_guest(A32(2), 3, a);
    }
    hle_scratch_release(mark);
    RET(DD_OK, 3);
}
static void sf_EnumOverlayZOrders(void) { RET(DD_OK, 4); }
static void sf_Flip(void) {                              /* (this, target, flags): swap front and back pixels */
    surf_t *s = S(A32(0));
    int gpu = s->back && d3d7_flip_gpu && d3d7_flip_gpu(A32(0),s->back->self);
    if (!gpu) D3D7_FLUSH(A32(0));
    if (s->back) {
        if (!gpu) D3D7_FLUSH(s->back->self);
        uint8_t *t = s->px;
        s->px = s->back->px, s->back->px = t;
        mark(s);
    }
    /* d3d7_note_present owns the unique primary-sequence frame counter. */
    if (d3d7_note_present) d3d7_note_present();          /* renderer's present counter */
    RET(DD_OK, 3);
}
static void sf_GetAttachedSurface(void) {                /* (this, &caps, &out) */
    surf_t *s = S(A32(0));
    uint32_t want = A32(1) ? MEM32(A32(1)) : 0;
    surf_t *found = NULL;
    if (want & DDSCAPS_BACKBUFFER) found = s->back;
    else {
        for (int i = 0; i < s->n_attached && !found; i++)
            if (!want || (s->attached[i]->caps & want)) found = s->attached[i];
    }
    /* dx7z's THRASH mip fill walks the chain with GetAttachedSurface(TEXTURE|
     * MIPMAP) and does NOT check the HRESULT, so a missing level is a guest
     * NULL dereference (0x1000C390, crash at guest_base+0). Some textures
     * reach the walk with no chain (created without DDSD_MIPMAPCOUNT, or the
     * walk asks for more levels than were allocated). Synthesise the next
     * level lazily: half the current surface, texture+mipmap caps, attached
     * to the current level. That keeps the ABI and the guest's loop finite
     * (each level halves, so it reaches 1x1 and stops). */
    if (!found && (want & DDSCAPS_MIPMAP) && (s->caps & DDSCAPS_TEXTURE)) {
        /* The guest's THRASH mip builder walks GetAttachedSurface(TEXTURE|
         * MIPMAP) and never checks the HRESULT, then dereferences the out
         * pointer (guest 0x1000C390, crash at guest_base+0 or +0xC4 when the
         * walk runs past the chain it expected). We only ever upload the base
         * level, so instead of synthesising levels whose fields the guest may
         * then misuse, hand back the surface itself: the walk stays valid and
         * bounded, every upload lands in real memory, and sampling uses the
         * base level we actually have. */
        found = s;
        if (trace()) fprintf(stderr, "[ddraw] mip walk on 0x%08X: returning base surface\n", s->self);
    }
    if (!found) { if (A32(2)) MEM32(A32(2)) = 0; RET(DDERR_NOTFOUND, 3); }
    MEM32(A32(2)) = found->self;
    MEM32(found->self + 4)++;
    RET(DD_OK, 3);
}
static void sf_ok_2(void) { RET(DD_OK, 2); }
static void sf_ok_1(void) { RET(DD_OK, 1); }
static void sf_GetCaps(void) { if (A32(1)) MEM32(A32(1)) = S(A32(0))->caps; RET(DD_OK, 2); }
static void sf_GetClipper(void) {
    surf_t *s = S(A32(0));
    if (!s->clipper) RET(DDERR_NOCLIPPERATTACHED, 2);
    MEM32(A32(1)) = s->clipper, MEM32(s->clipper + 4)++;
    RET(DD_OK, 2);
}
static void sf_GetColorKey(void) {                       /* (this, flags, &key) */
    surf_t *s = S(A32(0));
    if (!(A32(1) & DDCKEY_SRCBLT) || !s->ck_src) RET(DDERR_NOCOLORKEY, 3);
    MEM32(A32(2)) = s->ck_lo, MEM32(A32(2) + 4) = s->ck_hi;
    RET(DD_OK, 3);
}
/* A DC on a surface: GDI text drawn into its pixels (gdidc.c). */
static void sf_GetDC(void) {
    surf_t *s = S(A32(0));
    D3D7_FLUSH(A32(0));
    if (A32(1)) MEM32(A32(1)) = hle_gdi_surface_dc(s->px, s->w, s->h, s->pitch, s->bpp);
    RET(DD_OK, 2);
}
static void sf_GetOverlayPosition(void) { RET(DDERR_UNSUPPORTED, 3); }
static void sf_GetPalette(void) {
    surf_t *s = S(A32(0));
    if (!s->palette) RET(DDERR_NOPALETTEATTACHED, 2);
    MEM32(A32(1)) = s->palette, MEM32(s->palette + 4)++;
    RET(DD_OK, 2);
}
static void sf_GetPixelFormat(void) { put_pf_desc(A32(1), S(A32(0))); RET(DD_OK, 2); }
static void sf_GetSurfaceDesc(void) { put_desc(A32(1), S(A32(0))); RET(DD_OK, 2); }
static void sf_Initialize(void) { RET(DD_OK, 2); }  /* (this, lpDD) */
static void sf_Lock(void) {                              /* (this, rect, &desc, flags, event) */
    surf_t *s = S(A32(0));
    s->gpu_sync_lock = !s->locks && nfs5_dx7_sync_lock && nfs5_dx7_sync_lock(A32(0)) &&
                       d3d7_sync_surface && d3d7_sync_surface(A32(0));
    if(!s->gpu_sync_lock) D3D7_FLUSH(A32(0));
    uint32_t d = A32(2);
    if (!d) RET(DDERR_INVALIDPARAMS, 5);
    put_desc(d, s);
    if (A32(1)) {
        int l = (int)MEM32(A32(1)), t = (int)MEM32(A32(1) + 4);
        MEM32(d + 36) = hle_va(s->px + (size_t)t * (size_t)s->pitch + (size_t)l * (size_t)(s->bpp / 8));
    }
    s->locks++;
    RET(DD_OK, 5);
}
static void sf_ReleaseDC(void) { hle_gdi_release_dc(A32(1)); mark(S(A32(0))); RET(DD_OK, 2); }
static void sf_SetClipper(void) {
    surf_t *s = S(A32(0));
    if (A32(1)) MEM32(A32(1) + 4)++;
    s->clipper = A32(1);
    RET(DD_OK, 2);
}
static void sf_SetColorKey(void) {                       /* (this, flags, &key) */
    surf_t *s = S(A32(0));
    if (A32(1) & DDCKEY_SRCBLT) {
        if (A32(2)) s->ck_src = 1, s->ck_lo = MEM32(A32(2)), s->ck_hi = MEM32(A32(2) + 4);
        else s->ck_src = 0;
    }
    RET(DD_OK, 3);
}
static void sf_SetOverlayPosition(void) { RET(DDERR_UNSUPPORTED, 3); }
static void sf_SetPalette(void) {
    surf_t *s = S(A32(0));
    if (A32(1)) MEM32(A32(1) + 4)++;
    s->palette = A32(1);
    mark(s);
    RET(DD_OK, 2);
}
static void sf_Unlock(void) {
    surf_t *s = S(A32(0));
    if(s->gpu_sync_lock) {
        s->gpu_sync_lock=0;if(s->locks)s->locks--;RET(DD_OK,2);
    }
    D3D7_DIRTY(A32(0));                                  /* CPU pixels changed */
    if (s->locks) s->locks--;
    mark(s);
    RET(DD_OK, 2);
}
static void sf_UpdateOverlay(void) { RET(DDERR_UNSUPPORTED, 6); }
static void sf_UpdateOverlayDisplay(void) { RET(DDERR_UNSUPPORTED, 2); }
static void sf_UpdateOverlayZOrder(void) { RET(DDERR_UNSUPPORTED, 3); }
static void sf_GetDDInterface(void) { MEM32(A32(1)) = g_dd; RET(DD_OK, 2); }
static void sf_PageLock(void) { RET(DD_OK, 2); }
static void sf_PageUnlock(void) { RET(DD_OK, 2); }
static void sf_SetPrivateData(void) { RET(DD_OK, 5); }
static void sf_GetPrivateData(void) { if (A32(3)) MEM32(A32(3)) = 0; RET(DDERR_NOTFOUND, 4); }
static void sf_FreePrivateData(void) { RET(DD_OK, 2); }
static void sf_GetUniquenessValue(void) { if (A32(1)) MEM32(A32(1)) = 0; RET(DD_OK, 2); }
static void sf_ChangeUniquenessValue(void) { RET(DD_OK, 1); }
static void sf_SetPriority(void) { RET(DD_OK, 2); }
static void sf_GetPriority(void) { if (A32(1)) MEM32(A32(1)) = 0; RET(DD_OK, 2); }
static void sf_SetLOD(void) { RET(DD_OK, 2); }
static void sf_GetLOD(void) { if (A32(1)) MEM32(A32(1)) = 0; RET(DD_OK, 2); }
static void sf_SetSurfaceDesc(void) {                    /* (this, &desc, flags): the game's own memory */
    surf_t *s = S(A32(0));
    uint32_t d = A32(1), fl = MEM32(d + 4);
    if ((fl & DDSD_LPSURFACE) && MEM32(d + 36)) s->px = (uint8_t *)hle_ptr(MEM32(d + 36));   /* own_px is kept until the surface goes */
    if (fl & DDSD_PITCH) s->pitch = (int)MEM32(d + 16);
    if (fl & DDSD_WIDTH) s->w = (int)MEM32(d + 12);
    if (fl & DDSD_HEIGHT) s->h = (int)MEM32(d + 8);
    RET(DD_OK, 3);
}

/* ---- IDirectDrawPalette, IDirectDrawClipper ---- */
static void pl_Release(void) {
    uint32_t self = A32(0), n = --MEM32(self + 4);
    if (!n) { hle_free(P(self)); hle_com_free(self); }
    RET(n, 1);
}
static void pl_GetCaps(void) { if (A32(1)) MEM32(A32(1)) = 0x4u; RET(DD_OK, 2); }   /* DDPCAPS_8BIT */
static void pl_GetEntries(void) {                        /* (this, flags, start, count, entries) */
    pal_t *p = P(A32(0));
    uint32_t start = A32(2), n = A32(3);
    if (start + n > 256) RET(DDERR_INVALIDPARAMS, 5);
    memcpy(APTR(4), p->entries + start, n * 4);
    RET(DD_OK, 5);
}
static void pl_SetEntries(void) {
    pal_t *p = P(A32(0));
    uint32_t start = A32(2), n = A32(3);
    if (start + n > 256) RET(DDERR_INVALIDPARAMS, 5);
    memcpy(p->entries + start, APTR(4), n * 4);
    g_frame_seq++;
    if (hle_dd_primary_hook) hle_dd_primary_hook();
    RET(DD_OK, 5);
}
static void pl_Initialize(void) { RET(DD_OK, 4); }
static void cl_Release(void) { uint32_t self = A32(0), n = --MEM32(self + 4); if (!n) hle_com_free(self); RET(n, 1); }
static void cl_GetClipList(void) { if (A32(3)) MEM32(A32(3)) = 0; RET(DDERR_UNSUPPORTED, 4); }
static void cl_GetHWnd(void) { MEM32(A32(1)) = MEM32(A32(0) + 8); RET(DD_OK, 2); }
static void cl_IsClipListChanged(void) { if (A32(1)) MEM32(A32(1)) = 0; RET(DD_OK, 2); }
static void cl_SetClipList(void) { RET(DD_OK, 3); }
static void cl_SetHWnd(void) { MEM32(A32(0) + 8) = A32(2); RET(DD_OK, 3); }
static void cl_Initialize(void) { RET(DD_OK, 3); }

static void vtables(void) {
    if (g_vt_dd) return;
#define DD_COMMON(I) \
        { I "QueryInterface", dd_QueryInterface }, { I "AddRef", hle_com_AddRef }, { I "Release", dd_Release }, \
        { I "Compact", dd_Compact }, { I "CreateClipper", dd_CreateClipper }, { I "CreatePalette", dd_CreatePalette }, \
        { I "CreateSurface", dd_CreateSurface }, { I "DuplicateSurface", dd_DuplicateSurface }, \
        { I "EnumDisplayModes", dd_EnumDisplayModes }, { I "EnumSurfaces", dd_EnumSurfaces }, \
        { I "FlipToGDISurface", dd_FlipToGDISurface }, { I "GetCaps", dd_GetCaps }, { I "GetDisplayMode", dd_GetDisplayMode }, \
        { I "GetFourCCCodes", dd_GetFourCCCodes }, { I "GetGDISurface", dd_GetGDISurface }, \
        { I "GetMonitorFrequency", dd_GetMonitorFrequency }, { I "GetScanLine", dd_GetScanLine }, \
        { I "GetVerticalBlankStatus", dd_GetVerticalBlankStatus }, { I "Initialize", dd_Initialize }, \
        { I "RestoreDisplayMode", dd_RestoreDisplayMode }, { I "SetCooperativeLevel", dd_SetCooperativeLevel }
    static const win32hle_shim dd[] = { DD_COMMON("IDirectDraw::"), { "IDirectDraw::SetDisplayMode", dd_SetDisplayMode },
        { "IDirectDraw::WaitForVerticalBlank", dd_WaitForVerticalBlank }, { 0, 0 } };
    static const win32hle_shim dd2[] = { DD_COMMON("IDirectDraw2::"), { "IDirectDraw2::SetDisplayMode", dd2_SetDisplayMode },
        { "IDirectDraw2::WaitForVerticalBlank", dd_WaitForVerticalBlank },
        { "IDirectDraw2::GetAvailableVidMem", dd2_GetAvailableVidMem }, { 0, 0 } };
    /* IDirectDraw7: slots 0-20 = DD_COMMON, 21 SetDisplayMode, 22
     * WaitForVerticalBlank, 23 GetAvailableVidMem, 24 GetSurfaceFromDC,
     * 25 RestoreAllSurfaces, 26 TestCooperativeLevel, 27 GetDeviceIdentifier,
     * 28 StartModeTest, 29 EvaluateMode. */
    static const win32hle_shim dd7[] = { DD_COMMON("IDirectDraw7::"), { "IDirectDraw7::SetDisplayMode", dd2_SetDisplayMode },
        { "IDirectDraw7::WaitForVerticalBlank", dd_WaitForVerticalBlank },
        { "IDirectDraw7::GetAvailableVidMem", dd2_GetAvailableVidMem },
        { "IDirectDraw7::GetSurfaceFromDC", dd7_GetSurfaceFromDC },
        { "IDirectDraw7::RestoreAllSurfaces", dd7_RestoreAllSurfaces },
        { "IDirectDraw7::TestCooperativeLevel", dd7_TestCooperativeLevel },
        { "IDirectDraw7::GetDeviceIdentifier", dd7_GetDeviceIdentifier },
        { "IDirectDraw7::StartModeTest", dd7_StartModeTest },
        { "IDirectDraw7::EvaluateMode", dd7_EvaluateMode }, { 0, 0 } };
    static const win32hle_shim sf[] = {
        { "IDirectDrawSurface::QueryInterface", sf_QueryInterface }, { "IDirectDrawSurface::AddRef", hle_com_AddRef },
        { "IDirectDrawSurface::Release", sf_Release }, { "IDirectDrawSurface::AddAttachedSurface", sf_AddAttachedSurface },
        { "IDirectDrawSurface::AddOverlayDirtyRect", sf_AddOverlayDirtyRect }, { "IDirectDrawSurface::Blt", sf_Blt },
        { "IDirectDrawSurface::BltBatch", sf_BltBatch }, { "IDirectDrawSurface::BltFast", sf_BltFast },
        { "IDirectDrawSurface::DeleteAttachedSurface", sf_DeleteAttachedSurface },
        { "IDirectDrawSurface::EnumAttachedSurfaces", sf_EnumAttachedSurfaces },
        { "IDirectDrawSurface::EnumOverlayZOrders", sf_EnumOverlayZOrders }, { "IDirectDrawSurface::Flip", sf_Flip },
        { "IDirectDrawSurface::GetAttachedSurface", sf_GetAttachedSurface }, { "IDirectDrawSurface::GetBltStatus", sf_ok_2 },
        { "IDirectDrawSurface::GetCaps", sf_GetCaps }, { "IDirectDrawSurface::GetClipper", sf_GetClipper },
        { "IDirectDrawSurface::GetColorKey", sf_GetColorKey }, { "IDirectDrawSurface::GetDC", sf_GetDC },
        { "IDirectDrawSurface::GetFlipStatus", sf_ok_2 }, { "IDirectDrawSurface::GetOverlayPosition", sf_GetOverlayPosition },
        { "IDirectDrawSurface::GetPalette", sf_GetPalette }, { "IDirectDrawSurface::GetPixelFormat", sf_GetPixelFormat },
        { "IDirectDrawSurface::GetSurfaceDesc", sf_GetSurfaceDesc }, { "IDirectDrawSurface::Initialize", sf_Initialize },
        { "IDirectDrawSurface::IsLost", sf_ok_1 }, { "IDirectDrawSurface::Lock", sf_Lock },
        { "IDirectDrawSurface::ReleaseDC", sf_ReleaseDC }, { "IDirectDrawSurface::Restore", sf_ok_1 },
        { "IDirectDrawSurface::SetClipper", sf_SetClipper }, { "IDirectDrawSurface::SetColorKey", sf_SetColorKey },
        { "IDirectDrawSurface::SetOverlayPosition", sf_SetOverlayPosition }, { "IDirectDrawSurface::SetPalette", sf_SetPalette },
        { "IDirectDrawSurface::Unlock", sf_Unlock }, { "IDirectDrawSurface::UpdateOverlay", sf_UpdateOverlay },
        { "IDirectDrawSurface::UpdateOverlayDisplay", sf_UpdateOverlayDisplay },
        { "IDirectDrawSurface::UpdateOverlayZOrder", sf_UpdateOverlayZOrder },
        { "IDirectDrawSurface2::GetDDInterface", sf_GetDDInterface }, { "IDirectDrawSurface2::PageLock", sf_PageLock },
        { "IDirectDrawSurface2::PageUnlock", sf_PageUnlock }, { "IDirectDrawSurface3::SetSurfaceDesc", sf_SetSurfaceDesc },
        { "IDirectDrawSurface4::SetPrivateData", sf_SetPrivateData },
        { "IDirectDrawSurface4::GetPrivateData", sf_GetPrivateData },
        { "IDirectDrawSurface4::FreePrivateData", sf_FreePrivateData },
        { "IDirectDrawSurface4::GetUniquenessValue", sf_GetUniquenessValue },
        { "IDirectDrawSurface4::ChangeUniquenessValue", sf_ChangeUniquenessValue },
        { "IDirectDrawSurface7::SetPriority", sf_SetPriority },
        { "IDirectDrawSurface7::GetPriority", sf_GetPriority },
        { "IDirectDrawSurface7::SetLOD", sf_SetLOD },
        { "IDirectDrawSurface7::GetLOD", sf_GetLOD },
        { 0, 0 } };
    static const win32hle_shim gm[] = {
        { "IDirectDrawGammaControl::QueryInterface", hle_com_QueryInterface },
        { "IDirectDrawGammaControl::AddRef", hle_com_AddRef },
        { "IDirectDrawGammaControl::Release", gm_Release },
        { "IDirectDrawGammaControl::GetGammaRamp", gm_GetGammaRamp },
        { "IDirectDrawGammaControl::SetGammaRamp", gm_SetGammaRamp },
        { 0, 0 } };
    static const win32hle_shim d3d3[] = {
        { "IDirect3D3::QueryInterface", d3_QueryInterface }, { "IDirect3D3::AddRef", hle_com_AddRef },
        { "IDirect3D3::Release", hle_com_Release },
        { "IDirect3D3::EnumDevices", d3_EnumDevices },
        { "IDirect3D3::CreateLight", d3_notimpl2 }, { "IDirect3D3::CreateMaterial", d3_notimpl2 },
        { "IDirect3D3::CreateViewport", d3_notimpl2 }, { "IDirect3D3::FindDevice", d3_notimpl2 },
        { "IDirect3D3::CreateDevice", d3_CreateDevice },
        { "IDirect3D3::CreateVertexBuffer", d3_CreateVertexBuffer },
        { "IDirect3D3::EnumZBufferFormats", d3_EnumZBufferFormats },
        { "IDirect3D3::EvictManagedTextures", d3_ok_1 },
        { 0, 0 } };
    static const win32hle_shim pl[] = {
        { "IDirectDrawPalette::QueryInterface", hle_com_QueryInterface }, { "IDirectDrawPalette::AddRef", hle_com_AddRef },
        { "IDirectDrawPalette::Release", pl_Release }, { "IDirectDrawPalette::GetCaps", pl_GetCaps },
        { "IDirectDrawPalette::GetEntries", pl_GetEntries }, { "IDirectDrawPalette::Initialize", pl_Initialize },
        { "IDirectDrawPalette::SetEntries", pl_SetEntries }, { 0, 0 } };
    static const win32hle_shim cl[] = {
        { "IDirectDrawClipper::QueryInterface", hle_com_QueryInterface }, { "IDirectDrawClipper::AddRef", hle_com_AddRef },
        { "IDirectDrawClipper::Release", cl_Release }, { "IDirectDrawClipper::GetClipList", cl_GetClipList },
        { "IDirectDrawClipper::GetHWnd", cl_GetHWnd }, { "IDirectDrawClipper::Initialize", cl_Initialize },
        { "IDirectDrawClipper::IsClipListChanged", cl_IsClipListChanged }, { "IDirectDrawClipper::SetClipList", cl_SetClipList },
        { "IDirectDrawClipper::SetHWnd", cl_SetHWnd }, { 0, 0 } };
    g_vt_dd = hle_com_vtable(dd);
    g_vt_dd2 = hle_com_vtable(dd2);
    g_vt_dd7 = hle_com_vtable(dd7);
    g_vt_gamma = hle_com_vtable(gm);
    g_vt_d3d3 = hle_com_vtable(d3d3);
    g_vt_surf = hle_com_vtable(sf);
    g_vt_pal = hle_com_vtable(pl);
    g_vt_clip = hle_com_vtable(cl);
    g_dd = hle_com_new(g_vt_dd, 12);
    g_dd2 = hle_com_new(g_vt_dd2, 12);
    g_dd7 = hle_com_new(g_vt_dd7, 12);
    g_gamma = hle_com_new(g_vt_gamma, 8);
    g_d3d3 = hle_com_new(g_vt_d3d3, 8);
    MEM32(g_dd + 4) = MEM32(g_dd2 + 4) = MEM32(g_dd7 + 4) = 0;   /* held by nobody until a Create */
    MEM32(g_gamma + 4) = 0, MEM32(g_d3d3 + 4) = 0;
    for (int i = 0; i < 256; i++) g_gammatab[0][i] = g_gammatab[1][i] = g_gammatab[2][i] = (uint16_t)(i * 257);
}

static void x_DirectDrawCreate(void) {                   /* (guid, &out, outer) */
    vtables();
    MEM32(g_dd + 4)++;
    MEM32(A32(1)) = g_dd;
    RET(DD_OK, 3);
}
static void x_DirectDrawEnumerateA(void) {               /* (callback, ctx): the primary display */
    static uint32_t desc, name;                          /* the guest may keep them */
    if (!desc) desc = hle_guest_strdup("Primary Display Driver"), name = hle_guest_strdup("display");
    uint32_t a[4] = { 0, desc, name, A32(1) };
    if (A32(0)) hle_call_guest(A32(0), 4, a);
    RET(DD_OK, 2);
}

/* DirectDrawCreateEx(guid, &out, riid, outer): the DX7 way to the device, and
 * the one dx7z uses to get the IDirectDraw7 face. */
static void x_DirectDrawCreateEx(void) {
    uint32_t out = A32(1);
    if (out) MEM32(out) = 0;
    if (!out || A32(3)) RET(DDERR_INVALIDPARAMS, 4);
    uint32_t d1 = A32(2) ? MEM32(A32(2)) : 0x15E65EC0u;    /* no riid: IDirectDraw7 */
    vtables();
    uint32_t obj;
    if (d1 == 0x15E65EC0u || d1 == 0) obj = g_dd7;         /* IDirectDraw7 */
    else if (d1 == 0x9C59509Au) obj = g_dd7;               /* DD4: the DD7 face is its prefix */
    else if (d1 == 0xB3A6F3E0u) obj = g_dd2;               /* DD2 */
    else if (d1 == 0x6C14DB80u) obj = g_dd;                /* DD1 */
    else { if (trace()) fprintf(stderr, "[ddraw] DirectDrawCreateEx(riid %#x): not served\n", d1); RET(0x80004002u, 4); }
    MEM32(obj + 4)++;
    MEM32(out) = obj;
    if (trace()) fprintf(stderr, "[ddraw] DirectDrawCreateEx -> %#x\n", obj);
    RET(DD_OK, 4);
}
/* DirectDrawEnumerateExA(cb, ctx, flags): DDENUMCALLBACKEXA is 5 args (guid,
 * description, name, context, hmonitor). The renderer probes this first with a
 * NULL callback; a display exists, so DD_OK. */
static void x_DirectDrawEnumerateExA(void) {
    static uint32_t desc, name;
    if (!desc) desc = hle_guest_strdup("Primary Display Driver"), name = hle_guest_strdup("display");
    uint32_t cb = A32(0);
    if (cb) { uint32_t a[5] = { 0, desc, name, A32(1), 0 }; hle_call_guest(cb, 5, a); }
    RET(DD_OK, 3);
}

const win32hle_shim win32hle_ddraw[] = {
    { "DirectDrawCreate", x_DirectDrawCreate }, { "DirectDrawEnumerateA", x_DirectDrawEnumerateA },
    { "DirectDrawCreateEx", x_DirectDrawCreateEx }, { "DirectDrawEnumerateExA", x_DirectDrawEnumerateExA },
    { 0, 0 }
};

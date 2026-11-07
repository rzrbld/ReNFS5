/*
 * renderer.h - the GLES2 renderer that emulates the Direct3D7 fixed-function
 * pipeline for NFS5's dx7z.dll.  It is deliberately independent of win32hle:
 * d3d7.c converts guest D3D7 state and guest D3D7 vertices into the plain
 * host structures below and calls this layer, which never touches guest
 * memory.  That keeps the drawing path testable from a standalone probe
 * (runtime/render/probe/gl_probe.c) without the game or the HLE.
 *
 * Ground rules:
 *  - GLES2 is the floor: no instancing, no VAOs, no integer textures, no
 *    depth24 unless the driver advertises OES_depth24.
 *  - Render targets are FBOs at the D3D surface's native size (640x480 for
 *    NFS5). Flip exchanges GPU targets and presents through SDL. Only an
 *    explicit CPU pixel access resolves a target into guest DDraw memory.
 *  - FBO orientation: screen row 0 is GL framebuffer row 0 (y = -1 in NDC).
 *    glReadPixels therefore returns rows top-to-bottom exactly as guest
 *    surfaces store them and texture upload needs no vertical flip.
 */
#ifndef NFS5_RENDERER_H
#define NFS5_RENDERER_H

#include <stdint.h>
#include <GLES2/gl2.h>

/* Canonical host vertex for a D3DTLVERTEX (XYZRHW) draw. 16 floats. */
typedef struct {
    float x, y, z, rhw;
    float color[4];      /* diffuse, straight alpha */
    float spec[4];       /* specular; spec[3] is the D3D7 TL fog factor */
    float tex0[2];
    float tex1[2];
} RTLVertex;

/* Canonical host vertex for a transformed (XYZ) draw. 18 floats. */
typedef struct {
    float x, y, z;
    float normal[3];
    float color[4];
    float spec[4];
    float tex0[2];
    float tex1[2];
} RLitVertex;

typedef struct {
    float diffuse[4], specular[4], ambient[4];
    float position[3], direction[3];
    float range, falloff;
    uint32_t type;       /* D3DLIGHT7 type: 1 point, 2 spot, 3 directional */
} RLight;

typedef struct {
    uint32_t width, height;          /* render-target size */
    uint32_t vp_x, vp_y, vp_w, vp_h; /* D3D viewport in target pixels */
    float vp_minz, vp_maxz;

    int cull;                        /* 0 none, otherwise D3DCULL_* value */
    int alpha_blend;
    int src_blend, dst_blend;        /* D3DBLEND values */
    int depth_test;                  /* D3DZBUFFERTYPE: 0/1/2 */
    int depth_write;
    int depth_func;                  /* D3DCMPFUNC */
    int alpha_test, alpha_func, alpha_ref;
    int lastpixel;                   /* D3DRS_LASTPIXEL: 0 => force last pixel */
    int zbias;                       /* D3DRS_ZBIAS 0..16 */
    int shade;                       /* D3DSHADEMODE */
    int specular_enable;
    int colorkey_enable;             /* D3DRS_COLORKEYENABLE */
    int colorkey_valid;
    float colorkey_lo[4], colorkey_hi[4];

    int fog;                         /* D3DRS_FOGENABLE */
    int fog_mode;                    /* D3DFOGMODE */
    int fog_vertex;                  /* D3DRS_FOGVERTEXMODE != NONE */
    uint32_t fog_color;
    float fog_start, fog_end, fog_density;

    int lighting;                    /* D3DRS_LIGHTING */
    int vertex_color_source;         /* 0 material, 1 color1 (D3DMCS_*) */
    float material_diffuse[4], material_ambient[4], material_specular[4],
          material_emissive[4], material_power;
    float global_ambient[4];
    int light_count;
    RLight lights[8];
    uint32_t light_on;

    int stage_on[2];
    int colorop[2], colorarg[2][2];
    int alphaop[2], alphaarg[2][2];
    int address[2][2];               /* [stage][u=0/v=1] D3DTADDRESS_* */
    int mag_filter[2], min_filter[2], mip_filter[2];
    uint32_t texture_factor;
    int texgen[2];                   /* apply texture matrix */
    float texmat[2][16];

    int render_state_fingerprint;    /* diagnostics only */
} RState;

/* One uploaded texture (a D3D7 texture surface's pixel content). */
typedef struct {
    GLuint gl;
    int w, h;
    int has_alpha;
    int levels;                      /* mip levels present in `gl` */
} RTexture;

/* Aggregate counters, reset by r_take_stats(). */
typedef struct {
    uint64_t draws, vertices, primitives;
    uint64_t clears, targets;
    uint64_t tex_uploads, tex_upload_bytes, tex_binds;
    uint64_t readbacks, readback_bytes;
    double cpu_ms;                   /* time spent in the renderer */

    uint64_t frames;
    double frame_ms;                 /* last frame interval */
    double frame_ms_total;
} RStats;

int  r_init(void);                   /* create the GLES context + programs; 1 on success */
void r_shutdown(void);
int  r_ready(void);
const char *r_context_info(void);    /* GL_VERSION / GL_RENDERER string, for docs+logs */

/* Render target: an FBO at w x h. Re-creates are cheap (called on SetRenderTarget).
 * Returns 1 on success; 0 if the FBO is incomplete (renderer stays usable for
 * screenshots but the caller should treat the frame as invalid). */
int  r_target(int w, int h);
/* Hint the target's colour format. 16 bpp guest render targets use RGB565 so
 * the frame can be read back in its native packed format (glReadPixels with
 * GL_RGB/GL_UNSIGNED_SHORT_5_6_5); on Mali-G31 the RGBA8 path costs ~40 ms
 * per frame, which is what made the front end run at ~1 fps. */
void r_target_hint_565(int on);
int  r_target_is_565(void);
int  r_target_width(void);
int  r_target_height(void);

void r_clear(uint32_t argb, float z, int clear_color, int clear_depth);
/* Load guest (RGBA8) pixels into the current target (2D drawn into the RT). */
void r_target_load(const uint8_t *rgba, int w, int h);
/* Rect version; x,y,w,h in screen space (top-left origin). */
void r_clear_rect(uint32_t argb, float z, int clear_color, int clear_depth,
                  int x, int y, int w, int h);
void r_set_viewport(int x, int y, int w, int h, float minz, float maxz);

/* Textures: create one, upload/replace the base level as RGBA8 (guest pixels
 * converted by d3d7.c), set the sampler parameters from the texture-stage
 * state.  `mips` is 0 for base only; >0 uploads a chain (not used by NFS5
 * assets per analysis, kept for completeness). */
RTexture r_tex_create(void);
void r_tex_delete(RTexture *t);
void r_tex_upload(RTexture *t, const uint8_t *rgba, int w, int h);
void r_tex_mipmap(RTexture *t);      /* generate mips from the base level */
void r_tex_params(const RState *s, int stage, const RTexture *t);
/* Texture objects for the two stages (GL names; 0 -> 1x1 white). */
void r_set_textures(GLuint t0, GLuint t1);

/* Draw. `prim` is a D3D7 D3DPRIMITIVETYPE value; indices may be NULL. */
void r_draw_tl(const RState *s, const RTLVertex *v, int nverts,
               const uint16_t *idx, int nidx, int prim);
void r_draw_lit(const RState *s, const RLitVertex *v, int nverts,
                const uint16_t *idx, int nidx, int prim,
                const float world[16], const float view[16], const float proj[16]);

/* Fast path: intern a fixed-function state once (id valid until the epoch
 * changes) and queue raw TL vertices in the THRASH/D3D layout - stride 32
 * (FVF 0x1C4) or 40 (FVF 0x2C4) - with the textures set by r_set_textures. */
int  r_state_intern(const RState *s);
unsigned r_state_epoch(void);
void r_draw_tlv(int state, const void *vertices, int stride, int nverts,
                const uint16_t *idx, int nidx, int prim);
int r_snapshot(void);
void r_submit(void);
int r_present_front(int w, int h, int stretch, int fps);
int r_read_front(uint8_t *rgba, int w, int h);

/* Read the finished frame back (RGBA8, row 0 = top/screen row 0). */
int  r_read_pixels(uint8_t *rgba, int w, int h);
/* Fast path: read the target back as RGB565 directly (glReadPixels GL_RGB /
 * GL_UNSIGNED_SHORT_5_6_5) into `out` (w*h uint16). Returns 0 if the driver
 * rejects it, so callers fall back to r_read_pixels + their own conversion. */
int  r_read_pixels_565(uint16_t *out, int w, int h);

/* Frame boundaries for the NFS5_STATS report (EndScene/readback). */
void r_frame_begin(void);
void r_frame_end(void);

/* PNG screenshot (rows top-to-bottom RGBA8). 1 on success. */
int  r_write_png(const char *path, const uint8_t *rgba, int w, int h);

/* Diagnostics. */
RStats r_take_stats(void);
void  r_get_stats(RStats *out);      /* without reset */
const char *r_last_error(void);

#endif /* NFS5_RENDERER_H */

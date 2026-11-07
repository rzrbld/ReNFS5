/* Native DX7 command backend. Guest ABI decoding lives in d3d7.c.
 * GPU resources stay GPU-owned until an explicit CPU read. Presentation
 * exchanges render targets and swaps SDL.
 *
 * Hot-path design (measured on the H700 / Mali-G31: libmali costs ~30 us per
 * glDraw* with state churn, so per-draw CPU and GL call counts dominate):
 *  - fixed-function states are interned once (r_state_intern) and commands
 *    carry a small state id; batching and redundant-state checks compare ids;
 *  - each interned state caches its compiled shader variant;
 *  - uniforms are addressed through a fixed location table per program and
 *    only re-sent when their value changes; raster state is diffed against
 *    the GL state actually applied, independent of program switches;
 *  - per-texture sampler parameters are cached (GL keeps them per texture);
 *  - pre-transformed vertices use one compact 40-byte layout that matches the
 *    THRASH/D3D TL vertex (x,y,z,rhw, BGRA diffuse, BGRA specular, uv0, uv1),
 *    so the common FVFs are copied without per-component conversion;
 *  - TL positions are emitted with w = 1/rhw, i.e. perspective-correct
 *    texturing like Direct3D (NFS5_AFFINE=1 restores the old affine look).
 */
#include "renderer.h"
#include "../shaders/shader_ff.h"
#include "gles_ctx.h"
#include <GLES2/gl2ext.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

typedef struct {
    GLuint fbo, color, depth;
    int w, h, packed;
} Target;
/* compact pre-transformed vertex (40 bytes) */
typedef struct {
    float pos[4];
    uint32_t c0, c1;     /* D3DCOLOR (ARGB dword, bytes B,G,R,A) */
    float uv[4];
} TLV;
/* transformed + lit vertex (only the generic XYZ path uses it) */
typedef struct {
    float pos[4], normal[3], color[4], spec[4], uv[4];
} LitV;
typedef struct {
    int state;
    GLuint textures[2];
    int levels[2];
    int lit, primitive;
    int matrices;        /* index into the matrix pool, lit only */
    size_t first, count;
} Command;

enum {
    U_VIEWPORT, U_TEXGEN, U_TEXMAT0, U_TEXMAT1, U_FOG, U_FOGVERTEX, U_FOGMODE, U_FOGSTART,
    U_FOGEND, U_FOGDENSITY, U_TEX0, U_TEX1, U_TFACTOR, U_PERSP,
    U_STAGE0_ON, U_STAGE1_ON, U_C0OP, U_C0A1, U_C0A2, U_A0OP, U_A0A1, U_A0A2,
    U_C1OP, U_C1A1, U_C1A2, U_A1OP, U_A1A1, U_A1A2,
    U_ALPHA_TEST, U_ALPHA_FUNC, U_COLORKEY_ON, U_FOG_ON, U_SPECULAR_ADD, U_FLAT_DIFFUSE,
    U_ALPHA_REF, U_COLORKEY_LO, U_COLORKEY_HI, U_FOG_COLOR,
    U_WORLD, U_VIEW, U_PROJ, U_LIGHTING, U_MAT_DIFFUSE, U_MAT_AMBIENT, U_MAT_SPECULAR,
    U_MAT_EMISSIVE, U_AMBIENT, U_LIGHT_COUNT,
    U_LIGHT_DIFFUSE, U_LIGHT_SPECULAR, U_LIGHT_AMBIENT, U_LIGHT_POS, U_LIGHT_PARAM,
    U_IMAGE, U_COUNT
};
static const char *const uniform_names[U_COUNT] = {
    "u_viewport", "u_texgen", "u_texmat0", "u_texmat1", "u_fog", "u_fogvertex", "u_fogmode",
    "u_fogstart", "u_fogend", "u_fogdensity", "u_tex0", "u_tex1", "u_tfactor", "u_persp",
    "u_stage0_on", "u_stage1_on", "u_c0op", "u_c0a1", "u_c0a2", "u_a0op", "u_a0a1", "u_a0a2",
    "u_c1op", "u_c1a1", "u_c1a2", "u_a1op", "u_a1a1", "u_a1a2",
    "u_alpha_test", "u_alpha_func", "u_colorkey_on", "u_fog_on", "u_specular_add",
    "u_flat_diffuse", "u_alpha_ref", "u_colorkey_lo", "u_colorkey_hi", "u_fog_color",
    "u_world", "u_view", "u_proj", "u_lighting", "u_mat_diffuse", "u_mat_ambient",
    "u_mat_specular", "u_mat_emissive", "u_ambient", "u_light_count",
    "u_light_diffuse[0]", "u_light_specular[0]", "u_light_ambient[0]", "u_light_pos[0]",
    "u_light_param[0]", "image"};
typedef struct {
    GLuint object;
    int specialized, lit;
    GLint loc[U_COUNT];
    unsigned char val[U_COUNT][64];
    unsigned char len[U_COUNT];
    int applied_state;   /* interned state whose uniforms are loaded, -1 none */
    int applied_matrices;
} Program;
typedef struct {
    int value[20];
    int lit;
} ShaderKey;
typedef struct {
    ShaderKey key;
    Program program;
} Variant;
/* interned fixed-function state */
typedef struct {
    RState s;
    uint32_t hash;
    Program *prog[2];    /* [lit] cached shader variant */
} StateEntry;
#define STATE_CAP 1024
#define STATE_HASH 2048
/* GL raster state actually applied (diffed independent of programs) */
typedef struct {
    int valid;
    int depth_test, depth_func, depth_write, blend, src, dst, cull, zbias;
    int vx, vy, vw, vh;
    float minz, maxz;
} Raster;
typedef struct { GLint min, mag, ws, wt; } TexParams;

static struct {
    int ready, hint, frame_open;
    Variant variants[128];
    int variant_count;
    Target back, front;
    Program tl, lit, copy;
    GLuint stream, stream_lit, white, upload;
    GLuint *names;       /* texture id -> GL name (worker side) */
    size_t names_cap;
    StateEntry *states;
    int nstates;
    int state_hash[STATE_HASH];
    unsigned epoch;
    Raster raster;
    GLuint bound[2];
    int active_unit;
    TexParams *tp;
    size_t tp_cap;
    int persp;
    RStats stats;
    double frame_start;
    char error[256];
} gpu;
/* --- recorded GL work: ordered ops + the vertex/command data they use --- */
enum { OP_DRAWS = 1, OP_CLEAR, OP_TARGET, OP_TEX_CREATE, OP_TEX_UPLOAD, OP_TEX_DELETE, OP_TEX_MIPMAP,
       OP_TARGET_LOAD, OP_FLUSH, OP_SNAPSHOT, OP_PRESENT, OP_READ };
typedef struct {
    int type;
    int a, b, c, d, e, f;
    uint32_t u;
    float z;
    size_t off;
    void *dst;
    int *result;
} Op;
typedef struct {
    TLV *tlv;
    size_t used, capacity;
    LitV *litv;
    size_t lit_used, lit_capacity;
    float *matrices;     /* 48 floats per lit command */
    size_t mat_used, mat_capacity;
    Command *commands;
    size_t count, command_capacity;
    size_t draw_start;   /* first command not yet covered by an OP_DRAWS */
    Op *ops;
    size_t nops, ops_cap;
    uint8_t *arena;
    size_t arena_used, arena_cap;
} Batch;
/* front end: the thread that calls the r_* API (the game's render thread) */
static struct {
    Batch b[2];
    int cur;
    int threaded;        /* a GL worker thread executes submitted batches */
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    Batch *pending;
    int busy;
    int init_state;      /* worker init: 0 running, 1 ok, 2 no surfaceless, 3 failed */
    pthread_mutex_t surface;   /* who may bind the window surface */
    GLuint next_tex;
    GLuint textures[2];
    int levels[2];
    int back_w, back_h, back_packed, hint, have_front;
} fe = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER,
        .surface = PTHREAD_MUTEX_INITIALIZER, .next_tex = 1};
#define FB (&fe.b[fe.cur])
static void sync_all(void);
static double milliseconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static int error(const char *message) {
    snprintf(gpu.error, sizeof gpu.error, "%s", message);
    fprintf(stderr, "[dx7] %s\n", message);
    return 0;
}
static GLuint shader(GLenum type, const char *source) {
    GLuint id = glCreateShader(type);
    glShaderSource(id, 1, &source, NULL);
    glCompileShader(id);
    GLint ok;
    glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(id, sizeof gpu.error, NULL, gpu.error);
        fprintf(stderr, "[dx7] shader: %s\n", gpu.error);
        glDeleteShader(id);
        return 0;
    }
    return id;
}
static int program(Program *p, const char *vs, const char *fs, int lit) {
    memset(p, 0, sizeof *p);
    GLuint v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) {
        if (v)
            glDeleteShader(v);
        if (f)
            glDeleteShader(f);
        return 0;
    }
    p->object = glCreateProgram();
    glAttachShader(p->object, v);
    glAttachShader(p->object, f);
    const char *names[] = {"a_pos", "a_normal", "a_color", "a_spec", "a_tex0", "a_tex1"};
    for (int i = 0; i < 6; i++)
        glBindAttribLocation(p->object, i, names[i]);
    glLinkProgram(p->object);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok;
    glGetProgramiv(p->object, GL_LINK_STATUS, &ok);
    if (!ok) {
        glGetProgramInfoLog(p->object, sizeof gpu.error, NULL, gpu.error);
        fprintf(stderr, "[dx7] program: %s\n", gpu.error);
        return 0;
    }
    for (int i = 0; i < U_COUNT; i++)
        p->loc[i] = glGetUniformLocation(p->object, uniform_names[i]);
    p->lit = lit;
    p->applied_state = -1;
    p->applied_matrices = -1;
    return 1;
}
/* upload a uniform only when its value changed for this program */
static int changed(Program *p, int u, const void *v, size_t n) {
    if (p->loc[u] < 0)
        return 0;
    if (p->len[u] == n && !memcmp(p->val[u], v, n))
        return 0;
    memcpy(p->val[u], v, n);
    p->len[u] = (unsigned char)n;
    return 1;
}
static void ui(Program *p, int u, int v) {
    if (changed(p, u, &v, sizeof v))
        glUniform1i(p->loc[u], v);
}
static void uf(Program *p, int u, float v) {
    if (changed(p, u, &v, sizeof v))
        glUniform1f(p->loc[u], v);
}
static void uv(Program *p, int u, const float *v) {
    if (changed(p, u, v, 4 * sizeof *v))
        glUniform4fv(p->loc[u], 1, v);
}
static void um(Program *p, int u, const float *v) {
    if (changed(p, u, v, 16 * sizeof *v))
        glUniformMatrix4fv(p->loc[u], 1, GL_FALSE, v);
}
static void uc(Program *p, int u, uint32_t c) {
    float v[] = {((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f,
                 (c >> 24) / 255.f};
    uv(p, u, v);
}
static void destroy(Target *t) {
    if (t->depth)
        glDeleteRenderbuffers(1, &t->depth);
    if (t->fbo)
        glDeleteFramebuffers(1, &t->fbo);
    if (t->color)
        glDeleteTextures(1, &t->color);
    memset(t, 0, sizeof *t);
}
/* Anything outside the command executor that touches GL state calls this. */
/* NFS5_DETAIL (launcher DETAIL):
 *   low    nearest texture filtering everywhere (crisp, cheapest)
 *   medium the filtering the game asks for (default)
 *   high   mipmapped trilinear + 4x anisotropic textures and 4x MSAA render
 *          targets (GL_EXT_multisampled_render_to_texture: on tile-based GPUs
 *          the samples resolve in tile memory, so the cost stays modest).
 * The game's own mip-chain build is disabled (dx7z override), so mip levels
 * are generated here for power-of-two textures that are uploaded rarely. */
enum { DETAIL_LOW, DETAIL_MEDIUM, DETAIL_HIGH };
static int g_detail = -1;
static int detail_level(void) {
    if (g_detail < 0) {
        const char *d = getenv("NFS5_DETAIL");
        g_detail = (d && !strcmp(d, "low")) ? DETAIL_LOW : (d && !strcmp(d, "high")) ? DETAIL_HIGH : DETAIL_MEDIUM;
    }
    return g_detail;
}
typedef void (GL_APIENTRYP PFN_FBTEX2DMS)(GLenum, GLenum, GLenum, GLuint, GLint, GLsizei);
typedef void (GL_APIENTRYP PFN_RBSTORAGEMS)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
static PFN_FBTEX2DMS p_fbtex2dms;
static PFN_RBSTORAGEMS p_rbstoragems;
static int g_msaa = -1;          /* samples for render targets, 0 = off */
static int g_aniso;              /* GL_EXT_texture_filter_anisotropic present */
static void detail_caps(void) {  /* on the GL thread, context current */
    if (g_msaa >= 0) return;
    g_msaa = 0;
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    g_aniso = ext && strstr(ext, "GL_EXT_texture_filter_anisotropic") != NULL;
    if (detail_level() == DETAIL_HIGH && ext && strstr(ext, "GL_EXT_multisampled_render_to_texture")) {
        p_fbtex2dms = (PFN_FBTEX2DMS)gles_ctx_proc("glFramebufferTexture2DMultisampleEXT");
        p_rbstoragems = (PFN_RBSTORAGEMS)gles_ctx_proc("glRenderbufferStorageMultisampleEXT");
        if (p_fbtex2dms && p_rbstoragems) g_msaa = 4;
    }
    fprintf(stderr, "[dx7] detail=%s msaa=%dx anisotropic=%s\n",
            detail_level() == DETAIL_LOW ? "low" : detail_level() == DETAIL_HIGH ? "high" : "medium",
            g_msaa, g_aniso ? "yes" : "no");
}

static void gl_state_unknown(void) {
    gpu.raster.valid = 0;
    gpu.bound[0] = gpu.bound[1] = (GLuint)-1;
    gpu.active_unit = -1;
}
static int target(Target *t, int w, int h, int packed) {
    if (t->fbo && t->w == w && t->h == h && t->packed == packed)
        return 1;
    destroy(t);
    gl_state_unknown();
    t->w = w;
    t->h = h;
    t->packed = packed;
    glGenTextures(1, &t->color);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t->color);
    glTexImage2D(GL_TEXTURE_2D, 0, packed ? GL_RGB : GL_RGBA, w, h, 0, packed ? GL_RGB : GL_RGBA,
                 packed ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    detail_caps();
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glGenRenderbuffers(1, &t->depth);
    glBindRenderbuffer(GL_RENDERBUFFER, t->depth);
    int ms = g_msaa;
    if (ms) {
        p_fbtex2dms(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->color, 0, ms);
        p_rbstoragems(GL_RENDERBUFFER, ms, GL_DEPTH_COMPONENT16, w, h);
    } else {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->color, 0);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->depth);
    if (ms && glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[dx7] MSAA render target incomplete; anti-aliasing off\n");
        g_msaa = 0;                                 /* fall back for good */
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->color, 0);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->depth);
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        destroy(t);
        return error("incomplete GPU render target");
    }
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glDepthMask(1);
    glClearColor(0, 0, 0, 0);
    glClearDepthf(1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    gpu.stats.targets++;
    return 1;
}
static GLenum compare(int value) {
    static const GLenum values[] = {GL_ALWAYS,  GL_NEVER,    GL_LESS,   GL_EQUAL, GL_LEQUAL,
                                    GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
    return value >= 1 && value <= 8 ? values[value] : GL_ALWAYS;
}
static GLenum blend(int value) {
    static const GLenum values[] = {GL_ONE,
                                    GL_ZERO,
                                    GL_ONE,
                                    GL_SRC_COLOR,
                                    GL_ONE_MINUS_SRC_COLOR,
                                    GL_SRC_ALPHA,
                                    GL_ONE_MINUS_SRC_ALPHA,
                                    GL_DST_ALPHA,
                                    GL_ONE_MINUS_DST_ALPHA,
                                    GL_DST_COLOR,
                                    GL_ONE_MINUS_DST_COLOR,
                                    GL_SRC_ALPHA_SATURATE,
                                    GL_SRC_ALPHA_SATURATE,
                                    GL_SRC_ALPHA_SATURATE};
    return value >= 1 && value <= 13 ? values[value] : GL_ONE;
}
static GLenum primitive(int value) {
    static const GLenum values[] = {GL_TRIANGLES, GL_POINTS,         GL_LINES,       GL_LINE_STRIP,
                                    GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN};
    return value >= 1 && value <= 6 ? values[value] : GL_TRIANGLES;
}
static GLenum address(int value) {
    return value == 1                   ? GL_REPEAT
           : (value == 2 || value == 5) ? GL_MIRRORED_REPEAT
                                        : GL_CLAMP_TO_EDGE;
}
static void toggle(GLenum capability, int enabled) {
    if (enabled)
        glEnable(capability);
    else
        glDisable(capability);
}

/* ------------------------------------------------------------ state intern */
static uint32_t state_hash_of(const RState *s) {
    const uint32_t *w = (const uint32_t *)s;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof *s / 4; i++)
        h = (h ^ w[i]) * 16777619u;
    return h;
}
static void states_reset(void) {
    gpu.nstates = 0;
    memset(gpu.state_hash, 0xff, sizeof gpu.state_hash);
    gpu.epoch++;
    for (int i = 0; i < gpu.variant_count; i++)
        gpu.variants[i].program.applied_state = -1;
    gpu.tl.applied_state = gpu.lit.applied_state = -1;
}
unsigned r_state_epoch(void) { return gpu.epoch; }
int r_state_intern(const RState *s) {
    if (!gpu.states) {
        gpu.states = (StateEntry *)calloc(STATE_CAP, sizeof(StateEntry));
        if (!gpu.states)
            return -1;
        states_reset();
    }
    uint32_t h = state_hash_of(s);
    unsigned k = h & (STATE_HASH - 1);
    while (gpu.state_hash[k] >= 0) {
        StateEntry *e = &gpu.states[gpu.state_hash[k]];
        if (e->hash == h && !memcmp(&e->s, s, sizeof *s))
            return gpu.state_hash[k];
        k = (k + 1) & (STATE_HASH - 1);
    }
    if (gpu.nstates >= STATE_CAP) {
        /* commands reference ids: finish them, then start a new epoch */
        sync_all();
        states_reset();
        return r_state_intern(s);
    }
    int id = gpu.nstates++;
    StateEntry *e = &gpu.states[id];
    memcpy(&e->s, s, sizeof *s);
    e->hash = h;
    e->prog[0] = e->prog[1] = NULL;
    gpu.state_hash[k] = id;
    return id;
}

/* ------------------------------------------------------- raster + uniforms */
static void raster(const RState *s) {
    Raster *r = &gpu.raster;
    int v = r->valid;
    if (!v || r->depth_test != s->depth_test)
        toggle(GL_DEPTH_TEST, s->depth_test), r->depth_test = s->depth_test;
    if (!v || r->depth_func != s->depth_func)
        glDepthFunc(compare(s->depth_func)), r->depth_func = s->depth_func;
    if (!v || r->depth_write != (s->depth_write != 0))
        glDepthMask(s->depth_write != 0), r->depth_write = s->depth_write != 0;
    if (!v || r->blend != s->alpha_blend)
        toggle(GL_BLEND, s->alpha_blend), r->blend = s->alpha_blend;
    if (!v || r->src != s->src_blend || r->dst != s->dst_blend) {
        glBlendFunc(blend(s->src_blend), blend(s->dst_blend));
        r->src = s->src_blend;
        r->dst = s->dst_blend;
    }
    if (!v || r->cull != s->cull) {
        toggle(GL_CULL_FACE, s->cull > 1);
        glFrontFace(GL_CCW);
        glCullFace(s->cull == 2 ? GL_FRONT : GL_BACK);
        r->cull = s->cull;
    }
    if (!v || r->zbias != s->zbias) {
        toggle(GL_POLYGON_OFFSET_FILL, s->zbias > 0);
        glPolygonOffset(0, -(float)s->zbias);
        r->zbias = s->zbias;
    }
    int vw = s->vp_w ? (int)s->vp_w : gpu.back.w, vh = s->vp_h ? (int)s->vp_h : gpu.back.h;
    if (!v || r->vx != (int)s->vp_x || r->vy != (int)s->vp_y || r->vw != vw || r->vh != vh) {
        glViewport((int)s->vp_x, (int)s->vp_y, vw, vh);
        r->vx = (int)s->vp_x;
        r->vy = (int)s->vp_y;
        r->vw = vw;
        r->vh = vh;
    }
    if (!v || r->minz != s->vp_minz || r->maxz != s->vp_maxz) {
        glDepthRangef(s->vp_minz, s->vp_maxz);
        r->minz = s->vp_minz;
        r->maxz = s->vp_maxz;
    }
    r->valid = 1;
}
static void uniforms(Program *p, const RState *s, const float *matrices) {
    const float viewport[4] = {s->vp_x, s->vp_y, s->vp_w ? s->vp_w : gpu.back.w,
                               s->vp_h ? s->vp_h : gpu.back.h};
    uv(p, U_VIEWPORT, viewport);
    ui(p, U_TEXGEN, s->texgen[0] | (s->texgen[1] << 1));
    if (s->texgen[0])
        um(p, U_TEXMAT0, s->texmat[0]);
    if (s->texgen[1])
        um(p, U_TEXMAT1, s->texmat[1]);
    ui(p, U_FOG, s->fog);
    ui(p, U_FOGVERTEX, s->fog_vertex);
    ui(p, U_FOGMODE, s->fog_mode);
    uf(p, U_FOGSTART, s->fog_start);
    uf(p, U_FOGEND, s->fog_end);
    uf(p, U_FOGDENSITY, s->fog_density);
    ui(p, U_TEX0, 0);
    ui(p, U_TEX1, 1);
    ui(p, U_PERSP, gpu.persp);
    uc(p, U_TFACTOR, s->texture_factor);
    if (!p->specialized) {
        ui(p, U_STAGE0_ON, s->stage_on[0]);
        ui(p, U_STAGE1_ON, s->stage_on[1]);
        for (int k = 0; k < 2; k++) {
            ui(p, U_C0OP + k * 6, s->colorop[k]);
            ui(p, U_C0A1 + k * 6, s->colorarg[k][0]);
            ui(p, U_C0A2 + k * 6, s->colorarg[k][1]);
            ui(p, U_A0OP + k * 6, s->alphaop[k]);
            ui(p, U_A0A1 + k * 6, s->alphaarg[k][0]);
            ui(p, U_A0A2 + k * 6, s->alphaarg[k][1]);
        }
        ui(p, U_ALPHA_TEST, s->alpha_test);
        ui(p, U_ALPHA_FUNC, s->alpha_func);
        ui(p, U_COLORKEY_ON, s->colorkey_enable && s->colorkey_valid && s->stage_on[0]);
        ui(p, U_FOG_ON, s->fog && s->fog_mode);
        ui(p, U_SPECULAR_ADD, s->specular_enable);
        ui(p, U_FLAT_DIFFUSE, 0);
    }
    uf(p, U_ALPHA_REF, s->alpha_ref / 255.f);
    if (s->colorkey_enable) {
        uv(p, U_COLORKEY_LO, s->colorkey_lo);
        uv(p, U_COLORKEY_HI, s->colorkey_hi);
    }
    uc(p, U_FOG_COLOR, s->fog_color);
    if (p->lit && matrices) {
        um(p, U_WORLD, matrices);
        um(p, U_VIEW, matrices + 16);
        um(p, U_PROJ, matrices + 32);
        ui(p, U_LIGHTING, s->lighting);
        uv(p, U_MAT_DIFFUSE, s->material_diffuse);
        uv(p, U_MAT_AMBIENT, s->material_ambient);
        float spec[4];
        memcpy(spec, s->material_specular, sizeof spec);
        spec[3] = s->material_power;
        uv(p, U_MAT_SPECULAR, spec);
        uv(p, U_MAT_EMISSIVE, s->material_emissive);
        uv(p, U_AMBIENT, s->global_ambient);
        float diffuse[32] = {0}, specular[32] = {0}, ambient[32] = {0}, position[32] = {0},
              parameter[32] = {0};
        int n = s->light_count < 8 ? s->light_count : 8;
        for (int i = 0; i < n; i++) {
            const RLight *l = &s->lights[i];
            memcpy(diffuse + i * 4, l->diffuse, 16);
            memcpy(specular + i * 4, l->specular, 16);
            memcpy(ambient + i * 4, l->ambient, 16);
            memcpy(position + i * 4, l->type == 3 ? l->direction : l->position, 12);
            parameter[i * 4] = l->type;
            parameter[i * 4 + 1] = l->range;
            parameter[i * 4 + 2] = l->falloff;
        }
        ui(p, U_LIGHT_COUNT, n);
        if (n > 0) {
            if (p->loc[U_LIGHT_DIFFUSE] >= 0) glUniform4fv(p->loc[U_LIGHT_DIFFUSE], n, diffuse);
            if (p->loc[U_LIGHT_SPECULAR] >= 0) glUniform4fv(p->loc[U_LIGHT_SPECULAR], n, specular);
            if (p->loc[U_LIGHT_AMBIENT] >= 0) glUniform4fv(p->loc[U_LIGHT_AMBIENT], n, ambient);
            if (p->loc[U_LIGHT_POS] >= 0) glUniform4fv(p->loc[U_LIGHT_POS], n, position);
            if (p->loc[U_LIGHT_PARAM] >= 0) glUniform4fv(p->loc[U_LIGHT_PARAM], n, parameter);
        }
    }
}

/* ------------------------------------------------------------- textures */
static TexParams *tex_params_slot(GLuint name) {
    if (name >= gpu.tp_cap) {
        size_t cap = gpu.tp_cap ? gpu.tp_cap : 1024;
        while (cap <= name)
            cap *= 2;
        TexParams *p = (TexParams *)realloc(gpu.tp, cap * sizeof *p);
        if (!p)
            return NULL;
        for (size_t i = gpu.tp_cap; i < cap; i++)
            p[i].min = p[i].mag = p[i].ws = p[i].wt = -1;
        gpu.tp = p;
        gpu.tp_cap = cap;
    }
    return &gpu.tp[name];
}
static void tex_params_forget(GLuint name) {
    if (name < gpu.tp_cap)
        gpu.tp[name].min = gpu.tp[name].mag = gpu.tp[name].ws = gpu.tp[name].wt = -1;
}
static void bind_unit(int k, GLuint tex, const RState *s, int levels) {
    if (gpu.bound[k] != tex) {
        if (gpu.active_unit != k) {
            glActiveTexture(GL_TEXTURE0 + k);
            gpu.active_unit = k;
        }
        glBindTexture(GL_TEXTURE_2D, tex);
        gpu.bound[k] = tex;
        gpu.stats.tex_binds++;
    }
    int point = s->min_filter[k] == 1, mip = s->mip_filter[k];
    int detail = detail_level();
    GLint min = point ? GL_NEAREST : GL_LINEAR;
    if (detail == DETAIL_HIGH && !point && levels > 1)
        min = GL_LINEAR_MIPMAP_LINEAR;              /* the game had mip chains on real hardware */
    else if (mip && levels > 1)
        min = point ? (mip == 2 ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST)
                    : (mip == 2 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST);
    GLint mag = s->mag_filter[k] == 1 ? GL_NEAREST : GL_LINEAR;
    if (detail == DETAIL_LOW)
        min = mag = GL_NEAREST;
    GLint ws = address(s->address[k][0]), wt = address(s->address[k][1]);
    TexParams *tp = tex_params_slot(tex);
    if (tp && tp->min == min && tp->mag == mag && tp->ws == ws && tp->wt == wt)
        return;
    if (gpu.active_unit != k) {
        glActiveTexture(GL_TEXTURE0 + k);
        gpu.active_unit = k;
    }
    if (!tp || tp->min != min) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min);
    if (!tp || tp->mag != mag) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
    if (!tp || tp->ws != ws) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ws);
    if (!tp || tp->wt != wt) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wt);
    if (tp) {
        tp->min = min;
        tp->mag = mag;
        tp->ws = ws;
        tp->wt = wt;
    }
}

/* ------------------------------------------------------------ attributes */
static void attributes_tl(void) {
    glBindBuffer(GL_ARRAY_BUFFER, gpu.stream);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(TLV), (void *)offsetof(TLV, pos));
    glDisableVertexAttribArray(1);
    glVertexAttrib3f(1, 0, 0, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(TLV), (void *)offsetof(TLV, c0));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(TLV), (void *)offsetof(TLV, c1));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, sizeof(TLV), (void *)offsetof(TLV, uv));
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 2, GL_FLOAT, GL_FALSE, sizeof(TLV),
                          (void *)(offsetof(TLV, uv) + 2 * sizeof(float)));
}
static void attributes_lit(void) {
    glBindBuffer(GL_ARRAY_BUFFER, gpu.stream_lit);
    const GLint sizes[] = {4, 3, 4, 4, 2, 2};
    const size_t offsets[] = {offsetof(LitV, pos),   offsetof(LitV, normal),
                              offsetof(LitV, color), offsetof(LitV, spec),
                              offsetof(LitV, uv),    offsetof(LitV, uv) + 2 * sizeof(float)};
    for (int i = 0; i < 6; i++) {
        glEnableVertexAttribArray(i);
        glVertexAttribPointer(i, sizes[i], GL_FLOAT, GL_FALSE, sizeof(LitV), (void *)offsets[i]);
    }
}

/* Fixed-function state is constant for a draw. Compile it into the fragment
 * program instead of interpreting both texture combiners at every pixel. */
static const char *const shader_constants[20] = {
    "u_stage0_on",  "u_stage1_on",   "u_c0op",   "u_c0a1",         "u_c0a2",
    "u_a0op",       "u_a0a1",        "u_a0a2",   "u_c1op",         "u_c1a1",
    "u_c1a2",       "u_a1op",        "u_a1a1",   "u_a1a2",         "u_alpha_test",
    "u_alpha_func", "u_colorkey_on", "u_fog_on", "u_specular_add", "u_flat_diffuse"};
static Program *draw_program(const RState *s, int lit) {
    Program *fallback = lit ? &gpu.lit : &gpu.tl;
    static int generic = -1;
    if (generic < 0)
        generic = getenv("NFS5_GENERIC_SHADER") != NULL;
    if (generic)
        return fallback;
    ShaderKey key;
    memset(&key, 0, sizeof key);
    int vals[20] = {s->stage_on[0],
                    s->stage_on[1],
                    s->colorop[0],
                    s->colorarg[0][0],
                    s->colorarg[0][1],
                    s->alphaop[0],
                    s->alphaarg[0][0],
                    s->alphaarg[0][1],
                    s->colorop[1],
                    s->colorarg[1][0],
                    s->colorarg[1][1],
                    s->alphaop[1],
                    s->alphaarg[1][0],
                    s->alphaarg[1][1],
                    s->alpha_test,
                    s->alpha_test ? s->alpha_func : 8,
                    s->colorkey_enable && s->colorkey_valid && s->stage_on[0],
                    s->fog && s->fog_mode,
                    s->specular_enable,
                    0};
    memcpy(key.value, vals, sizeof vals);
    key.lit = lit;
    for (int stage = 0; stage < 2; stage++) {
        if (!key.value[stage])
            memset(key.value + 2 + stage * 6, 0, 6 * sizeof(int));
    }
    for (int i = 0; i < gpu.variant_count; i++)
        if (!memcmp(&key, &gpu.variants[i].key, sizeof key))
            return gpu.variants[i].program.object ? &gpu.variants[i].program : fallback;
    if (gpu.variant_count == 128)
        return fallback;
    char source[16384];
    size_t used = 0;
    const char *line = FF_FS;
    while (*line) {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) + 1 : strlen(line);
        if (!strncmp(line, "uniform int ", 12)) {
            const char *name = line + 12;
            while (name < line + length && *name != ';' && *name != '\n') {
                while (*name == ' ' || *name == ',')
                    name++;
                const char *stop = name;
                while ((*stop >= 'a' && *stop <= 'z') || (*stop >= '0' && *stop <= '9') ||
                       *stop == '_')
                    stop++;
                int index = -1;
                for (int i = 0; i < 20; i++)
                    if (strlen(shader_constants[i]) == (size_t)(stop - name) &&
                        !strncmp(name, shader_constants[i], stop - name)) {
                        index = i;
                        break;
                    }
                if (index < 0)
                    return fallback;
                int n = snprintf(source + used, sizeof source - used, "const int %s = %d;\n",
                                 shader_constants[index], key.value[index]);
                if (n < 0 || (size_t)n >= sizeof source - used)
                    return fallback;
                used += (size_t)n;
                name = stop;
            }
        } else {
            if (length >= sizeof source - used)
                return fallback;
            memcpy(source + used, line, length);
            used += length;
        }
        line += length;
    }
    source[used] = 0;
    Variant *variant = &gpu.variants[gpu.variant_count++];
    variant->key = key;
    if (!program(&variant->program, lit ? FF_VS_LIT : FF_VS_TL, source, lit)) {
        if (variant->program.object)
            glDeleteProgram(variant->program.object);
        memset(&variant->program, 0, sizeof variant->program);
        return fallback;
    }
    variant->program.specialized = 1;
    gl_state_unknown();
    fprintf(stderr, "[dx7] compiled fragment variant %d\n", gpu.variant_count);
    return &variant->program;
}


/* ======================================================================
 * Worker side: everything below that issues GL runs on the thread that owns
 * the context (the GL worker when NFS5_GL_THREAD is on, otherwise the caller).
 * ====================================================================== */
uint64_t g_r_exec_ns, g_r_exec_calls;   /* NFS5_DRAWPROF: GL execution time */
uint64_t g_r_swap_ns, g_r_swaps, g_r_wait_ns, g_r_batch_ns;   /* worker swap, caller wait, worker busy */
static uint64_t rnow_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
uint64_t g_r_texonly_breaks, g_r_state_breaks;   /* NFS5_DRAWPROF: why batches split */

static GLuint tex_name(GLuint id) {
    if (!id)
        return gpu.white;
    return id < gpu.names_cap ? gpu.names[id] : gpu.white;
}
static void name_set(GLuint id, GLuint name) {
    if (id >= gpu.names_cap) {
        size_t cap = gpu.names_cap ? gpu.names_cap : 1024;
        while (cap <= id)
            cap *= 2;
        GLuint *p = realloc(gpu.names, cap * sizeof *p);
        if (!p)
            return;
        memset(p + gpu.names_cap, 0, (cap - gpu.names_cap) * sizeof *p);
        gpu.names = p;
        gpu.names_cap = cap;
    }
    gpu.names[id] = name;
}
static void upload_vertices(Batch *b) {
    if (b->used) {
        glBindBuffer(GL_ARRAY_BUFFER, gpu.stream);
        glBufferData(GL_ARRAY_BUFFER, b->used * sizeof(TLV), b->tlv, GL_STREAM_DRAW);
    }
    if (b->lit_used) {
        glBindBuffer(GL_ARRAY_BUFFER, gpu.stream_lit);
        glBufferData(GL_ARRAY_BUFFER, b->lit_used * sizeof(LitV), b->litv, GL_STREAM_DRAW);
    }
}
static void execute_cmds(Batch *b, size_t first, size_t n) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    glBindFramebuffer(GL_FRAMEBUFFER, gpu.back.fbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    int format = -1;
    Program *active = NULL;
    for (size_t i = first; i < first + n; i++) {
        Command *c = &b->commands[i];
        StateEntry *se = &gpu.states[c->state];
        Program *p = se->prog[c->lit];
        if (!p)
            p = se->prog[c->lit] = draw_program(&se->s, c->lit);
        if (format != c->lit) {
            if (c->lit)
                attributes_lit();
            else
                attributes_tl();
            format = c->lit;
        }
        if (p != active) {
            glUseProgram(p->object);
            active = p;
        }
        raster(&se->s);
        if (p->applied_state != c->state || (c->lit && p->applied_matrices != c->matrices)) {
            uniforms(p, &se->s, c->lit ? b->matrices + (size_t)c->matrices * 48 : NULL);
            p->applied_state = c->state;
            p->applied_matrices = c->lit ? c->matrices : -1;
        }
        for (int k = 0; k < 2; k++) {
            if (k == 1 && !se->s.stage_on[1] && !c->textures[1])
                continue;   /* stage 1 unused: leave whatever is bound */
            bind_unit(k, tex_name(c->textures[k]), &se->s, c->levels[k]);
        }
        glDrawArrays(primitive(c->primitive), (GLint)c->first, (GLsizei)c->count);
        gpu.stats.draws++;
        gpu.stats.vertices += c->count;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    g_r_exec_ns += (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000000ll + (t1.tv_nsec - t0.tv_nsec));
    g_r_exec_calls++;
}
static void batch_reset(Batch *b) {
    b->used = b->lit_used = b->mat_used = b->count = b->draw_start = 0;
    b->nops = b->arena_used = 0;
    /* uniforms referencing matrix-pool slots are stale after the reset */
    for (int i = 0; i < gpu.variant_count; i++)
        gpu.variants[i].program.applied_matrices = -1;
    gpu.lit.applied_matrices = -1;
}
static void gl_clear_rect(uint32_t c, float z, int color, int depth, int x, int y, int w, int h) {
    glBindFramebuffer(GL_FRAMEBUFFER, gpu.back.fbo);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w, h);
    glColorMask(1, 1, 1, 1);
    glDepthMask(1);
    gpu.raster.depth_write = 1;
    glClearColor(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f,
                 (c >> 24) / 255.f);
    glClearDepthf(z);
    glClear((color ? GL_COLOR_BUFFER_BIT : 0) | (depth ? GL_DEPTH_BUFFER_BIT : 0));
    glDisable(GL_SCISSOR_TEST);
    gpu.stats.clears++;
}
static void copy_texture(GLuint texture, GLuint fbo, int x, int y, int w, int h, int flip) {
    float q[4][4];
    const float xy[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    for (int i = 0; i < 4; i++) {
        q[i][0] = xy[i * 2];
        q[i][1] = xy[i * 2 + 1];
        q[i][2] = (float)(i & 1);
        q[i][3] = flip ? 1.0f - (float)(i >> 1) : (float)(i >> 1);
    }
    gl_state_unknown();
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(x, y, w, h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glColorMask(1, 1, 1, 1);
    glUseProgram(gpu.copy.object);
    ui(&gpu.copy, U_IMAGE, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    tex_params_forget(texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindBuffer(GL_ARRAY_BUFFER, gpu.stream);
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
    for (int i = 1; i < 6; i++)
        if (i != 4)
            glDisableVertexAttribArray(i);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl_state_unknown();
}
static void fps_overlay(int fps, int height) {
    static const unsigned char masks[] = {63, 6, 91, 79, 102, 109, 125, 7, 127, 111};
    static const int bars[7][4] = {{0, 0, 3, 1}, {3, 0, 1, 3}, {3, 3, 1, 3}, {0, 6, 3, 1},
                                   {0, 3, 1, 3}, {0, 0, 1, 3}, {0, 3, 3, 1}};
    glEnable(GL_SCISSOR_TEST);
    glScissor(4, height - 30, 68, 26);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    char text[8];
    snprintf(text, sizeof text, "%d", fps > 999 ? 999 : fps);
    glClearColor(1, 1, 1, 1);
    for (int i = 0; text[i]; i++)
        for (int b = 0; b < 7; b++)
            if (masks[text[i] - '0'] & (1 << b)) {
                int x = 8 + i * 15 + bars[b][0] * 3, y = 8 + bars[b][1] * 3, w = bars[b][2] * 3,
                    h = bars[b][3] * 3;
                glScissor(x, height - y - h, w, h);
                glClear(GL_COLOR_BUFFER_BIT);
            }
    glDisable(GL_SCISSOR_TEST);
}
static int read_target(Target *t, void *p, int w, int h, int packed) {
    if (!t->fbo || !p || w <= 0 || h <= 0 || w > t->w || h > t->h)
        return 0;
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, packed ? 2 : 1);
    while (glGetError() != GL_NO_ERROR) {
    }
    glReadPixels(0, 0, w, h, packed ? GL_RGB : GL_RGBA,
                 packed ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_BYTE, p);
    int ok = glGetError() == GL_NO_ERROR;
    glBindFramebuffer(GL_FRAMEBUFFER, gpu.back.fbo);
    if (ok) {
        gpu.stats.readbacks++;
        gpu.stats.readback_bytes += (uint64_t)w * h * (packed ? 2 : 4);
    }
    return ok;
}
static void gl_present(int ww, int wh, int stretch, int fps) {
    if (!gpu.front.fbo)
        return;
    if (fe.threaded) {
        /* the window surface is shared with SDL_Renderer on the video thread */
        pthread_mutex_lock(&fe.surface);
        if (!gles_ctx_bind_window()) {
            static unsigned fails;
            if (fails++ < 5 || (fails & 255) == 0)
                fprintf(stderr, "[dx7] GL worker: window bind failed (%u): %s\n", fails,
                        gles_ctx_last_sdl_error());
            pthread_mutex_unlock(&fe.surface);
            return;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    int w = ww, h = wh;
    if (!stretch) {
        if ((int64_t)ww * gpu.front.h > (int64_t)wh * gpu.front.w)
            w = (int)((int64_t)wh * gpu.front.w / gpu.front.h);
        else
            h = (int)((int64_t)ww * gpu.front.h / gpu.front.w);
    }
    copy_texture(gpu.front.color, 0, (ww - w) / 2, (wh - h) / 2, w, h, 1);
    if (fps >= 0)
        fps_overlay(fps, wh);
    uint64_t ts0 = rnow_ns();
    gles_ctx_swap();
    g_r_swap_ns += rnow_ns() - ts0;
    g_r_swaps++;
    if (fe.threaded) {
        gles_ctx_surfaceless();
        pthread_mutex_unlock(&fe.surface);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, gpu.back.fbo);
}
/* one recorded op (worker side) */
static void run_op(Batch *b, Op *o) {
    switch (o->type) {
    case OP_DRAWS:
        execute_cmds(b, (size_t)o->a, (size_t)o->b);
        break;
    case OP_CLEAR:
        gl_clear_rect(o->u, o->z, o->a, o->b, o->c, o->d, o->e, o->f);
        break;
    case OP_TARGET:
        target(&gpu.back, o->a, o->b, o->c);
        break;
    case OP_TEX_CREATE: {
        GLuint t = 0;
        glGenTextures(1, &t);
        tex_params_forget(t);
        name_set((GLuint)o->a, t);
        break;
    }
    case OP_TEX_UPLOAD: {
        GLuint t = tex_name((GLuint)o->a);
        glActiveTexture(GL_TEXTURE0);
        gpu.active_unit = 0;
        glBindTexture(GL_TEXTURE_2D, t);
        gpu.bound[0] = t;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, o->b, o->c, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     b->arena + o->off);
        gpu.stats.tex_uploads++;
        gpu.stats.tex_upload_bytes += (uint64_t)o->b * o->c * 4;
        break;
    }
    case OP_TEX_DELETE: {
        GLuint t = tex_name((GLuint)o->a);
        if (t && t != gpu.white) {
            for (int k = 0; k < 2; k++)
                if (gpu.bound[k] == t)
                    gpu.bound[k] = (GLuint)-1;
            tex_params_forget(t);
            glDeleteTextures(1, &t);
        }
        name_set((GLuint)o->a, 0);
        break;
    }
    case OP_TEX_MIPMAP: {
        GLuint t = tex_name((GLuint)o->a);
        glActiveTexture(GL_TEXTURE0);
        gpu.active_unit = 0;
        glBindTexture(GL_TEXTURE_2D, t);
        gpu.bound[0] = t;
        glGenerateMipmap(GL_TEXTURE_2D);
        detail_caps();
        if (g_aniso)
            glTexParameterf(GL_TEXTURE_2D, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY_EXT */, 4.0f);
        break;
    }
    case OP_TARGET_LOAD:
        if (!gpu.upload)
            glGenTextures(1, &gpu.upload);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gpu.upload);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, o->a, o->b, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     b->arena + o->off);
        copy_texture(gpu.upload, gpu.back.fbo, 0, 0, gpu.back.w, gpu.back.h, 0);
        break;
    case OP_FLUSH:
        glFlush();
        break;
    case OP_SNAPSHOT:
        if (target(&gpu.front, gpu.back.w, gpu.back.h, gpu.back.packed)) {
            Target completed = gpu.back;
            gpu.back = gpu.front;
            gpu.front = completed;
            glBindFramebuffer(GL_FRAMEBUFFER, gpu.back.fbo);
        }
        break;
    case OP_PRESENT:
        gl_present(o->a, o->b, o->c, o->d);
        break;
    case OP_READ: {
        Target *t = o->a ? &gpu.front : &gpu.back;
        int ok = read_target(t, o->dst, o->b, o->c, o->d);
        if (o->result)
            *o->result = ok;
        break;
    }
    }
}
static void process_batch(Batch *b) {
    upload_vertices(b);
    for (size_t i = 0; i < b->nops; i++)
        run_op(b, &b->ops[i]);
    batch_reset(b);
}

/* ======================================================================
 * Worker thread
 * ====================================================================== */
static int gl_init_objects(void);
static void *worker_main(void *arg) {
    (void)arg;
    int st = 3;
    int ctx = gles_ctx_init();
    const char *ver = ctx ? (const char *)glGetString(GL_VERSION) : NULL;
    int objs = ctx && ver ? gl_init_objects() : 0;
    fprintf(stderr, "[dx7] GL worker: context=%d version=%s objects=%d (%s)\n", ctx,
            ver ? "ok" : "none", objs, gpu.error[0] ? gpu.error : "-");
    if (ctx && objs) {
        int sl = gles_ctx_surfaceless();
        st = sl ? 1 : 2;
        if (st == 1) {
            /* some stacks accept a surfaceless bind but cannot render: prove
             * the context works for FBO rendering before relying on it */
            glBindFramebuffer(GL_FRAMEBUFFER, gpu.back.fbo);
            GLenum fb = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (!glGetString(GL_VERSION) || fb != GL_FRAMEBUFFER_COMPLETE)
                st = 2;
            fprintf(stderr, "[dx7] GL worker: surfaceless bind ok, fbo status 0x%x -> %s\n", fb,
                    st == 1 ? "threaded" : "fallback");
        } else {
            fprintf(stderr, "[dx7] GL worker: surfaceless bind refused (%s)\n", gles_ctx_last_sdl_error());
        }
        if (st == 2)
            gles_ctx_unbind_current();     /* hand the context back to the caller */
    }
    pthread_mutex_lock(&fe.mu);
    fe.init_state = st;
    pthread_cond_broadcast(&fe.cv);
    pthread_mutex_unlock(&fe.mu);
    if (st != 1)
        return NULL;
    for (;;) {
        pthread_mutex_lock(&fe.mu);
        while (!fe.pending)
            pthread_cond_wait(&fe.cv, &fe.mu);
        Batch *b = fe.pending;
        fe.pending = NULL;
        fe.busy = 1;
        pthread_mutex_unlock(&fe.mu);
        uint64_t tb0 = rnow_ns();
        process_batch(b);
        g_r_batch_ns += rnow_ns() - tb0;
        pthread_mutex_lock(&fe.mu);
        fe.busy = 0;
        pthread_cond_broadcast(&fe.cv);
        pthread_mutex_unlock(&fe.mu);
    }
    return NULL;
}

/* ======================================================================
 * Front end (r_* API): records ops into the current batch
 * ====================================================================== */
static Op *op_new(int type) {
    Batch *b = FB;
    if (b->nops == b->ops_cap) {
        size_t cap = b->ops_cap ? b->ops_cap * 2 : 64;
        Op *p = realloc(b->ops, cap * sizeof *p);
        if (!p) {
            error("op queue allocation failed");
            return NULL;
        }
        b->ops = p;
        b->ops_cap = cap;
    }
    Op *o = &b->ops[b->nops++];
    memset(o, 0, sizeof *o);
    o->type = type;
    return o;
}
static size_t arena_put(const void *data, size_t n) {
    Batch *b = FB;
    if (b->arena_used + n > b->arena_cap) {
        size_t cap = b->arena_cap ? b->arena_cap : (1u << 20);
        while (cap < b->arena_used + n)
            cap *= 2;
        uint8_t *p = realloc(b->arena, cap);
        if (!p) {
            error("arena allocation failed");
            return (size_t)-1;
        }
        b->arena = p;
        b->arena_cap = cap;
    }
    size_t off = b->arena_used;
    memcpy(b->arena + off, data, n);
    b->arena_used += (n + 15) & ~(size_t)15;
    return off;
}
/* close the run of commands queued since the last op */
static void flush_draws(void) {
    Batch *b = FB;
    if (b->count == b->draw_start)
        return;
    if (fe.threaded) {
        Op *o = op_new(OP_DRAWS);
        if (o) {
            o->a = (int)b->draw_start;
            o->b = (int)(b->count - b->draw_start);
        }
        b->draw_start = b->count;
        return;
    }
    gles_ctx_ensure();
    upload_vertices(b);
    execute_cmds(b, b->draw_start, b->count - b->draw_start);
    batch_reset(b);
}
/* record (threaded) or run now (synchronous) */
static void issue(Op *proto) {
    flush_draws();
    if (fe.threaded) {
        Op *o = op_new(proto->type);
        if (o)
            *o = *proto;
        return;
    }
    gles_ctx_ensure();
    run_op(FB, proto);
    FB->arena_used = 0;
}
static void submit(int wait) {
    if (!fe.threaded)
        return;
    flush_draws();
    Batch *b = FB;
    pthread_mutex_lock(&fe.mu);
    uint64_t tw0 = rnow_ns();
    while (fe.pending || fe.busy)
        pthread_cond_wait(&fe.cv, &fe.mu);
    g_r_wait_ns += rnow_ns() - tw0;
    if (b->nops || b->used || b->lit_used) {
        fe.pending = b;
        fe.cur ^= 1;
        pthread_cond_broadcast(&fe.cv);
    }
    if (wait)
        while (fe.pending || fe.busy)
            pthread_cond_wait(&fe.cv, &fe.mu);
    pthread_mutex_unlock(&fe.mu);
}
static void sync_all(void) {
    if (fe.threaded)
        submit(1);
    else
        flush_draws();
}
void r_window_acquire(void) {
    if (fe.threaded)
        pthread_mutex_lock(&fe.surface);
}
/* drop this thread's GL context from the window (no lock; idempotent) */
void r_window_drop(void) {
    if (fe.threaded)
        gles_ctx_unbind_current();
}
void r_window_release(void) {
    if (!fe.threaded)
        return;
    gles_ctx_unbind_current();     /* SDL_Renderer's context off the window */
    pthread_mutex_unlock(&fe.surface);
}

/* ------------------------------------------------------------- queueing */
static int reserve_cmd(void) {
    Batch *b = FB;
    if (b->count == b->command_capacity) {
        size_t cap = b->command_capacity ? b->command_capacity * 2 : 256;
        Command *p = realloc(b->commands, cap * sizeof *p);
        if (!p)
            return error("command queue allocation failed");
        b->commands = p;
        b->command_capacity = cap;
    }
    return 1;
}
static int reserve_tl(size_t n) {
    if (FB->used + n > 262144) {
        if (fe.threaded)
            submit(0);
        else
            flush_draws();
    }
    Batch *b = FB;
    if (b->used + n > b->capacity) {
        size_t cap = b->capacity ? b->capacity : 8192;
        while (cap < b->used + n)
            cap *= 2;
        TLV *p = realloc(b->tlv, cap * sizeof *p);
        if (!p)
            return error("vertex queue allocation failed");
        b->tlv = p;
        b->capacity = cap;
    }
    return reserve_cmd();
}
static int independent(int prim, size_t n) {
    return prim == 1 || (prim == 2 && n % 2 == 0) || (prim == 4 && n % 3 == 0);
}
static void push_command(int state, int lit, int prim, size_t first, size_t n, int matrices) {
    Batch *b = FB;
    Command *last = b->count > b->draw_start ? &b->commands[b->count - 1] : NULL;
    if (last && !lit && !last->lit && last->primitive == prim && independent(prim, n) &&
        independent(last->primitive, last->count)) {
        if (last->state == state &&
            (last->textures[0] != fe.textures[0] || last->textures[1] != fe.textures[1]))
            g_r_texonly_breaks++;
        else if (last->state != state)
            g_r_state_breaks++;
    }
    if (last && !lit && !last->lit && last->state == state && last->primitive == prim &&
        independent(prim, n) && independent(last->primitive, last->count) &&
        last->first + last->count == first && last->textures[0] == fe.textures[0] &&
        last->textures[1] == fe.textures[1] && last->levels[0] == fe.levels[0] &&
        last->levels[1] == fe.levels[1]) {
        last->count += n;
        return;
    }
    Command *c = &b->commands[b->count++];
    c->state = state;
    c->lit = lit;
    c->primitive = prim;
    c->first = first;
    c->count = n;
    c->matrices = matrices;
    c->textures[0] = fe.textures[0];
    c->textures[1] = fe.textures[1];
    c->levels[0] = fe.levels[0];
    c->levels[1] = fe.levels[1];
}
/* Raw TL vertices in the THRASH/D3D layout: stride 32 (FVF 0x1C4: one uv) or
 * 40 (0x2C4: two uvs). Indices are de-indexed into the stream. */
void r_draw_tlv(int state, const void *vertices, int stride, int nverts, const uint16_t *idx,
                int nidx, int prim) {
    if (!gpu.ready || !vertices || nverts <= 0 || state < 0 || state >= gpu.nstates)
        return;
    size_t n = idx && nidx > 0 ? (size_t)nidx : (size_t)nverts;
    if (n > 1048576)
        return;
    if (idx && nidx > 0)
        for (size_t i = 0; i < n; i++)
            if (idx[i] >= nverts) {
                error("vertex index outside draw");
                return;
            }
    unsigned epoch = gpu.epoch;
    if (!reserve_tl(n))
        return;
    if (epoch != gpu.epoch)
        return;   /* state ids were recycled by a flush; caller re-interns next draw */
    Batch *b = FB;
    const uint8_t *base = (const uint8_t *)vertices;
    TLV *out = b->tlv + b->used;
    size_t first = b->used;
    if (stride == 40) {
        for (size_t i = 0; i < n; i++)
            memcpy(&out[i], base + (size_t)(idx && nidx > 0 ? idx[i] : i) * 40, 40);
    } else {
        for (size_t i = 0; i < n; i++) {
            memcpy(&out[i], base + (size_t)(idx && nidx > 0 ? idx[i] : i) * (size_t)stride, 32);
            out[i].uv[2] = out[i].uv[3] = 0.0f;
        }
    }
    b->used += n;
    push_command(state, 0, prim, first, n, -1);
}
static uint32_t pack_color(const float c[4]) {
    uint32_t r, g, b, a;
    float v;
    v = c[0] < 0 ? 0 : c[0] > 1 ? 1 : c[0]; r = (uint32_t)(v * 255.0f + 0.5f);
    v = c[1] < 0 ? 0 : c[1] > 1 ? 1 : c[1]; g = (uint32_t)(v * 255.0f + 0.5f);
    v = c[2] < 0 ? 0 : c[2] > 1 ? 1 : c[2]; b = (uint32_t)(v * 255.0f + 0.5f);
    v = c[3] < 0 ? 0 : c[3] > 1 ? 1 : c[3]; a = (uint32_t)(v * 255.0f + 0.5f);
    return a << 24 | r << 16 | g << 8 | b;
}
void r_draw_tl(const RState *s, const RTLVertex *v, int count, const uint16_t *indices, int ni,
               int prim) {
    if (!gpu.ready || !v || count <= 0)
        return;
    int state = r_state_intern(s);
    if (state < 0)
        return;
    size_t n = indices && ni > 0 ? (size_t)ni : (size_t)count;
    if (n > 1048576)
        return;
    for (size_t i = 0; indices && ni > 0 && i < n; i++)
        if (indices[i] >= count) {
            error("vertex index outside draw");
            return;
        }
    unsigned epoch = gpu.epoch;
    if (!reserve_tl(n))
        return;
    if (epoch != gpu.epoch && (state = r_state_intern(s)) < 0)
        return;
    Batch *b = FB;
    size_t first = b->used;
    for (size_t i = 0; i < n; i++) {
        const RTLVertex *a = v + (indices && ni > 0 ? indices[i] : i);
        TLV *o = &b->tlv[b->used++];
        o->pos[0] = a->x; o->pos[1] = a->y; o->pos[2] = a->z; o->pos[3] = a->rhw;
        o->c0 = pack_color(a->color);
        o->c1 = pack_color(a->spec);
        o->uv[0] = a->tex0[0]; o->uv[1] = a->tex0[1];
        o->uv[2] = a->tex1[0]; o->uv[3] = a->tex1[1];
    }
    push_command(state, 0, prim, first, n, -1);
}
void r_draw_lit(const RState *s, const RLitVertex *v, int count, const uint16_t *indices, int ni,
                int prim, const float w[16], const float view[16], const float proj[16]) {
    if (!gpu.ready || !v || count <= 0)
        return;
    size_t n = indices && ni > 0 ? (size_t)ni : (size_t)count;
    if (n > 1048576)
        return;
    for (size_t i = 0; indices && ni > 0 && i < n; i++)
        if (indices[i] >= count) {
            error("vertex index outside draw");
            return;
        }
    if (FB->lit_used + n > 262144 || FB->mat_used + 1 > 65536) {
        if (fe.threaded)
            submit(0);
        else
            flush_draws();
    }
    int state = r_state_intern(s);
    if (state < 0)
        return;
    Batch *b = FB;
    if (b->lit_used + n > b->lit_capacity) {
        size_t cap = b->lit_capacity ? b->lit_capacity : 4096;
        while (cap < b->lit_used + n)
            cap *= 2;
        LitV *p = realloc(b->litv, cap * sizeof *p);
        if (!p) { error("lit vertex allocation failed"); return; }
        b->litv = p;
        b->lit_capacity = cap;
    }
    if (b->mat_used + 1 > b->mat_capacity) {
        size_t cap = b->mat_capacity ? b->mat_capacity * 2 : 64;
        float *p = realloc(b->matrices, cap * 48 * sizeof(float));
        if (!p) { error("matrix pool allocation failed"); return; }
        b->matrices = p;
        b->mat_capacity = cap;
    }
    if (!reserve_cmd())
        return;
    int mi = (int)b->mat_used++;
    float *m = b->matrices + (size_t)mi * 48;
    memcpy(m, w, 64);
    memcpy(m + 16, view, 64);
    memcpy(m + 32, proj, 64);
    size_t first = b->lit_used;
    for (size_t i = 0; i < n; i++) {
        const RLitVertex *a = v + (indices && ni > 0 ? indices[i] : i);
        LitV *o = &b->litv[b->lit_used++];
        memcpy(o->pos, &a->x, 12);
        o->pos[3] = 1;
        memcpy(o->normal, a->normal, 12);
        memcpy(o->color, a->color, 16);
        memcpy(o->spec, a->spec, 16);
        memcpy(o->uv, a->tex0, 8);
        memcpy(o->uv + 2, a->tex1, 8);
    }
    push_command(state, 1, prim, first, n, mi);
}

/* ------------------------------------------------------------- lifecycle */
static const char copy_vs[] = "attribute vec4 a_pos;attribute vec2 a_tex0;varying vec2 uv;void "
                              "main(){gl_Position=a_pos;uv=a_tex0;}";
static const char copy_fs[] = "precision mediump float;uniform sampler2D image;varying vec2 "
                              "uv;void main(){gl_FragColor=texture2D(image,uv);}";
/* GL objects every mode needs; runs on the context's thread */
static int gl_init_objects(void) {
    const char *affine = getenv("NFS5_AFFINE");
    gpu.persp = !(affine && *affine && strcmp(affine, "0") != 0);
    if (!program(&gpu.tl, FF_VS_TL, FF_FS, 0) || !program(&gpu.lit, FF_VS_LIT, FF_FS, 1) ||
        !program(&gpu.copy, copy_vs, copy_fs, 0))
        return 0;
    glGenBuffers(1, &gpu.stream);
    glGenBuffers(1, &gpu.stream_lit);
    glGenTextures(1, &gpu.white);
    glBindTexture(GL_TEXTURE_2D, gpu.white);
    uint32_t white = ~0u;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl_state_unknown();
    return target(&gpu.back, 640, 480, 0);
}
int r_init(void) {
    if (gpu.ready)
        return 1;
    if (!gpu.states) {
        gpu.states = (StateEntry *)calloc(STATE_CAP, sizeof(StateEntry));
        states_reset();
    }
    const char *v = getenv("NFS5_GL_THREAD");
    int want_thread = !(v && !strcmp(v, "0"));
    int ok = 0;
    if (want_thread) {
        /* the worker creates the context bound to the window: take the window
         * away from SDL_Renderer on this (video) thread first */
        pthread_mutex_lock(&fe.surface);
        gles_ctx_unbind_current();
        fe.init_state = 0;
        int prc = pthread_create(&fe.thread, NULL, worker_main, NULL);
        if (prc) fprintf(stderr, "[dx7] GL worker: pthread_create failed (%d)\n", prc);
        if (!prc) {
            pthread_mutex_lock(&fe.mu);
            while (!fe.init_state)
                pthread_cond_wait(&fe.cv, &fe.mu);
            pthread_mutex_unlock(&fe.mu);
            if (fe.init_state == 1) {
                fe.threaded = 1;
                ok = 1;
            } else {
                pthread_join(fe.thread, NULL);
                ok = fe.init_state == 2;   /* context exists, now usable from this thread */
            }
        }
        pthread_mutex_unlock(&fe.surface);
        if (fe.init_state == 2)
            fprintf(stderr, "[dx7] GL worker: no surfaceless context, rendering on the caller\n");
    }
    if (!ok) {
        if (!gles_ctx_init())
            return error("SDL GLES context unavailable");
        gles_ctx_ensure();
        if (fe.init_state != 2 && !gl_init_objects())
            return 0;
    }
    fe.back_w = 640;
    fe.back_h = 480;
    fe.back_packed = 0;
    gpu.ready = 1;
    fprintf(stderr, "[dx7] native command renderer: %s (perspective %s, %s)\n", gles_ctx_info(),
            gpu.persp ? "correct" : "affine", fe.threaded ? "GL worker thread" : "GL on caller");
    return 1;
}
void r_shutdown(void) {
    if (!gpu.ready)
        return;
    sync_all();   /* the worker stays parked; the process is ending */
}
int r_ready(void) { return gpu.ready; }
const char *r_context_info(void) { return gles_ctx_info(); }
const char *r_last_error(void) { return gpu.error[0] ? gpu.error : "ok"; }
void r_target_hint_565(int on) { fe.hint = on != 0; }
int r_target_is_565(void) { return fe.back_packed; }
int r_target_width(void) { return fe.back_w; }
int r_target_height(void) { return fe.back_h; }
int r_target(int w, int h) {
    if (!gpu.ready || w <= 0 || h <= 0)
        return 0;
    if (fe.back_w == w && fe.back_h == h && fe.back_packed == fe.hint)
        return 1;
    Op o = {.type = OP_TARGET, .a = w, .b = h, .c = fe.hint};
    issue(&o);
    fe.back_w = w;
    fe.back_h = h;
    fe.back_packed = fe.hint;
    return 1;
}
void r_set_viewport(int x, int y, int w, int h, float lo, float hi) {
    (void)x; (void)y; (void)w; (void)h; (void)lo; (void)hi; /* in each RState */
}
void r_clear_rect(uint32_t c, float z, int color, int depth, int x, int y, int w, int h) {
    if (!gpu.ready)
        return;
    Op o = {.type = OP_CLEAR, .u = c, .z = z, .a = color, .b = depth, .c = x, .d = y, .e = w, .f = h};
    issue(&o);
}
void r_clear(uint32_t c, float z, int color, int depth) {
    r_clear_rect(c, z, color, depth, 0, 0, fe.back_w, fe.back_h);
}
RTexture r_tex_create(void) {
    RTexture t = {0};
    if (!gpu.ready)
        return t;
    t.gl = fe.next_tex++;          /* an id; the context thread maps it to a GL name */
    t.levels = 1;
    Op o = {.type = OP_TEX_CREATE, .a = (int)t.gl};
    issue(&o);
    return t;
}
void r_tex_delete(RTexture *t) {
    if (!t || !t->gl)
        return;
    Op o = {.type = OP_TEX_DELETE, .a = (int)t->gl};
    issue(&o);
    memset(t, 0, sizeof *t);
}
void r_tex_upload(RTexture *t, const uint8_t *p, int w, int h) {
    if (!t || !t->gl || !p || w <= 0 || h <= 0)
        return;
    flush_draws();
    Op o = {.type = OP_TEX_UPLOAD, .a = (int)t->gl, .b = w, .c = h};
    if (fe.threaded) {
        o.off = arena_put(p, (size_t)w * h * 4);
        if (o.off == (size_t)-1)
            return;
        Op *q = op_new(OP_TEX_UPLOAD);
        if (q)
            *q = o;
    } else {
        /* synchronous: upload straight from the caller's buffer */
        gles_ctx_ensure();
        GLuint name = tex_name(t->gl);
        glActiveTexture(GL_TEXTURE0);
        gpu.active_unit = 0;
        glBindTexture(GL_TEXTURE_2D, name);
        gpu.bound[0] = name;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, p);
        gpu.stats.tex_uploads++;
        gpu.stats.tex_upload_bytes += (uint64_t)w * h * 4;
    }
    t->w = w;
    t->h = h;
    t->levels = 1;
    /* high detail: mip chain for textures that are not re-uploaded all the time */
    if (detail_level() == DETAIL_HIGH && ++t->uploads <= 3 && w >= 8 && h >= 8)
        r_tex_mipmap(t);
}
void r_tex_mipmap(RTexture *t) {
    if (!t || !t->gl || t->w <= 0 || t->h <= 0 || (t->w & (t->w - 1)) || (t->h & (t->h - 1)))
        return;
    Op o = {.type = OP_TEX_MIPMAP, .a = (int)t->gl};
    issue(&o);
    t->levels = 1 + (int)log2f(t->w > t->h ? t->w : t->h);
}
void r_tex_params(const RState *s, int k, const RTexture *t) {
    (void)s;
    if (k >= 0 && k < 2)
        fe.levels[k] = t ? t->levels : 1;
}
void r_set_textures(GLuint a, GLuint b) {
    fe.textures[0] = a;
    fe.textures[1] = b;
}
void r_target_load(const uint8_t *p, int w, int h) {
    if (!p || !gpu.ready)
        return;
    flush_draws();
    Op o = {.type = OP_TARGET_LOAD, .a = w, .b = h};
    o.off = arena_put(p, (size_t)w * h * 4);
    if (o.off == (size_t)-1)
        return;
    if (fe.threaded) {
        Op *q = op_new(OP_TARGET_LOAD);
        if (q)
            *q = o;
    } else {
        gles_ctx_ensure();
        run_op(FB, &o);
        FB->arena_used = 0;
    }
}
void r_submit(void) {
    if (!gpu.ready)
        return;
    /* dx7z's sync(0) (lock/unlock without pixel access) only needs ordering:
     * keep it in the op stream. Handing the batch to the worker here made the
     * frame's present wait for the whole frame (one batch in flight). */
    Op o = {.type = OP_FLUSH};
    issue(&o);
}
int r_snapshot(void) {
    if (!gpu.ready)
        return 0;
    Op o = {.type = OP_SNAPSHOT};
    issue(&o);
    fe.have_front = 1;
    return 1;
}
int r_present_front(int ww, int wh, int stretch, int fps) {
    if (!fe.have_front)
        return 0;
    Op o = {.type = OP_PRESENT, .a = ww, .b = wh, .c = stretch, .d = fps};
    issue(&o);
    submit(0);                     /* the frame is complete: hand it to the worker */
    return 1;
}
static int read_sync(int front, void *p, int w, int h, int packed) {
    if (!gpu.ready || !p)
        return 0;
    int result = 0;
    Op o = {.type = OP_READ, .a = front, .b = w, .c = h, .d = packed, .dst = p, .result = &result};
    issue(&o);
    submit(1);
    return result;
}
int r_read_pixels(uint8_t *p, int w, int h) { return read_sync(0, p, w, h, 0); }
int r_read_pixels_565(uint16_t *p, int w, int h) {
    if (!fe.back_packed)
        return 0;
    return read_sync(0, p, w, h, 1);
}
int r_read_front(uint8_t *p, int w, int h) { return read_sync(1, p, w, h, 0); }
void r_frame_begin(void) {
    gpu.frame_open = 1;
    gpu.frame_start = milliseconds();
}
void r_frame_end(void) {
    flush_draws();
    if (gpu.frame_open) {
        gpu.stats.frames++;
        gpu.stats.frame_ms = milliseconds() - gpu.frame_start;
        gpu.stats.frame_ms_total += gpu.stats.frame_ms;
        gpu.frame_open = 0;
    }
}
void r_get_stats(RStats *s) {
    if (s)
        *s = gpu.stats;
}
RStats r_take_stats(void) {
    RStats result = gpu.stats;
    memset(&gpu.stats, 0, sizeof gpu.stats);
    return result;
}

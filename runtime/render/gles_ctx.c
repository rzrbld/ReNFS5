/*
 * gles_ctx.c - GLES2 context for the D3D7 renderer's offscreen FBO.
 *
 * NFS5 presents through DirectDraw + win32hle screen.c. Both contexts use
 * screen.c's visible SDL/OpenGL window; SDL selects the platform backend.
 * The renderer owns GPU targets and swaps the visible window directly.
 * Only standalone/headless probes create a hidden window or fall back to EGL.
 *
 * Every GL entry point below the context creation calls gles_ctx_ensure(),
 * because screen.c/SDL_Renderer share SDL and may leave another context
 * current on the same thread.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <SDL.h>
#include <pthread.h>
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "gles_ctx.h"

#define GLES_CTX_ES2_MAJOR 2
#define GLES_CTX_ES2_MINOR 0

static int         g_ready;
static int         g_use_sdl;
static SDL_Window *g_win;
static int g_owns_window;
/* The DirectDraw presenter owns the one visible SDL window. Standalone
 * renderer probes have no presenter and may create their own hidden window. */
extern SDL_Window *hle_screen_sdl_window(void) __attribute__((weak));
static SDL_GLContext g_sdl_ctx;

static EGLDisplay  g_egl_dpy = EGL_NO_DISPLAY;
static EGLContext  g_egl_ctx = EGL_NO_CONTEXT;
static EGLSurface  g_egl_surf = EGL_NO_SURFACE;
static EGLConfig   g_egl_cfg;

static char g_info[256] = "no GLES context";
static int g_ctx_major;
static pthread_t g_owner;
static int g_owner_set;

static void set_info(void) {
    const char *ver = (const char *)glGetString(GL_VERSION);
    const char *ren = (const char *)glGetString(GL_RENDERER);
    const char *ven = (const char *)glGetString(GL_VENDOR);
    snprintf(g_info, sizeof g_info, "GL_VERSION=%s | GL_RENDERER=%s | GL_VENDOR=%s",
             ver ? ver : "?", ren ? ren : "?", ven ? ven : "?");
}

static SDL_GLContext create_ctx(SDL_Window *w, int *major) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GLContext c = SDL_GL_CreateContext(w);
    if (c) { *major = 3; return c; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, GLES_CTX_ES2_MAJOR);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, GLES_CTX_ES2_MINOR);
    c = SDL_GL_CreateContext(w);
    if (c) *major = 2;
    return c;
}

static int try_sdl(void) {
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0 && SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[d3d7/render] SDL_Init(VIDEO) failed (%s), trying EGL\n", SDL_GetError());
        return 0;
    }
    SDL_Window *visible = hle_screen_sdl_window ? hle_screen_sdl_window() : NULL;
    if (visible) {
        /* A second context can render our FBO on the existing native surface.
         * Do not create another SDL window: fbdev backends have one display.
         * Ask for ES3 first, retaining ES2 support for older SDL backends. */
        g_sdl_ctx = create_ctx(visible, &g_ctx_major);
        if (!g_sdl_ctx) {
            fprintf(stderr, "[d3d7/render] SDL_GL_CreateContext(presenter): %s\n", SDL_GetError());
            return 0;
        }
        SDL_GL_SetSwapInterval(0); /* frame pacing belongs to the port frame clock */
        g_win = visible;
        g_owns_window = 0;
        g_use_sdl = 1;
        fprintf(stderr, "[d3d7/render] using presenter's visible SDL window\n");
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

    g_win = SDL_CreateWindow("nfs5-d3d7-offscreen",
                             SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                             16, 16, SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL);
    if (!g_win) {
        fprintf(stderr, "[d3d7/render] SDL_CreateWindow(GL,hidden) failed: %s -> dummy window\n",
                SDL_GetError());
        g_win = SDL_CreateWindow("nfs5-d3d7-offscreen",
                                 SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                 16, 16, SDL_WINDOW_HIDDEN);
        if (!g_win) return 0;
    }
    g_owns_window = 1;
    g_sdl_ctx = create_ctx(g_win, &g_ctx_major);
    if (!g_sdl_ctx) {
        fprintf(stderr, "[d3d7/render] SDL_GL_CreateContext(ES) failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_win);
        g_win = NULL;
        return 0;
    }
    SDL_GL_MakeCurrent(g_win, g_sdl_ctx);
    g_use_sdl = 1;
    g_ctx_major = g_ctx_major ? g_ctx_major : 2;
    return 1;
}

/* NFS5_SDL_GLES (handheld build): GLES is resolved through SDL2
 * (gles_loader.c) and nothing links EGL, so the raw-EGL fallback for headless
 * renderer probes is compiled out. */
#ifdef NFS5_SDL_GLES
static int try_egl(void) { return 0; }
#else
static int try_egl(void) {
    static const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_NONE
    };
    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    static const EGLint pb_attr[]  = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };

    g_egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_egl_dpy == EGL_NO_DISPLAY) {
        fprintf(stderr, "[d3d7/render] eglGetDisplay(DEFAULT) failed\n");
        return 0;
    }
    EGLint maj = 0, min = 0;
    if (!eglInitialize(g_egl_dpy, &maj, &min)) {
        fprintf(stderr, "[d3d7/render] eglInitialize failed (0x%x)\n", eglGetError());
        g_egl_dpy = EGL_NO_DISPLAY;
        return 0;
    }
    EGLint ncfg = 0;
    if (!eglChooseConfig(g_egl_dpy, cfg_attr, &g_egl_cfg, 1, &ncfg) || ncfg < 1) {
        fprintf(stderr, "[d3d7/render] eglChooseConfig: no ES2 config\n");
        return 0;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        fprintf(stderr, "[d3d7/render] eglBindAPI(ES) failed\n");
        return 0;
    }
    g_egl_ctx = eglCreateContext(g_egl_dpy, g_egl_cfg, EGL_NO_CONTEXT, ctx_attr);
    if (g_egl_ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "[d3d7/render] eglCreateContext(ES2) failed (0x%x)\n", eglGetError());
        return 0;
    }
    g_egl_surf = eglCreatePbufferSurface(g_egl_dpy, g_egl_cfg, pb_attr);
    if (g_egl_surf == EGL_NO_SURFACE) {
        /* surfaceless if the driver has no pbuffer support at all */
        const char *ext = eglQueryString(g_egl_dpy, EGL_EXTENSIONS);
        if (ext && strstr(ext, "EGL_KHR_surfaceless_context")) {
            if (eglMakeCurrent(g_egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_egl_ctx)) return 1;
        }
        fprintf(stderr, "[d3d7/render] no EGL pbuffer surface\n");
        return 0;
    }
    if (!eglMakeCurrent(g_egl_dpy, g_egl_surf, g_egl_surf, g_egl_ctx)) {
        fprintf(stderr, "[d3d7/render] eglMakeCurrent failed (0x%x)\n", eglGetError());
        return 0;
    }
    return 1;
}
#endif

int gles_ctx_init(void) {
    if (g_ready) return 1;
    if (try_sdl()) {
        g_ready = 1;
        g_owner = pthread_self();
        g_owner_set = 1;
        set_info();
        if (getenv("NFS5_TEXTRACE"))
            fprintf(stderr, "[gles] context created on thread %lu (SDL)\n", (unsigned long)g_owner);
        return 1;
    }
    /* A visible SDL presenter must keep SDL in charge of its display/context.
     * Raw EGL fallback is only for standalone/headless renderer probes. */
    if (hle_screen_sdl_window && hle_screen_sdl_window()) return 0;
    if (try_egl()) {
        g_ready = 1;
        g_owner = pthread_self();
        g_owner_set = 1;
        set_info();
        if (getenv("NFS5_TEXTRACE"))
            fprintf(stderr, "[gles] context created on thread %lu (EGL)\n", (unsigned long)g_owner);
        return 1;
    }
    return 0;
}

void gles_ctx_ensure(void) {
    if (!g_ready) return;
    static int dbg = -1;
    if (dbg < 0) { const char *v = getenv("NFS5_TEXTRACE"); dbg = v && *v && strcmp(v, "0") != 0; }
    if (g_use_sdl) {
        if (SDL_GL_GetCurrentContext() != g_sdl_ctx) {
            int rc = SDL_GL_MakeCurrent(g_win, g_sdl_ctx);
            if (dbg)
                fprintf(stderr, "[gles] ensure thread=%lu owner=%lu makecurrent=%d err=%s\n",
                        (unsigned long)pthread_self(), g_owner_set ? (unsigned long)g_owner : 0UL,
                        rc, SDL_GetError());
        }
#ifndef NFS5_SDL_GLES
    } else if (g_egl_dpy != EGL_NO_DISPLAY) {
        if (eglGetCurrentContext() != g_egl_ctx) {
            int rc = eglMakeCurrent(g_egl_dpy, g_egl_surf, g_egl_surf, g_egl_ctx);
            if (dbg)
                fprintf(stderr, "[gles] ensure thread=%lu owner=%lu eglmake=%d err=0x%x\n",
                        (unsigned long)pthread_self(), g_owner_set ? (unsigned long)g_owner : 0UL,
                        rc, eglGetError());
        }
#endif
    }
}

int gles_ctx_swap(void) {
    if (!g_ready || !g_use_sdl || !g_win || g_owns_window) return 0;
    gles_ctx_ensure();SDL_GL_SwapWindow(g_win);return 1;
}

void gles_ctx_shutdown(void) {
    if (!g_ready) return;
    if (g_use_sdl) {
        if (!SDL_WasInit(SDL_INIT_VIDEO)) {   /* host already shut SDL down */
            g_sdl_ctx = NULL;
            g_win = NULL;
            g_ready = 0;
            g_use_sdl = 0;
            snprintf(g_info, sizeof g_info, "no GLES context");
            return;
        }
        if (g_sdl_ctx) SDL_GL_DeleteContext(g_sdl_ctx);
        if (g_win && g_owns_window) SDL_DestroyWindow(g_win);
        g_sdl_ctx = NULL;
        g_win = NULL;
#ifndef NFS5_SDL_GLES
    } else if (g_egl_dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_egl_surf != EGL_NO_SURFACE) eglDestroySurface(g_egl_dpy, g_egl_surf);
        if (g_egl_ctx != EGL_NO_CONTEXT) eglDestroyContext(g_egl_dpy, g_egl_ctx);
        eglTerminate(g_egl_dpy);
        g_egl_dpy = EGL_NO_DISPLAY;
        g_egl_ctx = EGL_NO_CONTEXT;
        g_egl_surf = EGL_NO_SURFACE;
#endif
    }
    g_ready = 0;
    g_use_sdl = 0;
    snprintf(g_info, sizeof g_info, "no GLES context");
}

int gles_ctx_ready(void) { return g_ready; }

int gles_ctx_major(void) {
    if (g_ctx_major) return g_ctx_major;
    const char *v = (const char *)glGetString(GL_VERSION);
    return (v && v[10] >= '3') ? 3 : 2;
}

void *gles_ctx_proc(const char *name) {
    if (g_use_sdl) return SDL_GL_GetProcAddress(name);
#ifndef NFS5_SDL_GLES
    if (g_egl_dpy != EGL_NO_DISPLAY) return (void *)eglGetProcAddress(name);
#endif
    return NULL;
}

const char *gles_ctx_info(void) { return g_info; }

/* --- GL worker thread support (renderer.c NFS5_GL_THREAD) ---
 * The worker renders into FBOs with no surface bound and binds the window
 * only to present, so SDL_Renderer on the video thread can use the window in
 * between. EGL allows a surface to be current in one thread at a time. */
int gles_ctx_surfaceless(void) {
    if (!g_ready || !g_use_sdl) return 0;
    return SDL_GL_MakeCurrent(NULL, g_sdl_ctx) == 0;
}
int gles_ctx_bind_window(void) {
    if (!g_ready || !g_use_sdl || !g_win) return 0;
    return SDL_GL_MakeCurrent(g_win, g_sdl_ctx) == 0;
}
/* release whatever context this thread has current (SDL_Renderer's on the
 * video thread); SDL re-activates it on the next render call */
void gles_ctx_unbind_current(void) {
    SDL_Window *w = g_win ? g_win : (hle_screen_sdl_window ? hle_screen_sdl_window() : NULL);
    if (w && SDL_GL_GetCurrentContext()) SDL_GL_MakeCurrent(w, NULL);
}
const char *gles_ctx_last_sdl_error(void) { return SDL_GetError(); }

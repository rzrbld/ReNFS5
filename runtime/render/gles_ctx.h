/* gles_ctx.h - SDL/GLES drawing context (see gles_ctx.c). */
#ifndef NFS5_GLES_CTX_H
#define NFS5_GLES_CTX_H

/* 1 on success. Uses the existing visible SDL window. Standalone probes
 * may create a hidden SDL window or EGL pbuffer. Idempotent. */
int  gles_ctx_init(void);
int gles_ctx_swap(void);
void gles_ctx_shutdown(void);
/* Re-bind this context if SDL_GL_CreateContext users (screen.c / SDL_Renderer)
 * left another one current on this thread. Cheap. */
void gles_ctx_ensure(void);
int  gles_ctx_ready(void);
const char *gles_ctx_info(void);
/* Context capabilities and optional entry points beyond ES2. */
int  gles_ctx_major(void);            /* 3 when an ES3 context was created */
void *gles_ctx_proc(const char *name);

int  gles_ctx_surfaceless(void);
int  gles_ctx_bind_window(void);
void gles_ctx_unbind_current(void);
const char *gles_ctx_last_sdl_error(void);
#endif

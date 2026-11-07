/*
 * d3d7.h - the seam between win32hle's DirectDraw7 implementation (owned by
 * the HLE teammate, third_party/pcrecomp/runtime/win32hle/ddraw.c) and this
 * port's Direct3D7-on-GLES implementation (runtime/d3d7.c + runtime/render/).
 *
 * The game's renderer plugin dx7z.dll creates its DirectDraw7 object through
 * DirectDrawCreateEx, then asks it for a IDirect3D7 (QueryInterface) and
 * creates the 3D device from there.  Everything below the IDirectDraw7 object
 * - IDirect3D7, IDirect3DDevice7, IDirect3DTexture2, viewports, materials,
 * lights, vertex buffers - lives in this module; ddraw.c only needs the one
 * QI hook.  All three functions are declared weak by the caller, so a staged
 * build without the renderer still links and dx7z cleanly gets
 * E_NOINTERFACE (and reports "no Direct3D") instead of crashing.
 */
#ifndef NFS5_D3D7_H
#define NFS5_D3D7_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called from IDirectDraw7::QueryInterface (and from IDirectDraw4's, for the
 * fallback path) for any IID this module might serve.  Returns a guest COM
 * object for IID_IDirect3D7 / IID_IDirect3D3, or 0 when the IID is not ours.
 * The returned object is NOT AddRef'd: the caller does
 * `MEM32(obj+4)++; MEM32(out)=obj; return DD_OK;`. */
uint32_t d3d7_ddraw_query_interface(const uint8_t *iid16);

/* Optional: if ddraw's surface QueryInterface wants to forward IID checks,
 * this returns a guest IDirect3DTexture2 for IID_IDirect3DTexture2 on a DDraw
 * surface, else 0.  dx7z never asks for it (no such IID in the DLL), but it
 * makes the texture-handle path API-complete. */
uint32_t d3d7_surface_query_interface(uint32_t surf_obj, const uint8_t *iid16);

/* ddraw's surface contents changed under us (Lock/Unlock/Blt/BltFast on a
 * surface this module may have uploaded as a texture or used as a render
 * target).  Cheap: marks cached upload stale.  Optional for ddraw.c. */
void d3d7_note_surface_dirty(uint32_t surf_obj);

/* Before ddraw reads or copies `surf_obj` (Lock/Blt source/Flip), flush any
 * GLES rendering this module has pending into that surface's guest pixels.
 * Safe to call for any surface object; no-op when nothing is pending.
 * Optional for ddraw.c. */
void d3d7_flush_render_target(uint32_t surf_obj);

/* Host-side controls/diagnostics (used by the probe and by main.c at exit).
 * NFS5_TRACE_D3D7=1 prints every method call; NFS5_STATS=1 prints draw/upload/
 * frame-time counters every NFS5_STATS_FRAMES frames (default 120). */
void d3d7_stats_report(void);      /* counters + unimplemented-method table to stderr */
int  d3d7_active(void);            /* a D3D7 device exists */
void d3d7_shutdown(void);

/* Optional weak hook for the ddraw layer: call after a Flip/Blt presented the
 * primary so NFS5_STATS and NFS5_SHOTS count real presented frames (the front
 * end mostly presents through ddraw, not through D3D7). Polled by
 * d3d7_flush_render_target as well, so it is not required. */
void d3d7_note_present(void);          /* idempotent; from atexit/hle_on_exit */

#ifdef __cplusplus
}
#endif
#endif /* NFS5_D3D7_H */

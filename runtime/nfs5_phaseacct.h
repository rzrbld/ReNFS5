/* nfs5_phaseacct.h - per-frame main-thread time accounting for the NFS5 port.
 *
 * The question this answers: on the thread that runs the game, how much time
 * goes to (a) the lifted guest code itself, (b) the Win32 HLE transition layer
 * (import shims), (c) the Direct3D7/DirectDraw translation, (d) our GLES
 * renderer's submission work, (e) the glReadPixels driver sync, and (f) idling
 * in waits. Everything not inside an accounted region is reported as "guest".
 *
 * Nested regions are handled as exclusive time: a child's elapsed time is
 * subtracted from its parent, so the categories add up to the wall clock.
 * Enabled by NFS5_PHASE_LIVE=1 (a no-op otherwise: one branch per call).
 */
#ifndef NFS5_PHASEACCT_H
#define NFS5_PHASEACCT_H

#include <stdint.h>

enum nfs5_acct_cat {
    NFS5_ACCT_D3D7 = 0,      /* IDirect3D7 and IDirect3DDevice7 methods (transition layer) */
    NFS5_ACCT_RDRAW,         /* our GLES primitive translation/submission            */
    NFS5_ACCT_RUPLOAD,       /* texture upload/mipmap into GLES                     */
    NFS5_ACCT_READBACK,      /* glReadPixels (waits for llvmpipe raster: driver)     */
    NFS5_ACCT_DDRAW,         /* IDirectDraw and IDirectDrawSurface methods            */
    NFS5_ACCT_DINPUT,        /* IDirectInput methods                                */
    NFS5_ACCT_DSOUND,        /* IDirectSound methods                                */
    NFS5_ACCT_WIN32,         /* kernel32/user32/gdi32/winmm/... imports             */
    NFS5_ACCT_WAIT,          /* idle inside WaitFor or Sleep (nested in a shim)      */
    NFS5_ACCT_CONVERT,       /* RGBA -> RGB565 conversion into the guest primary    */
    NFS5_ACCT_PRESENT,       /* GPU composition and SDL swap */
    NFS5_ACCT_COUNT
};

const char *nfs5_acct_name(int cat);
void nfs5_acct_enter(int cat);
void nfs5_acct_leave(int cat);
void nfs5_acct_frame(void);          /* one presented frame, for the fps in the line */
int  nfs5_acct_on(void);             /* 1 when NFS5_PHASE_LIVE is enabled */

#endif

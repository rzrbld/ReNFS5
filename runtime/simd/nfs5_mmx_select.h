/*
 * nfs5_mmx_select.h - opt-in switch that makes lifted MMX code use the AArch64
 * NEON helper implementations instead of the scalar C reference.
 *
 * This file is included by third_party/pcrecomp/runtime/recomp32/recomp_types.h
 * at the very end of its MMX section, and only when the build defines
 * RECOMP_MMX_NEON on an AArch64 target:
 *
 *     /* Optional NEON fast paths for the packed-integer helpers above (aarch64). *​/
 *     #if defined(__aarch64__) && defined(RECOMP_MMX_NEON)
 *     #include "simd/nfs5_mmx_select.h"
 *     #endif
 *
 * Mechanism: the scalar helpers are already defined above this point, so this
 * header re-points the *names used from here on* (i.e. the lifted code) at the
 * NEON functions. The scalar C functions stay present in every translation
 * unit as the reference implementation and as the x86-64/server fallback; the
 * build only needs -DRECOMP_MMX_NEON to opt in. If RECOMP_MMX_NEON is not
 * defined (desktop x86-64 build, differential-test builds) the scalar helpers
 * keep their names and nothing changes.
 *
 * Correctness: every nfs5_neon_* function is bit-exact with the scalar helper
 * of the same name; see runtime/simd/simd_selftest.c and
 * docs/performance/SIMD.md. Do not add a mapping here for an operation the
 * lifter does not emit as a helper call (pand/por/pxor/pandn are emitted as
 * plain C operators by tools/lift/lift32.py MMX_BINOPS).
 */
#ifndef NFS5_MMX_SELECT_H
#define NFS5_MMX_SELECT_H

#if defined(__aarch64__)

#include "nfs5_mmx_neon.h"

/* Binary packed operations emitted by tools/lift/lift32.py (MMX_BINOPS). */
#define mmx_paddb    nfs5_neon_paddb
#define mmx_psubb    nfs5_neon_psubb
#define mmx_paddsb   nfs5_neon_paddsb
#define mmx_psubsb   nfs5_neon_psubsb
#define mmx_paddusb  nfs5_neon_paddusb
#define mmx_psubusb  nfs5_neon_psubusb
#define mmx_paddusw  nfs5_neon_paddusw
#define mmx_psubusw  nfs5_neon_psubusw
#define mmx_paddw    nfs5_neon_paddw
#define mmx_psubw    nfs5_neon_psubw
#define mmx_paddd    nfs5_neon_paddd
#define mmx_psubd    nfs5_neon_psubd
#define mmx_paddsw   nfs5_neon_paddsw
#define mmx_psubsw   nfs5_neon_psubsw
#define mmx_pmulhw   nfs5_neon_pmulhw
#define mmx_pmullw   nfs5_neon_pmullw
#define mmx_pmaddwd  nfs5_neon_pmaddwd
#define mmx_punpcklbw nfs5_neon_punpcklbw
#define mmx_punpckhbw nfs5_neon_punpckhbw
#define mmx_punpcklwd nfs5_neon_punpcklwd
#define mmx_punpckhwd nfs5_neon_punpckhwd
#define mmx_punpckldq nfs5_neon_punpckldq
#define mmx_punpckhdq nfs5_neon_punpckhdq
#define mmx_packssdw nfs5_neon_packssdw
#define mmx_packuswb nfs5_neon_packuswb
#define mmx_packsswb nfs5_neon_packsswb
#define mmx_pcmpeqb  nfs5_neon_pcmpeqb
#define mmx_pcmpgtb  nfs5_neon_pcmpgtb
#define mmx_pcmpeqw  nfs5_neon_pcmpeqw
#define mmx_pcmpgtw  nfs5_neon_pcmpgtw
#define mmx_pcmpeqd  nfs5_neon_pcmpeqd
#define mmx_pcmpgtd  nfs5_neon_pcmpgtd

/* Shifts emitted by tools/lift/lift32.py (MMX_SHIFTS). */
#define mmx_psllq    nfs5_neon_psllq
#define mmx_psrlq    nfs5_neon_psrlq
#define mmx_psllw    nfs5_neon_psllw
#define mmx_psrlw    nfs5_neon_psrlw
#define mmx_psraw    nfs5_neon_psraw
#define mmx_pslld    nfs5_neon_pslld
#define mmx_psrld    nfs5_neon_psrld
#define mmx_psrad    nfs5_neon_psrad

#endif /* __aarch64__ */

#endif /* NFS5_MMX_SELECT_H */

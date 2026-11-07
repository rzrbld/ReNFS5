/*
 * nfs5_mmx_neon.c - benchmark/support code for the AArch64 NEON path.
 *
 * The hot path itself is header-only (runtime/simd/nfs5_mmx_neon.h) so that
 * enabling it needs no change to the game's object list. This translation unit
 * holds the pieces that are only needed by measurement: backend identification
 * and the "game-like" fixed-point 4x4 transform kernels used by
 * runtime/simd/simd_selftest.c to compare scalar vs NEON on a realistic loop
 * (10k vertices, each a 4-word dot product per matrix row - the pmaddwd /
 * paddd pattern a software transform pipeline is built from).
 *
 * Build note: compiled by scripts/simd_selftest.sh only, never by the game
 * build. It needs the *scalar* MMX reference from recomp_types.h, so it must
 * not be compiled with -DRECOMP_MMX_NEON (which would re-point the mmx_* names
 * at NEON and make "scalar" mean NEON).
 */
#include <stdint.h>
#include <stddef.h>

#include "recomp_types.h"       /* scalar reference helpers */
#include "nfs5_mmx_neon.h"

#ifdef mmx_pmaddwd
#error "nfs5_mmx_neon.c is the scalar-vs-NEON benchmark TU; do not compile it with -DRECOMP_MMX_NEON"
#endif

/* ------------------------------------------------------------------ */
/* backend identification                                             */
/* ------------------------------------------------------------------ */
const char *nfs5_simd_backend(void) {
#if defined(__aarch64__)
    return "neon";
#else
    return "scalar";
#endif
}

int nfs5_simd_neon_available(void) {
#if defined(__aarch64__)
    return 1;
#else
    return 0;
#endif
}

/* ------------------------------------------------------------------ */
/* 10k-vertex fixed-point 4x4 transform, "game-like" benchmark        */
/*                                                                    */
/* verts: nverts * 4 int16 (x,y,z,w), matrix: 16 int16 row-major,     */
/* dst:   nverts * 4 int32. Each output = dot(row, vertex), computed  */
/* with MMX pmaddwd (two products summed per dword, 32-bit wrap) and  */
/* a horizontal add, exactly how a software T&L loop of this era uses */
/* the MMX register file.                                             */
/* ------------------------------------------------------------------ */
static inline uint64_t pack4w(const int16_t *p) {
    return (uint64_t)(uint16_t)p[0]
         | ((uint64_t)(uint16_t)p[1] << 16)
         | ((uint64_t)(uint16_t)p[2] << 32)
         | ((uint64_t)(uint16_t)p[3] << 48);
}

static inline int32_t hsum32(uint64_t p) {
    return (int32_t)((uint32_t)p + (uint32_t)(p >> 32));
}

void nfs5_simd_xform_scalar(int32_t *dst, const int16_t *verts,
                            const int16_t *mtx, int nverts) {
    for (int v = 0; v < nverts; v++) {
        uint64_t pv = pack4w(verts + 4 * v);
        for (int r = 0; r < 4; r++)
            dst[4 * v + r] = hsum32(mmx_pmaddwd(pv, pack4w(mtx + 4 * r)));
    }
}

#if defined(__aarch64__)
void nfs5_simd_xform_neon(int32_t *dst, const int16_t *verts,
                          const int16_t *mtx, int nverts) {
    for (int v = 0; v < nverts; v++) {
        uint64_t pv = pack4w(verts + 4 * v);
        for (int r = 0; r < 4; r++)
            dst[4 * v + r] = hsum32(nfs5_neon_pmaddwd(pv, pack4w(mtx + 4 * r)));
    }
}
#endif

/* Deterministic input generator shared by the verifier and the benchmark. */
void nfs5_simd_xform_fill(int16_t *verts, int16_t *mtx, int nverts) {
    for (int v = 0; v < nverts; v++) {
        verts[4 * v + 0] = (int16_t)((v * 37) & 0x7ff);
        verts[4 * v + 1] = (int16_t)((v * 91) & 0x7ff);
        verts[4 * v + 2] = (int16_t)((v * 13) & 0x7ff);
        verts[4 * v + 3] = 4096;                       /* fixed-point 1.0 */
    }
    for (int i = 0; i < 16; i++)
        mtx[i] = (int16_t)(((i * 517) & 0x3ff) - 512); /* ~Q4.12 rotation-ish row */
}

/* Returns 0 when scalar and NEON kernels agree, the number of mismatched
 * dwords otherwise, and -1 when NEON is not available (non-aarch64). */
int nfs5_simd_xform_verify(int nverts) {
#if defined(__aarch64__)
    if (nverts <= 0) return 0;
    static int16_t verts[4 * 65536];
    static int32_t a[4 * 65536], b[4 * 65536];
    int16_t mtx[16];
    if (nverts > 65536) nverts = 65536;
    nfs5_simd_xform_fill(verts, mtx, nverts);
    nfs5_simd_xform_scalar(a, verts, mtx, nverts);
    nfs5_simd_xform_neon(b, verts, mtx, nverts);
    int bad = 0;
    for (int i = 0; i < 4 * nverts; i++) if (a[i] != b[i]) bad++;
    return bad;
#else
    (void)nverts;
    return -1;
#endif
}

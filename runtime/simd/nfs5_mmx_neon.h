/*
 * nfs5_mmx_neon.h - AArch64 NEON implementations of the x86 MMX packed-integer
 * helpers declared in third_party/pcrecomp/runtime/recomp32/recomp_types.h.
 *
 * Why this exists
 * ---------------
 * The static recompiler lifts MMX instructions to calls to scalar C helpers
 * (mmx_paddw, mmx_pmulhw, mmx_pmaddwd, ...) that loop over the eight bytes /
 * four words / two dwords of a 64-bit MMX register. Those helpers are correct
 * but the loop shape is a per-element sequence on the host, whereas an ARM64
 * phone/handheld has a vector unit that does the same packed arithmetic in one
 * instruction. This header provides an exact-semantics NEON implementation for
 * each helper, with the scalar C kept as the reference implementation and the
 * fallback (x86-64 desktop build, and any aarch64 build that does not opt in).
 *
 * Selection
 * ---------
 * Do NOT include this header directly from a translation unit that already
 * includes recomp_types.h expecting scalar semantics (e.g. a differential
 * test).  The game build selects the NEON path through nfs5_mmx_select.h,
 * which recomp_types.h includes at the end of its MMX section when built with
 * -DRECOMP_MMX_NEON and __aarch64__ (see docs/performance/SIMD.md for the
 * vendored 4-line patch and its exact diff).
 *
 * Semantics contract
 * ------------------
 * Every function here is bit-exact with the corresponding scalar helper in
 * recomp_types.h for every possible input pair:
 *   - wrapping adds/subs are wrapping (vadd/vsub, never saturating),
 *   - the saturating forms saturate exactly like x86 (signed/unsigned),
 *   - pmulhw truncates the signed 32-bit product toward zero on the *right*
 *     shift (no rounding), matching `>> 16` on a non-negative-shift int32,
 *   - pmaddwd wraps in 32 bits and does not saturate,
 *   - packssdw/packsswb/packuswb saturate per x86 and place a's elements low,
 *   - shifts follow x86's "count wider than element" rule: logical forms
 *     yield 0, arithmetic forms replicate the sign bit,
 *   - comparisons produce all-ones lanes.
 * That contract is exercised by runtime/simd/simd_selftest.c (randomised
 * differential test, edge-value corpus, saturation and pmaddwd overflow).
 *
 * This file is header-only (all static inline) on purpose: the lifted code
 * calls these helpers from every translated TU, and a header-only design means
 * enabling them needs no change to the object list in the Makefile.
 */
#ifndef NFS5_MMX_NEON_H
#define NFS5_MMX_NEON_H

#include <stdint.h>

#if defined(__aarch64__)

#include <arm_neon.h>

/* ------------------------------------------------------------------ */
/* 8-bit wraparound add/sub                                           */
/* ------------------------------------------------------------------ */
static inline uint64_t nfs5_neon_paddb(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_u8(vadd_u8(vreinterpret_u8_u64(vcreate_u64(a)), vreinterpret_u8_u64(vcreate_u64(b)))), 0);
}
static inline uint64_t nfs5_neon_psubb(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_u8(vsub_u8(vreinterpret_u8_u64(vcreate_u64(a)), vreinterpret_u8_u64(vcreate_u64(b)))), 0);
}

/* ------------------------------------------------------------------ */
/* 16-bit wraparound add/sub                                          */
/* ------------------------------------------------------------------ */
static inline uint64_t nfs5_neon_paddw(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_u16(vadd_u16(vreinterpret_u16_u64(vcreate_u64(a)), vreinterpret_u16_u64(vcreate_u64(b)))), 0);
}
static inline uint64_t nfs5_neon_psubw(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_u16(vsub_u16(vreinterpret_u16_u64(vcreate_u64(a)), vreinterpret_u16_u64(vcreate_u64(b)))), 0);
}

/* ------------------------------------------------------------------ */
/* 32-bit wraparound add/sub                                          */
/* ------------------------------------------------------------------ */
static inline uint64_t nfs5_neon_paddd(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_u32(vadd_u32(vreinterpret_u32_u64(vcreate_u64(a)), vreinterpret_u32_u64(vcreate_u64(b)))), 0);
}
static inline uint64_t nfs5_neon_psubd(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_u32(vsub_u32(vreinterpret_u32_u64(vcreate_u64(a)), vreinterpret_u32_u64(vcreate_u64(b)))), 0);
}

/* ------------------------------------------------------------------ */
/* Saturating add/sub                                                 */
/* ------------------------------------------------------------------ */
static inline uint64_t nfs5_neon_paddsb(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_s8(vqadd_s8(vreinterpret_s8_u64(vcreate_u64(a)), vreinterpret_s8_u64(vcreate_u64(b)))), 0);
}
static inline uint64_t nfs5_neon_psubsb(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_s8(vqsub_s8(vreinterpret_s8_u64(vcreate_u64(a)), vreinterpret_s8_u64(vcreate_u64(b)))), 0);
}
static inline uint64_t nfs5_neon_paddsw(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_s16(vqadd_s16(vreinterpret_s16_u64(vcreate_u64(a)), vreinterpret_s16_u64(vcreate_u64(b)))), 0);
}
static inline uint64_t nfs5_neon_psubsw(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_s16(vqsub_s16(vreinterpret_s16_u64(vcreate_u64(a)), vreinterpret_s16_u64(vcreate_u64(b)))), 0);
}
static inline uint64_t nfs5_neon_paddusb(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_u8(vqadd_u8(vreinterpret_u8_u64(vcreate_u64(a)), vreinterpret_u8_u64(vcreate_u64(b)))), 0);
}
static inline uint64_t nfs5_neon_psubusb(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_u8(vqsub_u8(vreinterpret_u8_u64(vcreate_u64(a)), vreinterpret_u8_u64(vcreate_u64(b)))), 0);
}
static inline uint64_t nfs5_neon_paddusw(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_u16(vqadd_u16(vreinterpret_u16_u64(vcreate_u64(a)), vreinterpret_u16_u64(vcreate_u64(b)))), 0);
}
static inline uint64_t nfs5_neon_psubusw(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_u16(vqsub_u16(vreinterpret_u16_u64(vcreate_u64(a)), vreinterpret_u16_u64(vcreate_u64(b)))), 0);
}

/* ------------------------------------------------------------------ */
/* Multiply                                                           */
/* ------------------------------------------------------------------ */

/* High 16 bits of each signed 16x16 product, truncated (no rounding). */
static inline uint64_t nfs5_neon_pmulhw(uint64_t a, uint64_t b) {
    /* One widening multiply over the 4 lanes: [a0..a3] x [b0..b3]. */
    int16x8_t ab = vcombine_s16(vreinterpret_s16_u64(vcreate_u64(a)), vreinterpret_s16_u64(vcreate_u64(b)));
    int32x4_t p  = vmull_s16(vget_low_s16(ab), vget_high_s16(ab));
    return vget_lane_u64(vreinterpret_u64_s16(vshrn_n_s32(p, 16)), 0);
}

/* Low 16 bits of each signed 16x16 product (wraps, same as x86). */
static inline uint64_t nfs5_neon_pmullw(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_s16(vmul_s16(vreinterpret_s16_u64(vcreate_u64(a)), vreinterpret_s16_u64(vcreate_u64(b)))), 0);
}

/* Each dword = sum of a neighbouring pair of signed 16x16 products, wrapping.
 * Layout: dword0 = a0*b0 + a1*b1, dword1 = a2*b2 + a3*b3. */
static inline uint64_t nfs5_neon_pmaddwd(uint64_t a, uint64_t b) {
    int16x8_t ab = vcombine_s16(vreinterpret_s16_u64(vcreate_u64(a)), vreinterpret_s16_u64(vcreate_u64(b)));
    int32x4_t p  = vmull_s16(vget_low_s16(ab), vget_high_s16(ab));
    int32x4_t s  = vpaddq_s32(p, p);      /* [p0+p1, p2+p3, p0+p1, p2+p3] */
    return vget_lane_u64(vreinterpret_u64_s32(vget_low_s32(s)), 0);
}

/* ------------------------------------------------------------------ */
/* Interleave (unpack): a's element first, low or high half           */
/* ------------------------------------------------------------------ */
static inline uint64_t nfs5_neon_punpcklbw(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_u8(vzip1_u8(vreinterpret_u8_u64(vcreate_u64(a)), vreinterpret_u8_u64(vcreate_u64(b)))), 0);
}
static inline uint64_t nfs5_neon_punpckhbw(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_u8(vzip2_u8(vreinterpret_u8_u64(vcreate_u64(a)), vreinterpret_u8_u64(vcreate_u64(b)))), 0);
}
static inline uint64_t nfs5_neon_punpcklwd(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_u16(vzip1_u16(vreinterpret_u16_u64(vcreate_u64(a)), vreinterpret_u16_u64(vcreate_u64(b)))), 0);
}
static inline uint64_t nfs5_neon_punpckhwd(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_u16(vzip2_u16(vreinterpret_u16_u64(vcreate_u64(a)), vreinterpret_u16_u64(vcreate_u64(b)))), 0);
}
static inline uint64_t nfs5_neon_punpckldq(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_u32(vzip1_u32(vreinterpret_u32_u64(vcreate_u64(a)), vreinterpret_u32_u64(vcreate_u64(b)))), 0);
}
static inline uint64_t nfs5_neon_punpckhdq(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_u32(vzip2_u32(vreinterpret_u32_u64(vcreate_u64(a)), vreinterpret_u32_u64(vcreate_u64(b)))), 0);
}

/* ------------------------------------------------------------------ */
/* Shifts by a runtime count                                          */
/*                                                                    */
/* x86 MMX count rule: a count wider than the element produces zero    */
/* for the logical forms (psll/psrl) and sign-fill for the arithmetic  */
/* form (psra), not a wrapped shift. The scalar reference clamps psra  */
/* counts, which produces the same bits as an over-wide shift.         */
/* ------------------------------------------------------------------ */
static inline uint64_t nfs5_neon_psllq(uint64_t a, uint32_t c) {
    if (c > 63) return 0;
    return vget_lane_u64(vshl_u64(vdup_n_u64(a), vdup_n_s64((int64_t)c)), 0);
}
static inline uint64_t nfs5_neon_psrlq(uint64_t a, uint32_t c) {
    if (c > 63) return 0;
    return vget_lane_u64(vshl_u64(vdup_n_u64(a), vdup_n_s64(-(int64_t)c)), 0);
}
static inline uint64_t nfs5_neon_psllw(uint64_t a, uint32_t c) {
    if (c > 15) return 0;
    return vget_lane_u64(vreinterpret_u64_u16(vshl_u16(vreinterpret_u16_u64(vcreate_u64(a)), vdup_n_s16((int16_t)c))), 0);
}
static inline uint64_t nfs5_neon_psrlw(uint64_t a, uint32_t c) {
    if (c > 15) return 0;
    return vget_lane_u64(vreinterpret_u64_u16(vshl_u16(vreinterpret_u16_u64(vcreate_u64(a)), vdup_n_s16((int16_t)(-(int32_t)c)))), 0);
}
static inline uint64_t nfs5_neon_psraw(uint64_t a, uint32_t c) {
    if (c > 15) c = 15;
    return vget_lane_u64(vreinterpret_u64_s16(vshl_s16(vreinterpret_s16_u64(vcreate_u64(a)), vdup_n_s16((int16_t)(-(int32_t)c)))), 0);
}
static inline uint64_t nfs5_neon_pslld(uint64_t a, uint32_t c) {
    if (c > 31) return 0;
    return vget_lane_u64(vreinterpret_u64_u32(vshl_u32(vreinterpret_u32_u64(vcreate_u64(a)), vdup_n_s32((int32_t)c))), 0);
}
static inline uint64_t nfs5_neon_psrld(uint64_t a, uint32_t c) {
    if (c > 31) return 0;
    return vget_lane_u64(vreinterpret_u64_u32(vshl_u32(vreinterpret_u32_u64(vcreate_u64(a)), vdup_n_s32((int32_t)(-(int32_t)c)))), 0);
}
static inline uint64_t nfs5_neon_psrad(uint64_t a, uint32_t c) {
    if (c > 31) c = 31;
    return vget_lane_u64(vreinterpret_u64_s32(vshl_s32(vreinterpret_s32_u64(vcreate_u64(a)), vdup_n_s32((int32_t)(-(int32_t)c)))), 0);
}

/* ------------------------------------------------------------------ */
/* Pack with saturation (a's elements low)                            */
/* ------------------------------------------------------------------ */
static inline uint64_t nfs5_neon_packssdw(uint64_t a, uint64_t b) {
    int32x4_t ab = vcombine_s32(vreinterpret_s32_u64(vcreate_u64(a)), vreinterpret_s32_u64(vcreate_u64(b)));
    return vget_lane_u64(vreinterpret_u64_s16(vqmovn_s32(ab)), 0);
}
static inline uint64_t nfs5_neon_packsswb(uint64_t a, uint64_t b) {
    int16x8_t ab = vcombine_s16(vreinterpret_s16_u64(vcreate_u64(a)), vreinterpret_s16_u64(vcreate_u64(b)));
    return vget_lane_u64(vreinterpret_u64_s8(vqmovn_s16(ab)), 0);
}
static inline uint64_t nfs5_neon_packuswb(uint64_t a, uint64_t b) {
    int16x8_t ab = vcombine_s16(vreinterpret_s16_u64(vcreate_u64(a)), vreinterpret_s16_u64(vcreate_u64(b)));
    return vget_lane_u64(vreinterpret_u64_u8(vqmovun_s16(ab)), 0);
}

/* ------------------------------------------------------------------ */
/* Compare: all-ones lane on true                                     */
/* ------------------------------------------------------------------ */
static inline uint64_t nfs5_neon_pcmpeqb(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_u8(vceq_u8(vreinterpret_u8_u64(vcreate_u64(a)), vreinterpret_u8_u64(vcreate_u64(b)))), 0);
}
static inline uint64_t nfs5_neon_pcmpgtb(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_u8(vcgt_s8(vreinterpret_s8_u64(vcreate_u64(a)), vreinterpret_s8_u64(vcreate_u64(b)))), 0);
}
static inline uint64_t nfs5_neon_pcmpeqw(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_u16(vceq_u16(vreinterpret_u16_u64(vcreate_u64(a)), vreinterpret_u16_u64(vcreate_u64(b)))), 0);
}
static inline uint64_t nfs5_neon_pcmpgtw(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_u16(vcgt_s16(vreinterpret_s16_u64(vcreate_u64(a)), vreinterpret_s16_u64(vcreate_u64(b)))), 0);
}
static inline uint64_t nfs5_neon_pcmpeqd(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_u32(vceq_u32(vreinterpret_u32_u64(vcreate_u64(a)), vreinterpret_u32_u64(vcreate_u64(b)))), 0);
}
static inline uint64_t nfs5_neon_pcmpgtd(uint64_t a, uint64_t b) {
    return vget_lane_u64(vreinterpret_u64_u32(vcgt_s32(vreinterpret_s32_u64(vcreate_u64(a)), vreinterpret_s32_u64(vcreate_u64(b)))), 0);
}

/* ------------------------------------------------------------------ */
/* Bitwise (the lifter emits these as plain C operators, so they are  */
/* not needed for selection; provided for API parity and the self-test)*/
/* ------------------------------------------------------------------ */
static inline uint64_t nfs5_neon_pand(uint64_t a, uint64_t b) {
    return vget_lane_u64(vand_u64(vdup_n_u64(a), vdup_n_u64(b)), 0);
}
static inline uint64_t nfs5_neon_pandn(uint64_t a, uint64_t b) {   /* ~a & b */
    return vget_lane_u64(vbic_u64(vdup_n_u64(b), vdup_n_u64(a)), 0);
}
static inline uint64_t nfs5_neon_por(uint64_t a, uint64_t b) {
    return vget_lane_u64(vorr_u64(vdup_n_u64(a), vdup_n_u64(b)), 0);
}
static inline uint64_t nfs5_neon_pxor(uint64_t a, uint64_t b) {
    return vget_lane_u64(veor_u64(vdup_n_u64(a), vdup_n_u64(b)), 0);
}

#else /* !__aarch64__ */

/*
 * Non-AArch64 hosts keep the scalar C reference implementation in
 * recomp_types.h; including this header there defines nothing, so it is safe
 * to include unconditionally from portable code.
 */

#endif /* __aarch64__ */

#endif /* NFS5_MMX_NEON_H */

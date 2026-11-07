/* nfs5_xform.c - native replacement for the game's software vertex
 * transform kernel, Porsche.exe 0x005B0730 (hand-written x87 assembly in the
 * original, reached through a function pointer).
 *
 *   void __cdecl xform(uint32 flags, int n, const float *src, float *dst)
 *
 * For each of n vertices: camera-space x/y/z = 3x4 matrix [0x5DEA58] applied
 * to src (flags & 2: w = 1 and the translation row is added; otherwise the
 * fourth source component scales the translation row), the rounded floats go
 * to dst[0..2], rhw = [0x5DFC90] / z (from the unrounded z) to dst[3], and a
 * clip outcode computed by a projection routine ([0x5DFB80 + ((flags>>8)&3)*4],
 * 0x5B05C0 or 0x5B0660) from the float bit patterns is stored through the
 * clip-code cursor [0x5DFC98]. Strides: [0x5DEA64] src, [0x5DEA68] dst,
 * [0x5DEA6C] clip codes.
 *
 * The lift evaluates x87 in host double; the same double operations in the
 * same order are used here, so results are bit-identical (NFS5_XFORM_VERIFY=1
 * compares every byte, register and flag against the translated original on
 * live data). Besides the arithmetic, the lifted loop paid a dispatch-table
 * binary search per vertex for the indirect `call edx`.
 */
#include "recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void recomp_nfs_porsche_005B0730(void);

static inline float fl(uint32_t va) { float v; memcpy(&v, (const void *)ADDR(va), 4); return v; }
static inline uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* outcode routines: integer tricks on the float bits (eax=x, ebx=y, ecx=z) */
static uint32_t code_05C0(uint32_t xb, uint32_t yb, uint32_t zb, uint32_t *ecx_o, uint32_t *edx_o,
                          uint32_t *esi_o, uint32_t *ebp_o) {
    uint32_t eax = xb, ebx = yb, ecx = zb, edx, esi, edi, ebp;
    eax >>= 0x1d; ebx >>= 0x1e; ecx >>= 0x1f;
    esi = MEM32(0x5DFC9C); edi = MEM32(0x5DFCA0); ebp = MEM32(0x5DFCA4);
    eax &= 4; ebx &= 2; esi &= 0x7fffffffu; eax |= ebx; ebp &= 0x7fffffffu; eax |= ecx;
    edi &= 0x7fffffffu;
    ebx = ebp; ebx -= esi; ebp -= edi; ebx >>= 0x1b; ebp &= 0x80000000u; ebp >>= 0x1c;
    ebx &= 0x10; ebx |= ebp;
    ebp = MEM32(0x5DFCA4); ecx = MEM32(0x5DEA74); edx = ebp; esi = MEM32(0x5DEA70); edi = ebp;
    ebx |= eax; edi ^= 0xffffffffu; ecx -= ebp; edx -= esi; ecx &= edi; edx |= ebp;
    edx >>= 0x1f; ecx &= 0x80000000u; ecx >>= 0x1e;
    ebx = MEM32(ebx * 4 + 0x5DFB90);
    ecx |= edx;
    ebx |= ecx;
    *ecx_o = ecx; *edx_o = edx; *esi_o = esi; *ebp_o = ebp;
    return ebx;
}
static uint32_t code_0660(uint32_t xb, uint32_t yb, uint32_t zb, uint32_t *ecx_o, uint32_t *edx_o,
                          uint32_t *esi_o, uint32_t *ebp_o) {
    uint32_t eax = xb, ebx = yb, ecx = zb, edx, esi, edi, ebp;
    eax >>= 0x1d; esi = MEM32(0x5DFC9C); ebx >>= 0x1e; edi = MEM32(0x5DFCA0); ecx >>= 0x1f;
    ebp = MEM32(0x5DFCA4);
    eax &= 4; ebx &= 2; esi &= 0x7fffffffu; eax |= ebx; ebp &= 0x7fffffffu; eax |= ecx;
    edi &= 0x7fffffffu;
    edx = MEM32(0x5DEA60); edx += ebp;
    ebx = ebp; ebx -= esi; ebp -= edi; ebx >>= 0x1b; ebp &= 0x80000000u; ebp >>= 0x1c;
    ebx &= 0x10; ebx |= ebp;
    ebp = edx; edx -= esi; ebp -= edi; edx >>= 0x1b; ebp &= 0x80000000u; ebp >>= 0x1c;
    edx &= 0x10; edx |= ebp;
    ebx |= eax; eax |= edx;
    ebp = MEM32(0x5DFCA4); ecx = MEM32(0x5DEA74); edx = ebp; esi = MEM32(0x5DEA70); edi = ebp;
    ebx = MEM32(ebx * 4 + 0x5DFC10);
    edi ^= 0xffffffffu; ecx -= ebp; edx -= esi; ecx &= edi; edx |= ebp; edx >>= 0x1f;
    ecx &= 0x80000000u; ecx >>= 0x1e;
    eax = MEM32(eax * 4 + 0x5DFC10);
    ebx <<= 8; ecx |= edx; ebx |= eax;
    ebx |= ecx;
    *ecx_o = ecx; *edx_o = edx; *esi_o = esi; *ebp_o = ebp;
    return ebx;
}

/* returns 0 (nothing touched) when the call is outside the supported shape */
static int native_try(void) {
    uint32_t esp0 = g_esp;                       /* -> return address */
    uint32_t flags = MEM32(esp0 + 4);
    int32_t n = (int32_t)MEM32(esp0 + 8);
    uint32_t proj = MEM32(((flags >> 8) & 3u) * 4u + 0x5DFB80);
    uint32_t ss = MEM32(0x5DEA64), ds = MEM32(0x5DEA68), cs = MEM32(0x5DEA6C);
    if (n < 1 || n > 0x100000 || (proj != 0x5B05C0u && proj != 0x5B0660u))
        return 0;
    if ((int32_t)ss < 0 || (int32_t)ds < 16 || (int32_t)cs < 0)
        return 0;
    uint32_t esp1 = esp0 - 16;                   /* after push ebx, esi, edi, ebp */
    MEM32(esp0 - 4) = g_ebx;
    MEM32(esp0 - 8) = g_esi;
    MEM32(esp0 - 12) = g_edi;
    MEM32(esp0 - 16) = g_ebp;
    MEM32(0x5DFC94) = proj;
    MEM32(0x5DFC98) = MEM32(0x69E0D0);
    uint16_t cw = (uint16_t)g_fpu_cw;
    MEM16(0x5DFCA8) = cw;
    MEM16(0x5DFCAA) = (uint16_t)((cw & 0xfcffu) | 0x3fu);
    const int homog = !(flags & 2u);
    uint32_t src = MEM32(esp1 + 0x1C), dst = MEM32(esp1 + 0x20);
    const uint32_t mat = MEM32(0x5DEA58);
    const float *m = (const float *)ADDR(mat);
    const double m0 = m[0], m1 = m[1], m2 = m[2], m4 = m[4], m5 = m[5], m6 = m[6];
    const double m8 = m[8], m9 = m[9], m10 = m[10], m12 = m[12], m13 = m[13], m14 = m[14];
    const double scale = (double)fl(0x5DFC90);
    uint32_t clip = MEM32(0x5DFC98);
    uint32_t ecx = 0, edx = 0, esi = 0, ebp = 0, code = 0, cf = 0;
    for (int32_t i = 0; i < n; i++) {
        const float *v = (const float *)ADDR(src);
        double x0 = v[0], x1 = v[1], x2 = v[2];
        double X = x0 * m0; X += x1 * m4; X += x2 * m8;
        double Y = x0 * m1; Y += x1 * m5; Y += x2 * m9;
        double Z = x0 * m2; Z += x1 * m6; Z += x2 * m10;
        if (homog) {
            double w = v[3];
            X += w * m12; Y += w * m13; Z += w * m14;
        } else {
            X += m12; Y += m13; Z += m14;
        }
        float xf = (float)X, yf = (float)Y, zf = (float)Z;
        uint32_t xb = bits(xf), yb = bits(yf), zb = bits(zf);
        MEM32(0x5DFC9C) = xb;
        MEM32(0x5DFCA0) = yb;
        MEM32(0x5DFCA4) = zb;
        float rhw = (float)(scale / Z);
        MEM32(esp1 - 4) = RECOMP_RETADDR;        /* the call's return slot */
        float *o = (float *)ADDR(dst);
        MEM32(dst) = xb;
        MEM32(dst + 4) = yb;
        MEM32(dst + 8) = zb;
        code = proj == 0x5B05C0u ? code_05C0(xb, yb, zb, &ecx, &edx, &esi, &ebp)
                                 : code_0660(xb, yb, zb, &ecx, &edx, &esi, &ebp);
        o[3] = rhw;
        src += ss;
        dst += ds;
        uint32_t cur = clip;
        cf = (uint32_t)(cs + cur) < cs;          /* add esi, ecx */
        clip = cs + cur;
        MEM32(esp1 + 0x1C) = src;
        MEM32(esp1 + 0x20) = dst;
        MEM32(esp1 + 0x18) = (uint32_t)(n - 1 - i);
        MEM32(0x5DFC98) = clip;
        MEM32(cur) = code;
        ecx = cur;
    }
    (void)edx; (void)esi; (void)ebp;
    g_backedges += (uint32_t)n;
    /* registers: ebx/esi/edi/ebp popped; eax = advanced src, ecx = last clip
     * slot, edx = 0. Flags: dec edx (1 -> 0) after add esi, ecx. */
    g_eax = src;
    g_ecx = ecx;
    g_edx = 0;
    g_flag_k = FK_DEC;
    g_flag_a = 1;
    g_flag_b = 1;
    g_flag_cf = cf;
    g_fpu_cw = cw;
    g_esp = esp0 + 4;
    return 1;
}
static void native_5B0730(void) {
    if (!native_try())
        recomp_nfs_porsche_005B0730();
}

/* ------------------------------------------------------------ verification */
static uint64_t g_calls, g_bad;
typedef struct { uint32_t va, len; } span_t;
static void verify_5B0730(void) {
    uint32_t esp0 = g_esp;
    int32_t n = (int32_t)MEM32(esp0 + 8);
    uint32_t flags = MEM32(esp0 + 4);
    uint32_t proj = MEM32(((flags >> 8) & 3u) * 4u + 0x5DFB80);
    uint32_t ds = MEM32(0x5DEA68), cs = MEM32(0x5DEA6C);
    if (n < 1 || n > 0x10000 || (proj != 0x5B05C0u && proj != 0x5B0660u) || (int32_t)ds < 16 ||
        (int32_t)cs < 0 || (int32_t)MEM32(0x5DEA64) < 0) {
        recomp_nfs_porsche_005B0730();
        return;
    }
    uint32_t dst = MEM32(esp0 + 16), clip = MEM32(0x69E0D0);
    span_t sp[4] = {{esp0 - 24, 24 + 20}, {0x5DFC94, 0x18},
                    {dst, (uint32_t)(n - 1) * ds + 16}, {clip, (uint32_t)(n - 1) * cs + 4}};
    size_t total = 0;
    for (int i = 0; i < 4; i++) total += sp[i].len;
    uint8_t *before = malloc(total), *after = malloc(total);
    if (!before || !after) { free(before); free(after); native_5B0730(); return; }
    size_t o = 0;
    for (int i = 0; i < 4; i++) { memcpy(before + o, (void *)ADDR(sp[i].va), sp[i].len); o += sp[i].len; }
    uint32_t r0[8] = {g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi};
    uint32_t f0[4] = {g_flag_k, g_flag_a, g_flag_b, g_flag_cf};
    uint16_t cw0 = (uint16_t)g_fpu_cw;
    int top0 = g_fp_top;
    uint32_t be = g_backedges;
    recomp_nfs_porsche_005B0730();
    uint32_t r1[8] = {g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi};
    uint32_t f1[4] = {g_flag_k, g_flag_a, g_flag_b, g_flag_cf};
    uint16_t cw1 = (uint16_t)g_fpu_cw;
    int top1 = g_fp_top;
    o = 0;
    for (int i = 0; i < 4; i++) {
        memcpy(after + o, (void *)ADDR(sp[i].va), sp[i].len);
        memcpy((void *)ADDR(sp[i].va), before + o, sp[i].len);
        o += sp[i].len;
    }
    g_eax = r0[0]; g_ecx = r0[1]; g_edx = r0[2]; g_ebx = r0[3];
    g_esp = r0[4]; g_ebp = r0[5]; g_esi = r0[6]; g_edi = r0[7];
    g_flag_k = f0[0]; g_flag_a = f0[1]; g_flag_b = f0[2]; g_flag_cf = f0[3];
    g_fpu_cw = cw0; g_fp_top = top0; g_backedges = be;
    native_5B0730();
    uint32_t r2[8] = {g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi};
    uint32_t f2[4] = {g_flag_k, g_flag_a, g_flag_b, g_flag_cf};
    int bad_mem = 0;
    uint32_t bad_va = 0;
    o = 0;
    for (int i = 0; i < 4 && !bad_mem; i++) {
        const uint8_t *now = (const uint8_t *)ADDR(sp[i].va);
        for (uint32_t k = 0; k < sp[i].len; k++)
            if (after[o + k] != now[k]) { bad_mem = 1; bad_va = sp[i].va + k; break; }
        o += sp[i].len;
    }
    int bad_reg = memcmp(r1, r2, sizeof r1) || cw1 != (uint16_t)g_fpu_cw || top1 != g_fp_top;
    int bad_flags = memcmp(f1, f2, sizeof f1) != 0;
    g_calls++;
    if (bad_mem || bad_reg || bad_flags) {
        g_bad++;
        if (g_bad <= 20)
            fprintf(stderr, "[xform-verify] MISMATCH n=%d flags=%#x mem=%d(va %#x) reg=%d flags=%d "
                    "eax %#x/%#x ecx %#x/%#x edx %#x/%#x f %u,%#x,%#x,%u / %u,%#x,%#x,%u\n",
                    n, flags, bad_mem, bad_va, bad_reg, bad_flags, r1[0], r2[0], r1[1], r2[1], r1[2],
                    r2[2], f1[0], f1[1], f1[2], f1[3], f2[0], f2[1], f2[2], f2[3]);
    }
    if ((g_calls & (g_calls - 1)) == 0 && g_calls >= 64)
        fprintf(stderr, "[xform-verify] %llu calls, %llu mismatches\n", (unsigned long long)g_calls,
                (unsigned long long)g_bad);
    free(before);
    free(after);
}

recomp_func_t nfs5_xform_lookup(uint32_t va) {
    static int mode = -1;
    if (va != 0x005B0730u)
        return NULL;
    if (mode < 0) {
        const char *v = getenv("NFS5_NATIVE_XFORM");
        const char *vv = getenv("NFS5_XFORM_VERIFY");
        mode = (v && !strcmp(v, "0")) ? 0 : (vv && *vv && strcmp(vv, "0") != 0) ? 2 : 1;
        fprintf(stderr, "[xform] native transform kernel mode=%d\n", mode);
    }
    return mode == 2 ? verify_5B0730 : mode == 1 ? native_5B0730 : NULL;
}

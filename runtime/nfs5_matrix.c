/* nfs5_matrix.c - native replacements for two hot Porsche.exe matrix helpers.
 *
 * 0x539F50  __thiscall batch transform (5 args, ret 0x14):
 *     this(ecx) = float m[16];  args: count, src, src_stride, dst, dst_stride
 *     (a zero stride means 16). Each source vec4 (x,y,z,w) is multiplied by
 *     the 4x4 matrix; dst == src takes an in-place path that reads the whole
 *     vertex first, otherwise each output is stored as soon as it is
 *     computed. The two paths sum the four products in different orders.
 *
 * 0x53A6C0  __cdecl float det(const float *m, int n): determinant by cofactor
 *     expansion along row 0 (row stride 4 floats), recursing on minors built
 *     on its own stack; each step is sum = (float)(det(minor) * m[j] * sign +
 *     sum) and sign flips. A 4x4 costs ~40 lifted calls.
 *
 * The lift evaluates x87 in host double (product of two floats exact,
 * additions rounded to double, fstp rounds to float); the same operations in
 * the same order are used here and the project builds with
 * -ffp-contract=off, so results are bit-identical. Memory reads follow the
 * original's order so overlapping buffers behave the same. Registers the
 * originals leave behind (eax/ecx/edx), the argument slots they modify and
 * the x87 result are reproduced; their stack frames below esp are dead after
 * return and are not.
 *
 * tools/patch_local_regs.py renames the lifted bodies to *_lifted (WRAPPED);
 * these wrappers keep the original names. NFS5_NATIVE_MATRIX=0 runs the
 * lifted code; NFS5_MATRIX_VERIFY=1 runs both on live data and compares
 * memory, registers and the x87 result.
 */
#include "recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void recomp_nfs_porsche_00539F50_lifted(void);
void recomp_nfs_porsche_0053A6C0_lifted(void);

#define F(va) ((double)*(const float *)ADDR(va))
#define STF(va, v) (*(float *)ADDR(va) = (float)(v))

static int g_mode = -1;   /* 0 lifted, 1 native, 2 verify */
static int mode(void) {
    if (g_mode < 0) {
        const char *v = getenv("NFS5_NATIVE_MATRIX"), *vv = getenv("NFS5_MATRIX_VERIFY");
        g_mode = (v && !strcmp(v, "0")) ? 0 : (vv && *vv && strcmp(vv, "0") != 0) ? 2 : 1;
        fprintf(stderr, "[matrix] native matrix kernels mode=%d\n", g_mode);
    }
    return g_mode;
}

/* ------------------------------------------------------------ 0x539F50 */
static int native_539F50(void) {
    const uint32_t esp0 = g_esp, m = g_ecx;
    uint32_t n = MEM32(esp0 + 4), src = MEM32(esp0 + 8), ss = MEM32(esp0 + 12);
    uint32_t dst = MEM32(esp0 + 16), ds = MEM32(esp0 + 20);
    if (n > 0x100000u) return 0;
    if (!ss) ss = 16;
    if (!ds) ds = 16;
    if (n == 0) { g_esp = esp0 + 4 + 0x14; return 1; }
    if (src == dst) {
        uint32_t s = src, d = dst;
        for (uint32_t i = 0; i < n; i++, s += ss, d += ds) {
            /* all reads of the vertex happen before its first store */
            double o0 = F(s + 12) * F(m + 0x30); o0 = F(s) * F(m) + o0;
            o0 += F(s + 4) * F(m + 0x10);        o0 += F(s + 8) * F(m + 0x20);
            double o1 = F(s + 12) * F(m + 0x34); o1 += F(m + 0x24) * F(s + 8);
            o1 += F(m + 4) * F(s);               o1 += F(m + 0x14) * F(s + 4);
            double o2 = F(s + 4) * F(m + 0x18);  o2 += F(s + 12) * F(m + 0x38);
            o2 += F(s + 8) * F(m + 0x28);        o2 += F(m + 8) * F(s);
            double o3 = F(m + 0x2c) * F(s + 8);  o3 += F(m + 0xc) * F(s);
            o3 += F(m + 0x3c) * F(s + 12);       o3 += F(s + 4) * F(m + 0x1c);
            float f0 = (float)o0, f1 = (float)o1, f2 = (float)o2, f3 = (float)o3;
            *(float *)ADDR(d) = f0;
            *(float *)ADDR(d + 4) = f1;
            *(float *)ADDR(d + 8) = f2;
            *(float *)ADDR(d + 12) = f3;
        }
        MEM32(esp0 + 4) = 0;                       /* the count lives in its arg slot */
        g_eax = src + 8 + n * ss;
        g_edx = dst + n * ds;
    } else {
        uint32_t s = src, d = dst;
        for (uint32_t i = 0; i < n; i++, s += ss, d += ds) {
            double o = F(s) * F(m);              o += F(s + 4) * F(m + 0x10);
            o += F(s + 8) * F(m + 0x20);         o += F(s + 12) * F(m + 0x30);
            STF(d, o);
            o = F(s + 8) * F(m + 0x24);          o = F(m + 4) * F(s) + o;
            o += F(s + 4) * F(m + 0x14);         o += F(s + 12) * F(m + 0x34);
            STF(d + 4, o);
            o = F(s + 12) * F(m + 0x38);         o = F(m + 8) * F(s) + o;
            o += F(s + 8) * F(m + 0x28);         o += F(s + 4) * F(m + 0x18);
            STF(d + 8, o);
            o = F(m + 0x3c) * F(s + 12);         o += F(s + 4) * F(m + 0x1c);
            o += F(m + 0x2c) * F(s + 8);         o += F(s) * F(m + 0xc);
            STF(d + 12, o);
        }
        g_eax = src + 8 + n * ss;
        g_edx = dst + 8 + n * ds;
    }
    g_backedges += n;
    g_esp = esp0 + 4 + 0x14;
    return 1;
}

/* ------------------------------------------------------------ 0x53A6C0 */
static float det_rec(const uint32_t *m, int n) {
    if (n == 1) { float v; memcpy(&v, m, 4); return v; }
    if (n <= 0) return 0.0f;
    uint32_t minor[16];
    float sum = 0.0f, sign = 1.0f;
    for (int j = 0; j < n; j++) {
        for (int r = 1; r < n; r++) {
            int k = 0;
            for (int c = 0; c < n; c++)
                if (c != j) minor[(r - 1) * 4 + k++] = m[r * 4 + c];
        }
        float d = det_rec(minor, n - 1), mj;
        memcpy(&mj, &m[j], 4);
        double t = (double)d * (double)mj;
        t *= (double)sign;
        t += (double)sum;
        sum = (float)t;
        sign = -sign;
    }
    return sum;
}

static int native_53A6C0(void) {
    const uint32_t esp0 = g_esp, mp = MEM32(esp0 + 4);
    const int32_t n = (int32_t)MEM32(esp0 + 8);
    if (n > 5) return 0;                           /* minors would outgrow the 16-float buffer */
    float r;
    if (n == 1) {
        r = *(const float *)ADDR(mp);
        g_eax = mp;
    } else if (n <= 0) {
        r = 0.0f;
    } else {
        uint32_t m[5 * 4 + 1];
        memcpy(m, (const void *)ADDR(mp), (size_t)((n - 1) * 4 + n) * 4);
        r = det_rec(m, n);
        MEM32(esp0 + 4) = mp + 4u * (uint32_t)n;   /* advanced row-0 pointer in its arg slot */
        g_eax = mp + 4u * (uint32_t)n;
        g_ecx = 1;
        g_edx = esp0 - 0x68u * (uint32_t)(n - 2) - 0x40u;   /* innermost det(2)'s minor buffer */
    }
    fp_push((double)r);
    g_esp = esp0 + 4;
    return 1;
}

/* ------------------------------------------------------------ verify */
typedef struct { uint32_t va, len; } span_t;
static uint64_t v_calls[2], v_bad[2];

static void verify(int which, void (*lifted)(void), int (*native)(void), span_t *sp, int nsp, int fp_result) {
    size_t total = 0;
    for (int i = 0; i < nsp; i++) total += sp[i].len;
    uint8_t *before = malloc(total), *after = malloc(total);
    if (!before || !after) { free(before); free(after); if (!native()) lifted(); return; }
    size_t o = 0;
    for (int i = 0; i < nsp; i++) { memcpy(before + o, (void *)ADDR(sp[i].va), sp[i].len); o += sp[i].len; }
    uint32_t r0[8] = {g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi};
    int top0 = g_fp_top;
    uint32_t be = g_backedges;
    lifted();
    uint32_t r1[8] = {g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi};
    int top1 = g_fp_top;
    double res1 = fp_result ? fp_pop() : 0.0;
    o = 0;
    for (int i = 0; i < nsp; i++) {
        memcpy(after + o, (void *)ADDR(sp[i].va), sp[i].len);
        memcpy((void *)ADDR(sp[i].va), before + o, sp[i].len);
        o += sp[i].len;
    }
    g_eax = r0[0]; g_ecx = r0[1]; g_edx = r0[2]; g_ebx = r0[3];
    g_esp = r0[4]; g_ebp = r0[5]; g_esi = r0[6]; g_edi = r0[7];
    g_fp_top = top0; g_backedges = be;
    int used_native = native();
    if (!used_native) {                            /* outside the native kernel's range: keep lifted results */
        g_eax = r0[0]; g_ecx = r0[1]; g_edx = r0[2]; g_ebx = r0[3];
        g_esp = r0[4]; g_ebp = r0[5]; g_esi = r0[6]; g_edi = r0[7];
        g_fp_top = top0;
        lifted();
        free(before); free(after);
        return;
    }
    uint32_t r2[8] = {g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi};
    int top2 = g_fp_top;
    double res2 = 0.0;
    if (fp_result) { res2 = fp_pop(); fp_push(res2); }
    int bad_mem = 0;
    uint32_t bad_va = 0;
    o = 0;
    for (int i = 0; i < nsp && !bad_mem; i++) {
        const uint8_t *now = (const uint8_t *)ADDR(sp[i].va);
        for (uint32_t k = 0; k < sp[i].len; k++)
            if (after[o + k] != now[k]) { bad_mem = 1; bad_va = sp[i].va + k; break; }
        o += sp[i].len;
    }
    int bad_reg = memcmp(r1, r2, sizeof r1) != 0 || top1 != top2;
    int bad_fp = fp_result && memcmp(&res1, &res2, sizeof res1) != 0;
    v_calls[which]++;
    if (bad_mem || bad_reg || bad_fp) {
        v_bad[which]++;
        if (v_bad[which] <= 20)
            fprintf(stderr, "[matrix-verify] %s MISMATCH mem=%d(va %#x) reg=%d fp=%d (%.9g/%.9g) "
                    "eax %#x/%#x ecx %#x/%#x edx %#x/%#x esp %#x/%#x top %d/%d\n",
                    which ? "det 53A6C0" : "xform 539F50", bad_mem, bad_va, bad_reg, bad_fp, res1, res2,
                    r1[0], r2[0], r1[1], r2[1], r1[2], r2[2], r1[4], r2[4], top1, top2);
    }
    if ((v_calls[which] & (v_calls[which] - 1)) == 0 && v_calls[which] >= 64)
        fprintf(stderr, "[matrix-verify] %s %llu calls, %llu mismatches\n", which ? "det" : "xform",
                (unsigned long long)v_calls[which], (unsigned long long)v_bad[which]);
    free(before);
    free(after);
}

static void verify_539F50(void) {
    uint32_t esp0 = g_esp, n = MEM32(esp0 + 4), ds = MEM32(esp0 + 20), dst = MEM32(esp0 + 16);
    if (!ds) ds = 16;
    if (n > 0x10000u) { recomp_nfs_porsche_00539F50_lifted(); return; }
    span_t sp[2] = {{esp0 + 4, 20}, {dst, n ? (n - 1) * ds + 16 : 0}};
    verify(0, recomp_nfs_porsche_00539F50_lifted, native_539F50, sp, 2, 0);
}

static void verify_53A6C0(void) {
    span_t sp[1] = {{g_esp + 4, 8}};
    verify(1, recomp_nfs_porsche_0053A6C0_lifted, native_53A6C0, sp, 1, 1);
}

void recomp_nfs_porsche_00539F50(void) {
    switch (mode()) {
    case 1: if (native_539F50()) return; break;
    case 2: verify_539F50(); return;
    default: break;
    }
    recomp_nfs_porsche_00539F50_lifted();
}

void recomp_nfs_porsche_0053A6C0(void) {
    switch (mode()) {
    case 1: if (native_53A6C0()) return; break;
    case 2: verify_53A6C0(); return;
    default: break;
    }
    recomp_nfs_porsche_0053A6C0_lifted();
}

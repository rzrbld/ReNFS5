/* nfs5_audio_kernels.c - native replacements for the hot leaf kernels of the
 * game's software audio mixer (Porsche.exe SND engine, 0x59xxxx module).
 *
 * On the H700 the mixer runs as lifted x87 code on its own guest thread, and
 * every guest thread shares one machine lock, so the frame thread waits while
 * the mixer decodes and mixes (measured: mixer thread ~22% of a core, frame
 * thread blocked ~25-30% of race wall time). These functions are reached only
 * through function pointers (voice/node vtables), so they are swapped in at
 * the indirect-call lookup (nfs_recovered_lookup -> nfs5_audio_lookup).
 *
 * Exactness: the lift evaluates x87 arithmetic in host double and rounds to
 * float on fstp, with -ffp-contract=off. The code below performs the same
 * double operations in the same order, so results are bit-identical to the
 * translated originals (NFS5_AUDIO_VERIFY=1 checks that on live game data).
 * Guest-visible side effects are kept: stack scratch words written by the
 * prologue pushes, final register values, the x87 top (balanced) and the
 * flag snapshot of the last flag-writing instruction before `ret`.
 *
 *   0x0059A5A0  EA-XA ADPCM block decoder: 15-byte blocks -> 28 float samples
 *   0x0059BA70  y[i] += a * x[i]   (voice gain into a float mix bus)
 */
#include "recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* plain (non-volatile) host views of guest memory for the hot loops */
static inline float *gf(uint32_t va) { return (float *)ADDR(va); }
static inline const uint8_t *gb(uint32_t va) { return (const uint8_t *)ADDR(va); }

/* ---------------------------------------------------------------- 0x59A5A0
 * void __cdecl decode(state *s)   s: +0 remaining (int, -=28 per block)
 *   +4 hist1 (float bits), +8 hist2, +0xC src bytes, +0x10 dst float*.
 * The decoder writes hist1/hist2 to dst[-1]/dst[-2], decodes whole blocks
 * while remaining > 0, and stores the last two samples and dst back. */
static void native_59A5A0(void) {
    uint32_t esp0 = g_esp;                        /* -> return address */
    uint32_t ebp_frame = esp0 - 4;                /* push ebp; mov ebp, esp */
    MEM32(esp0 - 4) = g_ebp;
    MEM32(esp0 - 8) = g_eax;
    MEM32(esp0 - 12) = g_ebx;
    MEM32(esp0 - 16) = g_ecx;
    MEM32(esp0 - 20) = g_edx;
    MEM32(esp0 - 24) = g_esi;
    MEM32(esp0 - 28) = g_edi;
    uint32_t s = MEM32(ebp_frame + 8);
    uint32_t dst = MEM32(s + 0x10);
    MEM32(dst - 4) = MEM32(s + 4);
    MEM32(dst - 8) = MEM32(s + 8);
    uint32_t src = MEM32(s + 0xC);
    const float *ca_tab = gf(0x5E3B38), *cb_tab = gf(0x5E3B28), *nib_tab = gf(0x6B04C0);
    int32_t remaining = (int32_t)MEM32(s);
    uint32_t blocks = 0;
    while (remaining > 0) {
        remaining -= 0x1C;
        const uint8_t *in = gb(src);
        unsigned hdr = in[0];
        double ca = (double)ca_tab[hdr >> 4], cb = (double)cb_tab[hdr >> 4];
        const float *t = nib_tab + (hdr & 0xF) * 16;
        float *o = gf(dst);                       /* o[-1], o[-2]: history */
        for (int j = 0; j < 14; j++) {
            unsigned b = in[1 + j];
            double v = (double)o[-1] * cb;
            v += (double)t[b >> 4];
            v += (double)o[-2] * ca;
            o[0] = (float)v;
            v = (double)o[0] * cb;
            v += (double)t[b & 0xF];
            v += (double)o[-1] * ca;
            o[1] = (float)v;
            o += 2;
        }
        src += 0xF;
        dst += 0x70;
        blocks++;
    }
    MEM32(s) = (uint32_t)remaining;
    /* at exit ecx == 0 (or the initial 0 if no block ran), ebx == dst */
    MEM32(s + 4) = MEM32(dst - 4);
    MEM32(s + 8) = MEM32(dst - 8);
    MEM32(s + 0x10) = dst;
    g_backedges += blocks * 15u;
    /* registers are all restored by the epilogue; flags: cmp [esi], 0 */
    g_flag_a = (uint32_t)remaining;
    g_flag_b = 0;
    g_flag_cf = 0;
    g_flag_k = FK_CMP;
    g_esp = esp0 + 4;                             /* ret */
}

/* ---------------------------------------------------------------- 0x59BA70
 * void __cdecl mix(int n, float a, const float *x, float *y): y[i] += a*x[i]
 * Index walks down from n; n % 4 leftovers first, then groups of four while
 * the pre-decrement count is > 4 (signed). n == 0 (and n < 0) still runs one
 * group at indices -4..-1, exactly like the original. */
static inline void mix1(const float *x, float *y, int32_t i, double a) {
    double v = (double)x[i] * a;
    v += (double)y[i];
    y[i] = (float)v;
}
static void native_59BA70(void) {
    uint32_t esp0 = g_esp;
    uint32_t ebp_frame = esp0 - 4;
    MEM32(esp0 - 4) = g_ebp;
    MEM32(esp0 - 8) = g_ecx;
    MEM32(esp0 - 12) = g_esi;
    MEM32(esp0 - 16) = g_edi;
    int32_t n = (int32_t)MEM32(ebp_frame + 8);
    float af;
    memcpy(&af, (const void *)ADDR(ebp_frame + 0xC), 4);
    const double a = (double)af;
    const float *x = gf(MEM32(ebp_frame + 0x10));
    float *y = gf(MEM32(ebp_frame + 0x14));
    uint32_t fa, fb, fcf;
    int32_t c = n;
    uint32_t iters = 0;
    for (;;) {
        if ((uint32_t)c & 3u) {
            mix1(x, y, c - 1, a);
            fa = (uint32_t)c; fb = 1; fcf = (uint32_t)c < 1u;
            c -= 1;
            iters++;
            if (fa == 1u) break;                  /* je: result zero */
            continue;
        }
        /* groups of four; the original stores -0x10, -0xC, -4, -8 (same set) */
        do {
            mix1(x, y, c - 4, a);
            mix1(x, y, c - 3, a);
            mix1(x, y, c - 1, a);
            mix1(x, y, c - 2, a);
            fa = (uint32_t)c; fb = 4; fcf = (uint32_t)c < 4u;
            c -= 4;
            iters++;
        } while ((int32_t)fa > 4);
        break;
    }
    g_backedges += iters;
    g_flag_a = fa;
    g_flag_b = fb;
    g_flag_cf = fcf;
    g_flag_k = FK_CMP;
    g_esp = esp0 + 4;                             /* ecx/esi/edi/ebp restored */
}

/* ------------------------------------------------------------- verification
 * NFS5_AUDIO_VERIFY=1: run the translated original on the live data, keep its
 * results, restore the inputs, run the native kernel and compare every byte
 * either one may write, plus registers, esp, x87 top and flags. */
extern void recomp_nfs_porsche_0059A5A0(void);
extern void recomp_nfs_porsche_0059BA70(void);

typedef struct { uint32_t va, len; } span_t;
typedef struct {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t fk, fa, fb, fcf;
    int top;
} cpu_t;
static void cpu_get(cpu_t *c) {
    c->eax = g_eax; c->ecx = g_ecx; c->edx = g_edx; c->ebx = g_ebx;
    c->esp = g_esp; c->ebp = g_ebp; c->esi = g_esi; c->edi = g_edi;
    c->fk = g_flag_k; c->fa = g_flag_a; c->fb = g_flag_b; c->fcf = g_flag_cf;
    c->top = g_fp_top;
}
static void cpu_set(const cpu_t *c) {
    g_eax = c->eax; g_ecx = c->ecx; g_edx = c->edx; g_ebx = c->ebx;
    g_esp = c->esp; g_ebp = c->ebp; g_esi = c->esi; g_edi = c->edi;
    g_flag_k = c->fk; g_flag_a = c->fa; g_flag_b = c->fb; g_flag_cf = c->fcf;
    g_fp_top = c->top;
}
static uint64_t g_verify_calls[2], g_verify_bad[2];
static void verify(int which, void (*orig)(void), void (*native)(void), const span_t *sp, int nsp) {
    size_t total = 0;
    for (int i = 0; i < nsp; i++) total += sp[i].len;
    uint8_t *before = malloc(total), *after = malloc(total);
    if (!before || !after) { free(before); free(after); native(); return; }
    size_t o = 0;
    for (int i = 0; i < nsp; i++) { memcpy(before + o, (void *)ADDR(sp[i].va), sp[i].len); o += sp[i].len; }
    cpu_t in, out_orig, out_nat;
    cpu_get(&in);
    uint32_t be = g_backedges;
    orig();
    cpu_get(&out_orig);
    o = 0;
    for (int i = 0; i < nsp; i++) {
        memcpy(after + o, (void *)ADDR(sp[i].va), sp[i].len);
        memcpy((void *)ADDR(sp[i].va), before + o, sp[i].len);
        o += sp[i].len;
    }
    cpu_set(&in);
    g_backedges = be;
    native();
    cpu_get(&out_nat);
    int bad_mem = 0;
    o = 0;
    uint32_t bad_va = 0;
    for (int i = 0; i < nsp && !bad_mem; i++) {
        if (memcmp(after + o, (void *)ADDR(sp[i].va), sp[i].len)) {
            for (uint32_t k = 0; k < sp[i].len; k++)
                if (after[o + k] != ((uint8_t *)ADDR(sp[i].va))[k]) { bad_va = sp[i].va + k; break; }
            bad_mem = 1;
        }
        o += sp[i].len;
    }
    /* flag snapshots may legitimately differ in representation; compare the
     * registers, esp and x87 top exactly and the flags as a separate class */
    int bad_reg = out_orig.eax != out_nat.eax || out_orig.ecx != out_nat.ecx ||
                  out_orig.edx != out_nat.edx || out_orig.ebx != out_nat.ebx ||
                  out_orig.esp != out_nat.esp || out_orig.ebp != out_nat.ebp ||
                  out_orig.esi != out_nat.esi || out_orig.edi != out_nat.edi ||
                  out_orig.top != out_nat.top;
    int bad_flags = out_orig.fk != out_nat.fk || out_orig.fa != out_nat.fa ||
                    out_orig.fb != out_nat.fb || out_orig.fcf != out_nat.fcf;
    g_verify_calls[which]++;
    if (bad_mem || bad_reg || bad_flags) {
        g_verify_bad[which]++;
        if (g_verify_bad[which] <= 20)
            fprintf(stderr, "[audio-verify] %s MISMATCH mem=%d(va %#x) reg=%d flags=%d "
                    "(fk %u/%u fa %#x/%#x fb %#x/%#x cf %u/%u)\n",
                    which ? "mix 59BA70" : "decode 59A5A0", bad_mem, bad_va, bad_reg, bad_flags,
                    out_orig.fk, out_nat.fk, out_orig.fa, out_nat.fa, out_orig.fb, out_nat.fb,
                    out_orig.fcf, out_nat.fcf);
    }
    uint64_t n = g_verify_calls[0] + g_verify_calls[1];
    if ((n & (n - 1)) == 0 && n >= 64)
        fprintf(stderr, "[audio-verify] decode %llu calls %llu bad, mix %llu calls %llu bad\n",
                (unsigned long long)g_verify_calls[0], (unsigned long long)g_verify_bad[0],
                (unsigned long long)g_verify_calls[1], (unsigned long long)g_verify_bad[1]);
    free(before);
    free(after);
}
static void verify_59A5A0(void) {
    uint32_t s = MEM32(g_esp + 4);
    int32_t rem = (int32_t)MEM32(s);
    uint32_t blocks = rem > 0 ? ((uint32_t)rem + 27u) / 28u : 0;
    uint32_t dst = MEM32(s + 0x10);
    span_t sp[3] = { { g_esp - 28, 28 }, { s, 0x14 }, { dst - 8, 8 + blocks * 0x70 } };
    verify(0, recomp_nfs_porsche_0059A5A0, native_59A5A0, sp, 3);
}
static void verify_59BA70(void) {
    int32_t n = (int32_t)MEM32(g_esp + 4);
    uint32_t y = MEM32(g_esp + 16);
    /* n > 0 touches y[0, n); n == 0 touches y[-4, 0); n < 0 is not verified */
    if (n < 0 || n > 0x100000) { native_59BA70(); return; }
    int32_t lo = n ? 0 : -4, hi = n;
    span_t sp[2] = { { g_esp - 16, 16 }, { y + (uint32_t)lo * 4u, (uint32_t)(hi - lo) * 4u } };
    verify(1, recomp_nfs_porsche_0059BA70, native_59BA70, sp, 2);
}

/* ------------------------------------------------------------------ lookup */
static int g_mode = -1;   /* bit0 decode, bit1 mix; NFS5_NATIVE_AUDIO_MASK */
static int g_verify = -1;
recomp_func_t nfs5_audio_kernel_lookup(uint32_t va) {
    if (g_mode < 0) {
        const char *v = getenv("NFS5_NATIVE_AUDIO");
        const char *m = getenv("NFS5_NATIVE_AUDIO_MASK");
        g_mode = (v && !strcmp(v, "0")) ? 0 : (m && *m ? (int)strtol(m, NULL, 0) : 3);
        const char *vv = getenv("NFS5_AUDIO_VERIFY");
        g_verify = vv && *vv && strcmp(vv, "0") != 0;
        fprintf(stderr, "[audio] native kernels mask=%d verify=%d\n", g_mode, g_verify);
    }
    if (va == 0x0059A5A0u && (g_mode & 1)) return g_verify ? verify_59A5A0 : native_59A5A0;
    if (va == 0x0059BA70u && (g_mode & 2)) return g_verify ? verify_59BA70 : native_59BA70;
    return NULL;
}

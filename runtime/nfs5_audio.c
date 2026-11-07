#include "recomp_types.h"
#include <stdlib.h>
#include <string.h>
/* Cubic audio resampling with the guest's double-precision operation order.
 * Preserve stack scratch writes, CPU flags, phase/index updates and x87 slot
 * clearing. Bounded blocks that cannot cross a guest scheduling handoff use
 * this path; larger/unusual calls retain the translated implementation. */
static float rf(uint32_t address) {
    float v;
    memcpy(&v, (const void *)ADDR(address), 4);
    return v;
}
static void wf(uint32_t address, float v) { memcpy((void *)ADDR(address), &v, 4); }
static int cubic_resample_try(void) {
    uint32_t count = MEM32(g_esp + 4);
    if (!count || count > 4096 || g_backedges >= RECOMP_YIELD_EVERY ||
        count >= RECOMP_YIELD_EVERY - g_backedges)
        return 0;
    uint32_t out_start = MEM32(g_esp + 12), index_start = MEM32(g_esp + 16),
             phase_start = MEM32(g_esp + 20);
    uint64_t stack_lo = (uint64_t)g_esp - 24, stack_hi = (uint64_t)g_esp + 32;
    if ((uint64_t)out_start + count * 4 > UINT32_MAX ||
        ((uint64_t)out_start < stack_hi && (uint64_t)out_start + count * 4 > stack_lo) ||
        ((uint64_t)index_start < stack_hi && (uint64_t)index_start + 4 > stack_lo) ||
        ((uint64_t)phase_start < stack_hi && (uint64_t)phase_start + 4 > stack_lo))
        return 0;
    uint32_t _flag_k, _flag_a, _flag_b, _cf;
    RECOMP_ENTER(0x0059CC60u);
    g_esp -= 8;
    PUSH32(g_esp, g_ebp);
    PUSH32(g_esp, g_esi);
    uint32_t phasep = MEM32(g_esp + 0x24);
    uint32_t fracstep = MEM32(g_esp + 0x2c) >> 16;
    uint32_t phase = MEM32(phasep) >> 16;
    MEM32(phasep) = phase;
    uint32_t step = (MEM32(g_esp + 0x28) << 16) | fracstep;
    MEM32(g_esp + 0x28) = step;
    g_ecx = MEM32(g_esp + 0x18);
    PUSH32(g_esp, g_ebx);
    uint32_t indexp = MEM32(g_esp + 0x24);
    PUSH32(g_esp, g_edi);
    uint32_t out = MEM32(g_esp + 0x24);
    MEM32(g_esp + 0x2c) = count;
    do {
        phase = MEM32(phasep);
        MEM32(g_esp + 0x14) = 0;
        MEM32(g_esp + 0x10) = phase;
        uint32_t index = MEM32(indexp);
        double t = (double)(uint64_t)phase;
        uint32_t sum = phase + step;
        phase = sum & 0xffffu;
        t *= (double)rf(0x5c1818);
        MEM32(phasep) = phase;
        double t2 = t * t;
        double a = (double)rf(g_ecx + index * 4) - (double)rf(g_ecx + index * 4 + 4);
        MEM32(indexp) = index + (sum >> 16);
        a *= (double)rf(0x5b2600);
        a -= (double)rf(g_ecx + index * 4 - 4);
        a += (double)rf(g_ecx + index * 4 + 8);
        a *= (double)rf(0x5b23f0);
        double t3 = t2 * t;
        a *= t3;
        double b = (double)rf(g_ecx + index * 4 + 4);
        b += b;
        b += (double)rf(g_ecx + index * 4 - 4);
        double c = (double)rf(g_ecx + index * 4) * (double)rf(0x5b2400);
        c += (double)rf(g_ecx + index * 4 + 8);
        c *= (double)rf(0x5b23f0);
        b -= c;
        b *= t2;
        a += b;
        c = (double)rf(g_ecx + index * 4 + 4) - (double)rf(g_ecx + index * 4 - 4);
        c *= (double)rf(0x5b23f0);
        c *= t;
        a += c;
        a += (double)rf(g_ecx + index * 4);
        MEM32(g_esp + 0x2c) -= 1;
        wf(out, (float)a);
        out += 4;
        g_backedges++;
        step = MEM32(g_esp + 0x30);
    } while (MEM32(g_esp + 0x2c));
    for (unsigned i = 1; i <= 5; i++) {
        unsigned slot = (8u - (unsigned)g_fp_top - i) & 7u;
        g_st[slot] = 0;
        g_st_i64[slot] = 0;
    }
    g_edx = phase;
    g_eax = phase << 16;
    g_edi = POP32_VAL(g_esp);
    MEM32(phasep) = g_eax;
    g_ebx = POP32_VAL(g_esp);
    g_esi = POP32_VAL(g_esp);
    g_ebp = POP32_VAL(g_esp);
    g_flag_a = g_esp;
    g_flag_b = 8;
    g_flag_cf = (uint32_t)(g_esp + 8) < g_esp;
    g_flag_k = FK_ADD;
    g_esp += 12;
    return 1;
}

extern void recomp_nfs_porsche_0059CC60(void);
static void cubic_resample(void) {
    if (!cubic_resample_try())
        recomp_nfs_porsche_0059CC60();
}
extern recomp_func_t nfs5_audio_kernel_lookup(uint32_t va);
extern recomp_func_t nfs5_xform_lookup(uint32_t va);
recomp_func_t nfs5_audio_lookup(uint32_t va) {
    if (va == 0x005B0730u)
        return nfs5_xform_lookup(va);
    if ((va & 0xFFFF0000u) != 0x00590000u)
        return NULL;
    if (va != 0x0059cc60u)
        return nfs5_audio_kernel_lookup(va);
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("NFS5_NATIVE_AUDIO");
        enabled = !value || strcmp(value, "0") != 0;
    }
    return enabled ? cubic_resample : NULL;
}

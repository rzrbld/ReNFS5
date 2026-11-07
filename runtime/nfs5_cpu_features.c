/* Virtual x86 CPU model seen by the recompiled game.
 *
 * Modelled on a Pentium II/III class CPU: x87, MMX, TSC, CMOV, FXSR absent and
 * no SSE/3DNow: the current translation still contains unsupported instructions
 * in those paths. Implement and validate them before advertising these bits.
 *
 * CPUID is observable behaviour: NFS5's optimized spline/audio/renderer
 * routines and its "ISMMX" check dispatch on it. Change this only with
 * runtime evidence that a path is needed, and re-check the unsupported
 * instruction ledger (docs/evidence/translation-*.json).
 */
#include "nfs5_cpu_features.h"
#include <string.h>

void nfs5_guest_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t out[4]) {
    (void)subleaf;
    out[0] = out[1] = out[2] = out[3] = 0;
    switch (leaf) {
    case 0:
        /* Highest basic leaf; vendor "GenuineIntel". */
        out[0] = 1;
        out[1] = 0x756e6547u; /* "Genu" */
        out[2] = 0x6c65746eu; /* "ntel" */
        out[3] = 0x49656e69u; /* "ineI" */
        break;
    case 1:
        /* Family 6 model 8 stepping 6 (Coppermine-ish), no brand index. */
        out[0] = 0x00000686u;
        out[1] = 0;
        out[2] = 0;
        /* edx: FPU(0) PSE(3) TSC(4) MSR(5) CX8(8) CMOV(15) MMX(23).
         * Deliberately absent: SSE(25), SSE2(26), PSN(13), PSE36(17), SEP(11). */
        out[3] = (1u << 0) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 8) |
                 (1u << 15) | (1u << 23);
        break;
    case 0x80000000u:
        /* No extended feature leaves: hides 3DNow!/3DNowExt/MMXExt. */
        out[0] = 0x80000000u;
        break;
    default:
        break;
    }
}

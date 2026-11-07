#pragma once
/* Virtual x86 CPUID for the NFS5 guest.
 *
 * The lifted game and its renderer contain AMD 3DNow! optimized paths whose
 * instruction sites the lift32 translator cannot run (pfmul/pfadd/pfacc/...).
 * NFS5 chooses those paths at runtime from CPUID, so the HLE must advertise a
 * CPU that has the paths the lifter implements (x87, TSC, CMOV, MMX) and no
 * 3DNow!/SSE, exactly as pcrecomp recommends for this class of binary.
 *
 * Included from generated functions.h *after* recomp_types.h so the CPUID
 * macro overrides the built-in no-op used on non-MSVC hosts.
 */
#include <stdint.h>
#include "recomp_types.h"

void nfs5_guest_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t out[4]);

#undef CPUID
#define CPUID(a, b, c, d) do {                                        \
    uint32_t _l = (uint32_t)(a), _s = (uint32_t)(c), _r[4];           \
    RECOMP_REGS_OUT();                                                \
    nfs5_guest_cpuid(_l, _s, _r);                                     \
    g_eax = _r[0]; g_ebx = _r[1]; g_ecx = _r[2]; g_edx = _r[3];       \
    RECOMP_REGS_IN();                                                 \
} while (0)

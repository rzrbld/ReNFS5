/* RECOMP_POISON_FLAGS checking build: report reads of condition flags that a
 * dropped call/entry hand-off left stale (see recomp_types.h). */
#ifdef RECOMP_POISON_FLAGS
#include <stdio.h>
#include <stdint.h>
#include "recomp_types.h"
void recomp_flag_poison_hit(const char *what, uint32_t kind, uint32_t cf) {
    static uintptr_t seen[256]; static int n;
    uintptr_t ra = (uintptr_t)__builtin_return_address(0);
    for (int i = 0; i < n; i++) if (seen[i] == ra) return;
    if (n < 256) seen[n++] = ra;
    fprintf(stderr, "[flag-poison] %s kind=0x%x cf=0x%x func=0x%08X host=%p\n",
            what, kind, cf, (unsigned)g_cur_func, (void *)ra);
}
#endif

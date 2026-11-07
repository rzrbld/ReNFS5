/* Portable function-entry ring for bring-up (the vendored recomp_trace.c is
 * Windows-only). Compiled in when the build sets RECOMP_TRACE; the generated
 * code calls recomp_trace_enter() from RECOMP_ENTER. Dumped by the host's
 * crash handler so a fault can be read back as the sequence of guest
 * functions that led to it, without a debugger.
 */
#include <stdio.h>
#include "recomp_types.h"

#ifdef RECOMP_TRACE
uint32_t g_enter_trace[RECOMP_ENTER_SIZE];
uint32_t g_enter_idx;

void recomp_trace_enter(uint32_t va) {
    g_enter_trace[g_enter_idx & (RECOMP_ENTER_SIZE - 1)] = va;
    g_enter_idx++;
}

void nfs5_trace_dump(void) {
    fprintf(stderr, "[trace] last %u guest entries (%u total), oldest first:\n",
            g_enter_idx < RECOMP_ENTER_SIZE ? g_enter_idx : RECOMP_ENTER_SIZE, g_enter_idx);
    uint32_t n = g_enter_idx < RECOMP_ENTER_SIZE ? g_enter_idx : RECOMP_ENTER_SIZE;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (g_enter_idx - n + i) & (RECOMP_ENTER_SIZE - 1);
        fprintf(stderr, "  %08x\n", g_enter_trace[idx]);
    }
}
#else
void nfs5_trace_dump(void) { fprintf(stderr, "[trace] RECOMP_TRACE not compiled in\n"); }
#endif

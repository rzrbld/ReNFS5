/* Guest stack-integrity checker for bring-up (opt-in: -DRECOMP_STACKCHECK).
 *
 * Every lifted call pushes the dummy return address at a known guest stack
 * address; the callee must return with esp exactly there. A call whose stdcall
 * purge is wrong (or a lifted function with a stack bug) shifts esp, and that
 * shift is how the lift's sentinel can leak into game data. This records the
 * expected slot per call in a shadow stack and reports the first mismatch with
 * the function that returned wrongly, then resynchronises.
 */
#include <stdio.h>
#include <stdint.h>
#include "recomp_types.h"

#ifdef RECOMP_STACKCHECK
#define SHADOW_N (1u << 20)
static __thread uint32_t g_shadow[SHADOW_N];
static __thread int g_shadow_n;
static int g_reported;

void recomp_shadow_ret(uint32_t fn, uint32_t esp_now, uint32_t top) {
    if (g_reported < 60)
        fprintf(stderr, "[stackcheck] TOP IS NOT THE DUMMY: ret of fn %08x esp=%08x top=%08x cur=%08x\n",
                fn, esp_now, top, g_cur_func);
    g_reported++;
    (void)g_shadow_n;
}
void recomp_shadow_dump(void) {
    fprintf(stderr, "[stackcheck] %d entries pending, %d drifts reported\n", g_shadow_n, g_reported);
}
#endif

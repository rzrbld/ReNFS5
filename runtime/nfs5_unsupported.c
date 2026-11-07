/* Fatal handler for x86 instructions the lift could not translate.
 *
 * The translation ledger (docs/evidence/translation-porsche.json) records 338
 * unsupported sites in Porsche.exe: AMD 3DNow! packed-float ops, femms,
 * fnstenv/fldenv, one cli and 16 int instructions. They are never silently
 * NOPed: if execution ever reaches one, this reports where and stops, so the
 * path is either avoided (virtual CPUID) or implemented in the lifter.
 */
#include <stdio.h>
#include <stdlib.h>
#include "win32hle.h"

void nfs_unsupported_instruction(uint32_t address, const char *instruction) {
    fprintf(stderr,
            "\n[nfs5] UNSUPPORTED GUEST INSTRUCTION %s at 0x%08X\n"
            "[nfs5] current function 0x%08X esp 0x%08X\n"
            "[nfs5] this path must be avoided via CPUID or added to the lifter; refusing to continue\n",
            instruction ? instruction : "(unknown)", address, g_cur_func, g_esp);
    fflush(stderr);
    abort();
}

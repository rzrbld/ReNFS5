/* Runtime statistics hooks the profiler asks for that win32hle does not expose.
 *
 * nfs5_guest_heap_high_water() is intentionally a real definition (not a weak
 * undefined symbol) so every link of the port resolves it: the AArch64 package
 * gate treats project-internal undefined symbols as a failure even when they
 * are weak. The win32hle heap keeps no high-water mark, so this reports 0
 * (unknown) rather than inventing a number; RSS/heap proxies are measured from
 * /proc by scripts/perf_report.sh.
 */
#include <stdint.h>

uint32_t nfs5_guest_heap_high_water(void) {
    return 0;   /* unknown: not tracked by the vendored win32hle heap */
}

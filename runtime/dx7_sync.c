/* The driver's sync(mode=0) is a Lock/Unlock pair with no pixel access.
 * Keep its callbacks and bookkeeping in the translated implementation, but
 * submit the ordered GPU command stream rather than copying the framebuffer.
 * No CPU access occurs in this operation; GPU resource ordering is preserved
 * by the single drawing context. Real pixel locks still block for readback. Other
 * calls to the driver's lock entry point still require ordinary CPU pixels. */
#include "render/renderer.h"
#include "win32hle.h"
#include <stdlib.h>
extern void recomp_nfs_dx7z_100012C0(void);
static __thread int sync_scope;
static void dx7_sync(void) {
    int previous = sync_scope;
    sync_scope = A32(0) == 0 && !getenv("NFS5_CPU_SYNC");
    recomp_nfs_dx7z_100012C0();
    sync_scope = previous;
}
int nfs5_dx7_sync_lock(uint32_t surface) {
    return sync_scope && g_cur_func == 0x100013c0u && surface == MEM32(0x10021b8cu);
}
extern recomp_func_t nfs5_audio_lookup(uint32_t va);
recomp_func_t nfs_recovered_lookup(uint32_t va) {
    return va == 0x100012c0u ? dx7_sync : nfs5_audio_lookup(va);
}

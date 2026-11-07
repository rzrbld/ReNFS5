/*
 * nfs5_prof.h - sampling profiler and frame statistics for the NFS5 port.
 *
 * Design
 * ------
 * The lifted code keeps the guest VA of the function it is executing in the
 * global `g_cur_func` (updated by RECOMP_ENTER on every lifted call). A
 * low-priority sampler thread reads it at ~200 Hz and builds per-function hit
 * counts, so no instrumentation of the 10,787 lifted functions is needed and
 * the cost is one relaxed 32-bit load per sample.
 *
 * Zero overhead when off: nfs5_prof.c self-starts from an ELF constructor only
 * when NFS5_PROFILE is set; without it there is no thread, no allocation and no
 * sampling. nfs5_frame_mark() is then a single predictable branch.
 *
 * Two ways to use it
 * ------------------
 * 1. In-process (the intended packaged dev build): the Makefile compiles
 *    runtime/profiling/ into the game; run with NFS5_PROFILE=1. The present
 *    path calls nfs5_frame_mark() for frame timing.
 * 2. LD_PRELOAD (no rebuild required, used for the first baseline):
 *      gcc -shared -fPIC -O2 -DNFS5_PROF_PRELOAD ... -o libnfs5_prof.so
 *      LD_PRELOAD=./libnfs5_prof.so NFS5_PROFILE=1 ./build/nfs5-desktop
 *    In this mode the library also interposes SDL_RenderPresent /
 *    SDL_UpdateWindowSurface / SDL_GL_SwapWindow to count presents, and reads
 *    g_cur_func through the executable's -rdynamic dynamic symbol table.
 *
 * Environment variables
 * ---------------------
 *   NFS5_PROFILE=1                    enable (anything else => off)
 *   NFS5_PROFILE_INTERVAL_MS=5        sampling interval in ms (200 Hz default)
 *   NFS5_PROFILE_OUT=FILE             report path (default stderr)
 *   NFS5_PROFILE_FPS_CSV=FILE         per-frame CSV (default: not written)
 *   NFS5_PROFILE_MEM=FILE             RSS/heap time series (default: not written)
 *   NFS5_PROFILE_CATALOG_DIR=DIR      VA->name catalogs (default build/translated;
 *                                     reads {porsche,dx7z}/catalog.json when present)
 *   NFS5_PROFILE_TOP=25               hotspot rows in the report
 *   NFS5_PROFILE_TIMELINE_MS=250      startup timeline bucket width
 *   NFS5_PROFILE_SNAPSHOT_MS=1000     periodic report snapshot; 0 disables
 *   NFS5_PROFILE_ACTIVE               set by the profiled process itself so that
 *                                     children it spawns (e.g. win32hle's popen
 *                                     font lookup) do not start a second sampler
 *                                     (periodic snapshots survive a crash, so the
 *                                     startup profile of a boot that faults is
 *                                     still available)
 */
#ifndef NFS5_PROF_H
#define NFS5_PROF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called from the host present/flip path, once per presented frame. Counts a
 * frame for FPS / 1%-low / spike statistics. Safe to call when profiling is
 * off (no-op); safe before nfs5_prof_start(). */
void nfs5_frame_mark(void);

/* Explicit control. The constructor already starts when NFS5_PROFILE is set;
 * these exist for harnesses (perf_report.sh uses the env path only). */
int  nfs5_prof_start(void);      /* 0 on success, -1 if unsupported/already on */
void nfs5_prof_stop(void);       /* stop and write the final report */
void nfs5_prof_dump(void);       /* write a report snapshot now */
int  nfs5_prof_enabled(void);    /* 1 when the profiler is running */

/*
 * Optional exact guest-heap high-water hook.
 *
 * win32hle's guest heap (hle_heap_alloc) is malloc/mmap-backed, so the
 * profiler reports mallinfo2().uordblks as a *proxy* for guest heap use. If
 * the HLE layer ever grows a real accounting counter it can implement this
 * symbol (strong definition in third_party/pcrecomp/runtime/win32hle/
 * guest_mem.c) and the profiler will report it instead; the header only
 * declares a weak reference, so no link change is needed.
 */
uint32_t nfs5_guest_heap_high_water(void);   /* defined in runtime/nfs5_stats.c */

/*
 * Optional hook a project crash handler can call to record why the run ended
 * (e.g. "SIGSEGV at 0x...") in the next report snapshot. No-op when off.
 */
void nfs5_prof_note_event(const char *what);

/*
 * Named phase timers and counters for subsystems (renderer, audio, ...).
 *
 * Register a slot once at subsystem init, then bracket the phase:
 *
 *     static int slot = -1;
 *     if (slot < 0) slot = nfs5_prof_phase_slot("render_flush");
 *     nfs5_prof_phase_begin(slot);          // no-op when profiling is off
 *     ... flush + glReadPixels ...
 *     nfs5_prof_phase_end(slot);
 *     nfs5_prof_counter_add(nfs5_prof_counter_slot("tex_upload_bytes"), bytes);
 *
 * Semantics: phase_begin/end measure wall time between the two calls on the
 * calling thread (not nesting-aware; one open span per slot). Counters are
 * monotonic totals; the report prints total and average per event. All hooks
 * are cheap single-branch no-ops when profiling is off. Slot ids are -1 when
 * the profiler is off or the table is full; pass them straight through.
 *
 * Link note: a subsystem may declare these weak (`__attribute__((weak))`) and
 * call them through a null check, exactly like nfs5_frame_mark(), so the
 * subsystem links with or without runtime/profiling/nfs5_prof.c.
 */
#define NFS5_PROF_MAX_PHASES   16
#define NFS5_PROF_MAX_COUNTERS 16

int  nfs5_prof_phase_slot(const char *name);
void nfs5_prof_phase_begin(int slot);
void nfs5_prof_phase_end(int slot);
int  nfs5_prof_counter_slot(const char *name);
void nfs5_prof_counter_add(int slot, uint64_t value);

#ifdef __cplusplus
}
#endif

#endif /* NFS5_PROF_H */

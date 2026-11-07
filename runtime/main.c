/* NFS5 static-recompilation host.
 *
 * Boots the user's own Porsche.exe through pcrecomp's win32hle: maps the PE
 * image data at its original VAs, binds its imports to HLE shims, then runs
 * the lifted CRT entry point (which reaches the game's real WinMain and
 * message loops).
 *
 * Install root layout (same on desktop and handheld):
 *   root/data/       original game files, mounted guest C: (read-only base)
 *   root/userdata/   guest S: and the write overlay for C:
 *   root/config/     registry.txt and port settings
 *
 * Override with NFS5_ROOT / NFS5_DATA / NFS5_USERDATA / NFS5_CONFIG.
 */
#define _GNU_SOURCE
#include "win32hle.h"
#include "host.h"
#include <SDL.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <execinfo.h>
#include <unistd.h>
#include <pthread.h>

/* Shim tables that the game's imports need beyond the stock win32hle set.
 * Defined by the HLE modules; weak so a staged build still links. */
extern const win32hle_shim nfs_extra_shims[] __attribute__((weak));
extern void nfs_seed_registry(void) __attribute__((weak));
extern void hle_set_os_version(uint32_t major, uint32_t minor, uint32_t build,
                               uint32_t platform) __attribute__((weak));
extern void nfs_extra_init(const char *data_root, const char *userdata_root,
                           const char *registry_file) __attribute__((weak));
extern void nfs_platform_poll(void) __attribute__((weak));
extern int nfs_platform_configure(int argc, char **argv) __attribute__((weak));
void nfs5_trace_dump(void);
void nfs5_input_script_start(void);
void nfs5_present_dump_start(void);

static void crash_handler(int sig, siginfo_t *info, void *ctx) {
    (void)ctx;
    dprintf(2,
            "\n[nfs5] FATAL signal=%d fault_address=%p guest_base=%p\n"
            "[nfs5] current_guest=0x%08X import=%s\n"
            "[nfs5] eax=%08x ebx=%08x ecx=%08x edx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x fs=%08x\n",
            sig, info ? info->si_addr : NULL, (void *)g_mem_base, g_cur_func,
            g_cur_import ? g_cur_import : "(none)",
            g_eax, g_ebx, g_ecx, g_edx, g_esp, g_ebp, g_esi, g_edi, g_fs_base);
    nfs5_trace_dump();
    dprintf(2, "[nfs5] guest stack (resolvable words at esp):\n");
    for (int i = 0; i < 64; i++) {
        uint32_t va = MEM32(g_esp + 4u * (uint32_t)i);
        recomp_func_t fn = recomp_lookup(va);
        const char *nm = hle_name(va);
        if (fn || nm) dprintf(2, "  +%02d %08x %s%s\n", i, va, fn ? "guest-fn" : "", nm ? nm : "");
    }
    {   /* host backtrace: which HLE/glue code was running when the guest faulted */
        void *trace[40];
        int n = backtrace(trace, 40);
        dprintf(2, "[nfs5] host backtrace:\n");
        backtrace_symbols_fd(trace, n, 2);
    }
    dprintf(2, "[nfs5] last indirect calls (target <- caller):\n");
    for (int i = 0; i < ICALL_TRACE_SIZE; i++) {
        uint32_t idx = (g_icall_trace_idx - ICALL_TRACE_SIZE + (uint32_t)i) & (ICALL_TRACE_SIZE - 1);
        if (g_icall_trace[idx]) dprintf(2, "  [%2d] %08x <- %08x\n", i, g_icall_trace[idx], g_icall_from[idx]);
    }
    _exit(128 + sig);
}

static void mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0755) && errno != EEXIST) { /* keep going; the game reports open failures */ }
        *p = '/';
    }
    if (mkdir(tmp, 0755) && errno != EEXIST) { }
}

static const char *env_or(const char *name, const char *fallback, char *out, size_t n) {
    const char *v = getenv(name);
    if (v && *v) return v;
    snprintf(out, n, "%s", fallback);
    return out;
}

/* One independent buffer per path: env_or writes the fallback into `out`, so
 * the fallback must not be the same buffer (it was, and the paths came out
 * empty). */
static void path_env(const char *name, const char *fmt_a, const char *root,
                     const char *fmt_b, char *out, size_t n) {
    const char *v = getenv(name);
    if (v && *v) { snprintf(out, n, "%s", v); return; }
    if (fmt_a) snprintf(out, n, fmt_a, root);
    else snprintf(out, n, fmt_b, root);
}

static void exe_root(char *out, size_t n) {
    char exe[PATH_MAX];
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k > 0) {
        exe[k] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) { *slash = 0; snprintf(out, n, "%s", exe); return; }
    }
    if (!getcwd(out, n)) snprintf(out, n, ".");
}

/* Bring-up watchdog: a sampling thread that prints the guest function currently
 * executing (g_cur_func is maintained at every lifted function entry), plus a
 * short ring of recent calls. Enough to tell a spin from a wait without a full
 * profiler; enabled with NFS5_WATCHDOG=1. Reads are deliberately racy. */
/* NFS5_SCAN_BINDINGS=<ms>: search guest memory for the keyboard binding table:
 * accelerate=Up(0x48), brake=Down(0x50), left(0x4B), right(0x4D),
 * handbrake=Space(0x39), shift-up=A(0x1E). Uses /proc/self/mem so unmapped
 * guest pages cannot fault the scanner. Bring-up aid. */
/* NFS5_SNAP_AT="ms1,ms2,...": dump readable guest memory to /tmp/nfs5snap-<ms>.bin
 * plus an index file at those points. Used to diff guest state around input
 * events (which cells change when a key is pressed?) - bring-up aid. */
static void snap_guest_memory(uint32_t stamp) {
    char path[64], idx[64];
    const char *dir = getenv("NFS5_SNAP_DIR");
    if (!dir || !*dir) return;   /* only with an explicit absolute directory */
    snprintf(path, sizeof path, "%s/nfs5snap-%u.bin", dir, stamp);
    snprintf(idx, sizeof idx, "%s/nfs5snap-%u.txt", dir, stamp);
    FILE *out = fopen(path, "wb"), *xout = fopen(idx, "w");
    if (!out || !xout) { if (out) fclose(out); if (xout) fclose(xout); return; }
    FILE *mf = fopen("/proc/self/maps", "r");
    if (!mf) { fclose(out); fclose(xout); return; }
    char line[512];
    while (fgets(line, sizeof line, mf)) {
        unsigned long h0 = 0, h1 = 0;
        char perms[8] = "";
        if (sscanf(line, "%lx-%lx %7s", &h0, &h1, perms) != 3) continue;
        if (perms[0] != 'r') continue;
        intptr_t g0 = (intptr_t)h0 - g_mem_base, g1 = (intptr_t)h1 - g_mem_base;
        if (g0 < 0 || g1 <= 0 || g0 >= 0x40000000) continue;
        long long off = ftell(out);
        if (off < 0) continue;
        uint32_t a = (uint32_t)g0, e = (uint32_t)g1;
        if (e > 0x40000000u) e = 0x40000000u;
        int wrote = 0;
        for (uint32_t page = a & ~0xFFFu; page < e; page += 0x1000) {
            uint8_t buf[4096];
            int mfd = open("/proc/self/mem", O_RDONLY);
            if (mfd < 0) break;
            ssize_t r = pread(mfd, buf, sizeof buf, (off_t)(g_mem_base + (intptr_t)page));
            close(mfd);
            if (r <= 0) break;
            fwrite(buf, 1, (size_t)r, out);
            wrote += (int)r;
        }
        if (wrote) fprintf(xout, "%08x %08x %lld\n", a, e, off);
    }
    fclose(mf); fclose(out); fclose(xout);
    fprintf(stderr, "[snap] dumped at %u -> %s (%s)\n", stamp, path, idx);
}
static uint32_t g_t0_ms;   /* process start (hle_ticks_ms is boot-relative GetTickCount) */
static void *watchdog(void *arg) {
    (void)arg;
    for (;;) {
        usleep(1000000);
        fprintf(stderr, "[watchdog] guest=%08x esp=%08x eax=%08x ebx=%08x ecx=%08x edx=%08x\n",
                g_cur_func, g_esp, g_eax, g_ebx, g_ecx, g_edx);
        for (int i = 1; i <= 4; i++) {
            uint32_t idx = (g_icall_trace_idx - (uint32_t)i) & (ICALL_TRACE_SIZE - 1);
            if (g_icall_trace[idx])
                fprintf(stderr, "[watchdog]   icall %08x <- %08x\n", g_icall_trace[idx], g_icall_from[idx]);
        }
        {   /* guest memory snapshots at requested times (bring-up aid) */
            static char snaps[512];
            static int snaps_parsed, ns;
            static uint32_t ms[8];
            static int done[8];
            if (!snaps_parsed) {
                snaps_parsed = 1;
                const char *e = getenv("NFS5_SNAP_AT");
                if (e) snprintf(snaps, sizeof snaps, "%s", e);
                char *q = snaps;
                while (q && *q && ns < 8) { ms[ns++] = (uint32_t)strtoul(q, &q, 10); if (*q == ',') q++; else break; }
            }
            for (int i = 0; i < ns; i++)
                if (!done[i] && (uint32_t)(hle_ticks_ms() - g_t0_ms) >= ms[i]) { done[i] = 1; snap_guest_memory(ms[i]); }
        }
        if (getenv("NFS5_MEMWATCH")) {
            /* Bring-up probe for the input layer: the game keeps its DirectInput
             * scancode key-state array at 0x5DE028 and the keyboard-focus window
             * at 0x6B7BF8. Print a few interesting bytes each second. */
            fprintf(stderr, "[memwatch] kbd_focus=%08x key[0x48]=%02x key[0xC8]=%02x key[0x2E]=%02x key[0x1C]=%02x\n",
                    MEM32(0x6B7BF8), MEM8(0x5DE028 + 0x48), MEM8(0x5DE028 + 0xC8),
                    MEM8(0x5DE028 + 0x2E), MEM8(0x5DE028 + 0x1C));
            /* race input state candidates (see docs/analysis note): callback
             * registration globals, controller-state table, poll outputs. */
            fprintf(stderr, "[istate] 59e0=%08x 59e4=%08x 59e8=%08x 99b8=%08x p1=%08x p2=%08x p3=%08x p4=%08x\n",
                    MEM32(0x6559E0), MEM32(0x6559E4), MEM32(0x6559E8), MEM32(0x5E99B8),
                    MEM32(0x6B7C20), MEM32(0x6B7C24), MEM32(0x6B7C28), MEM32(0x6B7C2C));
            fprintf(stderr, "[istate2]");
            for (uint32_t a = 0x5E95C0; a < 0x5E9680; a += 4) fprintf(stderr, " %08x", MEM32(a));
            fprintf(stderr, "\n");
            fprintf(stderr, "[kbd] layout=%08x lastkey=%08x key48=%02x keyc8=%02x\n",
                    MEM32(0x69E5A0), MEM32(0x6BD9FC), MEM8(0x5DE028 + 0x48), MEM8(0x5DE028 + 0xC8));
            fprintf(stderr, "[istate3] 6559c0:");
            for (uint32_t a = 0x6559C0; a < 0x655A20; a += 4) fprintf(stderr, " %08x", MEM32(a));
            fprintf(stderr, "\n");
            /* recent guest indirect calls: what does the race loop dispatch to? */
            fprintf(stderr, "[memwatch] icalls:");
            for (int i = 1; i <= 24; i++) {
                uint32_t idx = (g_icall_trace_idx - (uint32_t)i) & (ICALL_TRACE_SIZE - 1);
                if (g_icall_trace[idx]) fprintf(stderr, " %08x", g_icall_trace[idx]);
            }
            fprintf(stderr, "\n");
        }
    }
    return NULL;
}

/* One game instance per payload: two copies on a 4-core H700 halve the frame
 * rate for both and make the port look broken. The lock file lives next to the
 * saves and the descriptor stays open for the life of the process. */
static int g_instance_lock = -1;
static int acquire_single_instance(const char *dir) {
    char path[2048];
    snprintf(path, sizeof path, "%s/.nfs5-running", dir);
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) return 0;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "[nfs5] another instance is already running; exiting\n");
        close(fd);
        return -1;
    }
    g_instance_lock = fd;
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IOLBF, 0);
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    g_t0_ms = hle_ticks_ms();
    hle_winmm_pad_preinit();   /* headless virtual pad, before the game enumerates devices */

    if (!getenv("NFS5_NO_CRASH_HANDLER")) {   /* run under gdb to catch the fault in the debugger */
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = crash_handler;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        sigaction(SIGFPE, &sa, NULL);
        sigaction(SIGABRT, &sa, NULL);
    }

    char root_buf[PATH_MAX];
    char root[PATH_MAX], data[PATH_MAX], userdata[PATH_MAX], config[PATH_MAX], regfile[PATH_MAX];
    exe_root(root_buf, sizeof root_buf);
    {   /* no self-copy through snprintf: fallback and destination are separate */
        const char *root_env = getenv("NFS5_ROOT");
        snprintf(root, sizeof root, "%s", (root_env && *root_env) ? root_env : root_buf);
    }
    path_env("NFS5_DATA", "%s/data", root, NULL, data, sizeof data);
    /* Player data lives beside the binaries: <root>/saves (game SaveData) and
     * <root>/configs (game and port configuration). Installations made with the
     * older <root>/userdata/{saves,configs} layout are migrated once. */
    {
        char legacy[PATH_MAX], legacy_saves[PATH_MAX], new_saves[PATH_MAX];
        char legacy_cfg[PATH_MAX], new_cfg[PATH_MAX];
        snprintf(legacy, sizeof legacy, "%s/userdata", root);
        snprintf(legacy_saves, sizeof legacy_saves, "%s/saves", legacy);
        snprintf(new_saves, sizeof new_saves, "%s/saves", root);
        if (access(legacy_saves, F_OK) == 0 && access(new_saves, F_OK) != 0) {
            rename(legacy_saves, new_saves);
            snprintf(legacy_cfg, sizeof legacy_cfg, "%s/configs", legacy);
            snprintf(new_cfg, sizeof new_cfg, "%s/configs", root);
            if (access(legacy_cfg, F_OK) == 0 && access(new_cfg, F_OK) != 0) rename(legacy_cfg, new_cfg);
            rmdir(legacy);
        }
    }
    path_env("NFS5_USERDATA", "%s", root, NULL, userdata, sizeof userdata);
    path_env("NFS5_CONFIG", "%s/configs", root, NULL, config, sizeof config);
    mkdir_p(userdata);
    {
        char d[PATH_MAX];
        snprintf(d, sizeof d, "%s/saves", userdata);
        mkdir_p(d);
        snprintf(d, sizeof d, "%s/configs", userdata);
        mkdir_p(d);
        /* one-time migration from the old flat layout */
        char oldp[PATH_MAX], newp[PATH_MAX];
        snprintf(oldp, sizeof oldp, "%s/SaveData", userdata);
        snprintf(newp, sizeof newp, "%s/saves", userdata);
        if (access(oldp, F_OK) == 0 && access(newp, F_OK) == 0) {
            struct dirent **nl = NULL;
            int nn = scandir(oldp, &nl, NULL, alphasort);
            for (int i = 0; i < nn; i++) {
                char a[PATH_MAX], b[PATH_MAX];
                snprintf(a, sizeof a, "%s/%s", oldp, nl[i]->d_name);
                snprintf(b, sizeof b, "%s/%s", newp, nl[i]->d_name);
                if (access(b, F_OK) != 0 && strcmp(nl[i]->d_name, ".") && strcmp(nl[i]->d_name, ".."))
                    rename(a, b);
                free(nl[i]);
            }
            free(nl);
        }
        const char *legacy[] = { "fe.txt", "fe_out.txt" };
        for (size_t i = 0; i < sizeof legacy / sizeof *legacy; i++) {
            snprintf(oldp, sizeof oldp, "%s/%s", userdata, legacy[i]);
            snprintf(newp, sizeof newp, "%s/configs/%s", userdata, legacy[i]);
            if (access(oldp, F_OK) == 0 && access(newp, F_OK) != 0) rename(oldp, newp);
        }
        /* the old flat port-config directory moves under userdata/configs */
        const char *legacy_cfg[] = { "launcher.cfg", "controller.cfg", "registry.txt",
                                     "install.json", "launcher-status.txt", "launcher-worker.log" };
        for (size_t i = 0; i < sizeof legacy_cfg / sizeof *legacy_cfg; i++) {
            snprintf(oldp, sizeof oldp, "%s/config/%s", root, legacy_cfg[i]);
            snprintf(newp, sizeof newp, "%s/configs/%s", userdata, legacy_cfg[i]);
            if (access(oldp, F_OK) == 0 && access(newp, F_OK) != 0) rename(oldp, newp);
        }
    }
    mkdir_p(config);

    /* Desktop development helper: a fresh checkout has no build/run/data yet;
     * fall back to the extracted-input symlink only in the build tree. */
    char probe[PATH_MAX];
    snprintf(probe, sizeof probe, "%s/Porsche.exe", data);
    if (access(probe, R_OK)) {
        snprintf(probe, sizeof probe, "%s/game-input/Porsche.exe", root);
        if (!access(probe, R_OK)) snprintf(data, sizeof data, "%s/game-input", root);
    }
    snprintf(probe, sizeof probe, "%s/Porsche.exe", data);
    if (access(probe, R_OK)) {
        fprintf(stderr, "[nfs5] cannot find Porsche.exe: data dir is %s\n"
                        "[nfs5] set NFS5_DATA to the extracted game directory\n", data);
        return 2;
    }
    snprintf(regfile, sizeof regfile, "%s/registry.txt", config);

    if (chdir(data)) { perror(data); return 1; }

    recomp_host_init();
    if (nfs_extra_shims && win32hle_register(nfs_extra_shims))
        fprintf(stderr, "[nfs5] registered extra import shims\n");
    hle_set_drive('D', data);          /* no separate CD: the installed tree is complete */
    hle_set_command_line("C:\\Porsche.exe");
    if (nfs_extra_init) {
        /* Maps C:/S:, installs the write overlay and seeds the installer's
         * registry values (win32hle/nfs_extra.c). */
        nfs_extra_init(data, userdata, regfile);
    } else {
        hle_set_drive('C', data);
        hle_set_drive('S', userdata);
        hle_set_overlay(data, userdata);
        hle_reg_load(regfile);
    }
    /* NFS5's renderer selection asks the DirectX registry for its version and
     * refuses the DX7 renderer below 7 ("You have DirectX %d. Please install
     * DirectX 7.", set3d.c). The port always provides DirectX 7-era APIs, so
     * the registry must say DirectX 7, as a DX7 install would have left it. */
    hle_reg_set_string(0x80000002u, "Software\\Microsoft\\DirectX", "Version", "4.07.00.0716");
    hle_reg_set_string(0x80000002u, "Software\\Microsoft\\DirectX", "InstalledVersion", "4.07.00.0716");
    if (nfs_seed_registry) nfs_seed_registry();

    /* NFS5 is a 1999/2000 Win9x title: its renderer selection refuses to run
     * when GetVersionExA reports an NT platform ("You have DirectX 0"), so the
     * guest sees Windows 98 unless a host overrides it. */
    if (hle_set_os_version) hle_set_os_version(4, 10, 1998, 1);
    win32hle_trace = getenv("NFS5_TRACE_IMPORTS") != NULL;
    int permissive = getenv("NFS5_STRICT_IMPORTS") == NULL;

    if (acquire_single_instance(userdata) < 0) return 1;
    fprintf(stderr, "[nfs5] root=%s data=%s userdata=%s config=%s\n",
            root, data, userdata, config);
    int unresolved = permissive ? recomp_host_boot_permissive("Porsche.exe")
                                : recomp_host_boot("Porsche.exe");
    if (unresolved && !permissive) return 3;
    {   /* SDL also supports direct handheld displays without X11/Wayland.
         * Only explicit diagnostic requests select headless operation. */
        const char *drv = getenv("SDL_VIDEODRIVER");
        const char *requested = getenv("NFS5_HEADLESS");
        int headless = requested && *requested ? strcmp(requested, "0") != 0
            : (drv && (!strcmp(drv, "offscreen") || !strcmp(drv, "dummy")));
        /* NFS5_FULLSCREEN=1 (set by the packaged nfs5-env.sh): handheld
         * firmwares may run a compositor (e.g. Weston on Anbernic's stock
         * RK3566/RK3568 Linux), where a plain window gets panels and
         * decorations and no longer fits the screen. Without it, a desktop
         * session (X11/Wayland) gets a window, anything else fullscreen. */
        const char *fs = getenv("NFS5_FULLSCREEN");
        int fullscreen = fs && *fs ? strcmp(fs, "0") != 0 : !getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY");
        fprintf(stderr, "[nfs5] display mode=%s fullscreen=%d SDL driver=%s\n",
                headless ? "headless" : "visible", fullscreen, drv ? drv : "auto");
        if (hle_screen_open("ReNFS5", fullscreen, headless) != 0) {
            fprintf(stderr, "[nfs5] cannot open game display: %s\n", SDL_GetError());
            return 1;
        }
    }
    fprintf(stderr, "[nfs5] entry=0x%08X unresolved_imports=%d, entering lifted CRT\n",
            recomp_host_entry(), unresolved);
    if (nfs_platform_configure && argc > 1 && !strcmp(argv[1], "--configure"))
        return nfs_platform_configure(argc, argv);
    if (getenv("NFS5_WATCHDOG")) {
        pthread_t t;
        if (!pthread_create(&t, NULL, watchdog, NULL)) pthread_detach(t);
    }
    nfs5_present_dump_start();
    nfs5_input_script_start();
    if (nfs_platform_poll) {
        /* A launcher hook may run before the guest's own loop starts. */
        nfs_platform_poll();
    }
    uint32_t rc = recomp_host_run();
    fprintf(stderr, "[nfs5] guest entry returned 0x%X\n", rc);
    return (int)(rc & 0xff);
}

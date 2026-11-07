/*
 * nfs_extra.c - the parts of Win32 NFS5 (Porsche Unleashed) imports that are
 * not Win32 at all: its copy-protection updater DLL mrbupd.dll, and the
 * registry seed an installer would have written.
 *
 * mrbupd.dll is EA/MicroRouter's "Merlin" update checker. Porsche.exe imports
 * five of its exports statically and calls them from its startup/update path
 * (call sites 0x4A5849..0x4A58FC, the only references; disassembled from the
 * user's own Porsche.exe):
 *
 *   h = mrbInitUpdate(&cfg);          cdecl 1 arg  -> context pointer
 *   if (mrbIsPrepareNeeded(h)) ...    cdecl 1 arg  -> BOOL
 *   if (mrbIsRepairNeeded(h)) ...     cdecl 1 arg  -> BOOL
 *   r = mrbIsUpdateAvailable(h,&av);  cdecl 2 args -> 1 if av != 0
 *   mrbTermUpdate(h);                 cdecl 1 arg
 *
 * The five exports are UNDECORATED and disassemble to plain `ret`, i.e. cdecl:
 * the caller cleans the arguments (the game does one `add esp,8` after the two
 * one-arg calls). A stdcall shim (RET's callee-clean purge) would corrupt the
 * game's stack, so they return with RET_CDECL below.
 *
 * This port has no network updater and no EA update service: the shims report
 * "no update available", which is the branch the game already handles (it
 * skips the update/repair UI and continues into WinMain).
 *
 * The registry seed is what the game's installer would have left behind:
 * HKLM\SOFTWARE\Electronic Arts\Need for Speed - Porsche Unleashed (the key
 * 3DSetup/3dsetup.ini names as RegistryKey) and the two alternate spellings
 * the binary probes (0x1D12AC/0x1D12E8/0x1D1320). Values are the ones the
 * binary asks for by name; absent values are traced at runtime (see
 * advapi32.c) so this seed is corrected from evidence, not guessed silently.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include "win32hle.h"

#define HKLM 0x80000002u
#define HKCU 0x80000001u
#define RET_CDECL(eax) do { g_eax = (uint32_t)(eax); g_esp += 4u; return; } while (0)

static int trace(void) {
    static int t = -1;
    if (t < 0) t = (getenv("NFS5_TRACE_MRB") != NULL || win32hle_trace) ? 1 : 0;
    return t;
}

/* One fake mrbupd context. Not the real 0x118-byte structure; the game only
 * passes it back to these shims, which ignore its contents. */
static uint32_t context(void) {
    static uint32_t ctx;
    if (!ctx) {
        uint8_t *p = (uint8_t *)hle_alloc(0x118);
        if (!p) hle_fatal("nfs_extra: no guest memory for mrbupd context");
        ctx = hle_va(p);
    }
    return ctx;
}

static void mrb_InitUpdate(void) {                 /* (void *cfg) -> context */
    if (trace()) fprintf(stderr, "[mrbupd] mrbInitUpdate(cfg=%#x) -> no update\n", A32(0));
    context();
    RET_CDECL(context());
}
static void mrb_IsPrepareNeeded(void) {            /* (context) -> BOOL */
    if (trace()) fprintf(stderr, "[mrbupd] mrbIsPrepareNeeded(%#x) -> 0\n", A32(0));
    RET_CDECL(0);
}
static void mrb_IsRepairNeeded(void) {             /* (context) -> BOOL */
    if (trace()) fprintf(stderr, "[mrbupd] mrbIsRepairNeeded(%#x) -> 0\n", A32(0));
    RET_CDECL(0);
}
static void mrb_IsUpdateAvailable(void) {          /* (context, DWORD *out) -> BOOL */
    uint32_t out = A32(1);
    if (out) MEM32(out) = 0;
    if (trace()) fprintf(stderr, "[mrbupd] mrbIsUpdateAvailable(%#x, out=%#x) -> 0 (none)\n", A32(0), out);
    RET_CDECL(0);
}
/* gimme.dll's only export: ConvertFile(path-in, path-out, flags) -> BOOL.
 *
 * ABI: CDECL. The call site at Porsche.exe 0x4DBE2F-0x4DBE38 pushes three
 * arguments, calls the resolved pointer, and then does `add esp,0xC` — the
 * caller purges, so a stdcall-style RET here would add another 12 bytes and
 * drift the guest stack (that drift was observed as a poisoned/NULL `this`
 * pointer in the front-end's loading transition). See RET_CDECL above.
 *
 * Semantics: the installed game files are already usable, so this answers the
 * conversion request for its usual FSH pair (FEData/art/AllDrivr.fsh ->
 * SaveData/pic16.fsh) by copying the source bytes to the destination and
 * reporting success, which is what the retail converter produces here (a
 * header-preserving copy). Any failure returns 0, the game's normal
 * "cannot convert" path. */
static void gimme_ConvertFile(void) {
    const char *src = ASTR(0), *dst = ASTR(1);
    uint32_t flags = A32(2);
    int ok = 0;
    if (src && dst && *src && *dst) {
        char host_src[1024], host_dst[1024];
        if (hle_host_path(src, host_src, sizeof host_src)) {
            hle_host_path_for_write(dst, host_dst, sizeof host_dst, 0);
            FILE *in = fopen(host_src, "rb");
            FILE *out = in ? fopen(host_dst, "wb") : NULL;
            if (in && out) {
                char buf[65536];
                size_t n;
                ok = 1;
                while ((n = fread(buf, 1, sizeof buf, in)) > 0)
                    if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
            }
            if (in) fclose(in);
            if (out) fclose(out);
        }
    }
    if (trace())
        fprintf(stderr, "[gimme] ConvertFile(\"%s\", \"%s\", %u) -> %d\n",
                src ? src : "", dst ? dst : "", flags, ok);
    RET_CDECL(ok);
}
static void mrb_TermUpdate(void) {                 /* (context) -> void */
    if (trace()) fprintf(stderr, "[mrbupd] mrbTermUpdate(%#x)\n", A32(0));
    RET_CDECL(0);
}

const win32hle_shim nfs_extra_shims[] = {
    { "mrbInitUpdate",         mrb_InitUpdate },
    { "mrbIsPrepareNeeded",    mrb_IsPrepareNeeded },
    { "mrbIsRepairNeeded",     mrb_IsRepairNeeded },
    { "mrbIsUpdateAvailable",  mrb_IsUpdateAvailable },
    { "mrbTermUpdate",         mrb_TermUpdate },
    { "ConvertFile",           gimme_ConvertFile },   /* gimme.dll (SafeDisc-era file converter) */
    { 0, 0 }
};

/* ---- the registry an install would have written ---- */
static const char *const g_game_keys[] = {
    "Software\\Electronic Arts\\Need for Speed - Porsche Unleashed",   /* 3dsetup.ini RegistryKey */
    "Software\\Electronic Arts\\Need for Speed - Porsche 2000",       /* probed alternate (0x1D12E8) */
    "Software\\Electronic Arts\\Need for Speed - Porsche",            /* probed alternate (0x1D1320) */
};

void nfs_extra_seed_registry(void) {
    for (size_t i = 0; i < sizeof g_game_keys / sizeof *g_game_keys; i++) {
        const char *k = g_game_keys[i];
        /* What the game asks for by name. Strings: the install/CD roots (the
         * game runs from C: here), the update/registration URLs it carries as
         * literals, and the 3DSetup keys its setup program stores. The path
         * values are the guest view, not host paths. */
        hle_reg_set_string(HKLM, k, "InstallPath", "C:\\");
        hle_reg_set_string(HKLM, k, "InstallDir", "C:\\");
        hle_reg_set_string(HKLM, k, "CDPath", "C:\\");
        hle_reg_set_string(HKLM, k, "UpdateUrl", "http://trans.ea.com/games/Pioneer/Nfs5");
        hle_reg_set_string(HKLM, k, "RegUrl", "http://login.earacing.com");
        hle_reg_set_string(HKLM, k, "Version", "3.2");
        hle_reg_set_dword(HKLM, k, "Installed", 1);
        /* 3DSetup's persisted choices (strings 0x1D6FB4..0x1D7020), with the
         * software DirectDraw driver: the game's own ddraw path, presented by
         * the port's presenter. */
        hle_reg_set_string(HKLM, k, "Thrash Driver", "dx7");   /* the loader makes this "dx7z.dll" */
        hle_reg_set_string(HKLM, k, "D3D Device", "CARD_Software");
        hle_reg_set_string(HKLM, k, "Thrash Resolution", "640x480");
        hle_reg_set_dword(HKLM, k, "Triple Buffer", 0);
        hle_reg_set_dword(HKLM, k, "Hardware Acceleration", 0);
        hle_reg_set_dword(HKLM, k, "Language", 0);
    }
    if (getenv("NFS5_TRACE_REG") || win32hle_trace)
        fprintf(stderr, "[nfs_extra] registry seeded under HKLM\\Software\\Electronic Arts\\\n");
}

/* The port's start-up contract (chosen by the Lead):
 *   data_root      the extracted game files, guest C:  (read-only source)
 *   userdata_root  the write overlay for C:, guest S:  (SaveData, config)
 *   registry_file  persistent registry, "key\name=type:value" lines
 * Relative guest paths resolve against data_root, so the game's fe.txt,
 * 3DSetup\3dsetup.ini and GameData\... open without the host cwd mattering;
 * writes land in userdata_root through the overlay instead of touching the
 * read-only extraction. `make dev-env` creates this layout. */
void nfs_extra_init(const char *data_root, const char *userdata_root, const char *registry_file) {
    if (data_root) hle_set_drive('C', data_root);
    if (userdata_root) hle_set_drive('S', userdata_root);
    if (data_root && userdata_root) hle_set_overlay(data_root, userdata_root);
    if (data_root && chdir(data_root) != 0)
        fprintf(stderr, "[nfs_extra] cannot chdir to data root %s\n", data_root);
    nfs_extra_seed_registry();
    if (registry_file) {
        char dir[1024];
        snprintf(dir, sizeof dir, "%s", registry_file);
        char *slash = strrchr(dir, '/');
        if (slash) { *slash = 0; mkdir(dir, 0755); }
        hle_reg_load(registry_file);   /* persisted values override the seed; sets the write-through file */
    }
    if (getenv("NFS5_TRACE_EXTRA") || win32hle_trace)
        fprintf(stderr, "[nfs_extra] init: C:=%s S:=%s reg=%s\n",
                data_root ? data_root : "(none)", userdata_root ? userdata_root : "(none)",
                registry_file ? registry_file : "(none)");
}

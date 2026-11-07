/* Install/verify/import support for the NFS5 port launcher.
 *
 * The port package never contains the original game. The user supplies an
 * extracted v3.2 install (or a ZIP of one) under <root>/source/; the launcher
 * verifies the version by SHA-256 and copies the game tree to <root>/data/.
 */
#ifndef NFS5_INSTALL_H
#define NFS5_INSTALL_H

#include <limits.h>
#include <stddef.h>

#define NFS5_VERSION_TEXT "3.2"
#define NFS5_PORSCHE_EXE "Porsche.exe"
#define NFS5_DX7Z_DLL "Drivers/dx7z.dll"
#define NFS5_PORSCHE_SHA256 "96af10308b12840c7a0770ff87443a8516c3b756f34d2bc276988cc64f56b98c"
#define NFS5_DX7Z_SHA256 "0433cc93106603e9e0a81168e2fa44c40e3121a71b7ae9cd6f3b1f7551f3fc18"
#define NFS5_SHA256_HEX_SIZE 65

/* The port never executes the user's EXE/DLL (they are recompiled into the
 * binary); they are only version markers. Data from a CD image or an installed
 * folder is accepted, and the UI reports whether the exact v3.2 EXE+DLL pair
 * was found. NFS5_STRICT_VERSION=1 restores the old strict rejection. */

typedef struct {
    int installed;                /* 1 = <data> has a usable game tree */
    int verified;                 /* 1 = the exact translated v3.2 EXE+DLL hashes */
    int data_present;             /* 1 = a Porsche.exe exists in <data> (may be wrong version) */
    char data_dir[PATH_MAX];
    char porsche_sha256[NFS5_SHA256_HEX_SIZE];
    char dx7z_sha256[NFS5_SHA256_HEX_SIZE];
    char version[NFS5_SHA256_HEX_SIZE];
    char message[512];
} Nfs5InstallState;

/* Verify the game tree in @data_dir. Returns 0 when installed and verified,
 * 1 when files are missing, 2 when they exist but are the wrong version,
 * -1 on I/O errors. Fills @st in all cases (best effort). */
int nfs5_verify_data(const char *data_dir, Nfs5InstallState *st);

/* Find a candidate source game directory under <root>/source (depth-limited).
 * The directory must contain Porsche.exe and Drivers/dx7z.dll. Returns 0 and
 * writes the directory when found; 1 when no candidate exists; 2 when a
 * candidate exists but its hashes do not match. */
int nfs5_find_source_game(const char *root, char out_dir[PATH_MAX], char *msg, size_t msg_n);

/* Total number of regular files that nfs5_import() will copy (excludes
 * SKIP clutter). Used for progress display before the copy starts. */
int nfs5_count_import_files(const char *src_dir);

typedef int (*Nfs5ProgressFn)(const char *stage, int percent, void *user);

/* Copy the verified game tree @src_dir into <root>/data. The source is verified
 * again before copying. @progress may be NULL. Returns 0 on success, -1 on
 * error with @msg filled. */
int nfs5_import(const char *src_dir, const char *root, Nfs5ProgressFn progress, void *user,
                char *msg, size_t msg_n);

/* If <root>/source contains an image but no extracted game, unpack it natively
 * into <root>/source/.extracted: ZIP (miniz) or CD image .iso/.bin/.cue
 * (built-in ISO9660/Joliet reader). Returns 0 if an extracted game can now be
 * found, 1 if no image is present, -1 on failure. */
int nfs5_extract_source_image(const char *root, char *msg, size_t msg_n);
/* Backwards-compatible alias. */
int nfs5_extract_source_zip(const char *root, char *msg, size_t msg_n);

/* Remove <root>/source/.extracted after a successful import. */
void nfs5_cleanup_staging(const char *root);

#endif

/* unpack.h - native (no external tools) source image unpacking for the launcher.
 *
 * Supported user inputs under <root>/source/:
 *   - a ZIP of an installed game folder            nfs5_unpack_zip()
 *   - an .iso (ISO9660, 2048-byte sectors)         nfs5_unpack_iso()
 *   - a .bin (+ .cue) CD image                     nfs5_unpack_iso()
 * Everything is extracted with in-process code only: miniz for ZIP/deflate and
 * a small ISO9660/Joliet reader for CD images. No unzip/bsdtar/mount calls.
 */
#ifndef NFS5_UNPACK_H
#define NFS5_UNPACK_H

#include <stddef.h>

/* 0 on success; -1 on error with a human-readable message in msg. Creates
 * dest_dir (recursively) and rejects entries with absolute/.. paths. */
int nfs5_unpack_zip(const char *zip_path, const char *dest_dir, char *msg, size_t msg_n);

/* Handles .iso, .bin (with sibling .cue or auto-detected raw sectors) and
 * .cue. Extracts the ISO9660/Joliet tree into dest_dir. */
int nfs5_unpack_iso(const char *image_path, const char *dest_dir, char *msg, size_t msg_n);

/* Recursively remove path (files, symlinks, directories); no shell-out. */
int nfs5_rm_rf(const char *path);

#endif

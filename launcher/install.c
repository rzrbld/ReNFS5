/* Install/verify/import; see launcher/install.h. */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include "install.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "sha256.h"
#include "unpack.h"

#define SOURCE_DIR "source"
#define STAGING_DIR "source/.extracted"
#define MAX_SCAN_DEPTH 6
#define MAX_IMPORT_FILES 200000

static void set_msg(char *msg, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_msg(char *msg, size_t n, const char *fmt, ...) {
    if (!msg || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, n, fmt, ap);
    va_end(ap);
}

static int join(char *out, size_t n, const char *a, const char *b) {
    int r = snprintf(out, n, "%s/%s", a, b);
    return (r < 0 || (size_t)r >= n) ? -1 : 0;
}

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int read_version(const char *data_dir, char out[32]) {
    char path[PATH_MAX];
    if (join(path, sizeof path, data_dir, "version.txt")) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[64] = {0};
    if (!fgets(line, sizeof line, f)) { fclose(f); return -1; }
    fclose(f);
    size_t len = strcspn(line, "\r\n");
    line[len] = 0;
    snprintf(out, 32, "%s", line);
    return 0;
}

static int is_game_data_dir(const char *dir);

static int verify_data(const char *data_dir, Nfs5InstallState *st, int importing) {
    memset(st, 0, sizeof *st);
    snprintf(st->data_dir, sizeof st->data_dir, "%s", data_dir ? data_dir : "");
    if (!data_dir || !*data_dir) {
        snprintf(st->message, sizeof st->message, "no data directory");
        return 1;
    }
    char marker[PATH_MAX];
    if (join(marker, sizeof marker, data_dir, ".nfs5-importing") ||
        (!importing && access(marker, F_OK) == 0)) {
        snprintf(st->message, sizeof st->message, "previous import incomplete; retry from source/");
        return 1;
    }
    if (!is_game_data_dir(data_dir)) {
        snprintf(st->message, sizeof st->message, "missing or empty FEData/GameData directories");
        return 1;
    }
    char porsche[PATH_MAX], dx7z[PATH_MAX];
    if (join(porsche, sizeof porsche, data_dir, NFS5_PORSCHE_EXE) ||
        join(dx7z, sizeof dx7z, data_dir, NFS5_DX7Z_DLL)) {
        snprintf(st->message, sizeof st->message, "data path too long");
        return -1;
    }
    if (nfs5_sha256_file(porsche, st->porsche_sha256) < 0) {
        snprintf(st->message, sizeof st->message, "missing %s", NFS5_PORSCHE_EXE);
        return 1;
    }
    st->data_present = 1;
    if (nfs5_sha256_file(dx7z, st->dx7z_sha256) < 0) {
        snprintf(st->message, sizeof st->message, "missing %s", NFS5_DX7Z_DLL);
        return 1;
    }
    read_version(data_dir, st->version);
    int porsche_ok = !strcmp(st->porsche_sha256, NFS5_PORSCHE_SHA256);
    int dx7z_ok = !strcmp(st->dx7z_sha256, NFS5_DX7Z_SHA256);
    int version_ok = 1;
    if (st->version[0]) {
        if (strncmp(st->version, NFS5_VERSION_TEXT, strlen(NFS5_VERSION_TEXT))) version_ok = 0;
    }
    if (!porsche_ok || !dx7z_ok || !version_ok) {
        const char *strict = getenv("NFS5_STRICT_VERSION");
        int tolerant = is_game_data_dir(data_dir) && !(strict && *strict && strcmp(strict, "0") != 0);
        if (!tolerant) {
            snprintf(st->message, sizeof st->message,
                     "wrong game version: %s=%s %s=%s version=%s (need %s)",
                     NFS5_PORSCHE_EXE, st->porsche_sha256, NFS5_DX7Z_DLL, st->dx7z_sha256,
                     st->version[0] ? st->version : "?", NFS5_VERSION_TEXT);
            return 2;
        }
        st->installed = 1;
        st->verified = 0;
        snprintf(st->message, sizeof st->message,
                 "game data present; EXE/DLL differ from the translated v3.2 pair "
                 "(version=%s) - imported as-is, untested version",
                 st->version[0] ? st->version : "?");
        return 0;
    }
    st->installed = 1;
    st->verified = 1;
    snprintf(st->message, sizeof st->message, "game files verified (v%s)", NFS5_VERSION_TEXT);
    return 0;
}

int nfs5_verify_data(const char *data_dir, Nfs5InstallState *st) {
    return verify_data(data_dir, st, 0);
}

/* Case-insensitive existence test for a file/dir name in dir (ISO9660 names are
 * upper-case, installed copies are mixed case). */
static int has_name_ci(const char *dir, const char *want) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d))) {
        if (!strcasecmp(e->d_name, want)) {
            char path[PATH_MAX];
            DIR *child = NULL;
            if (!join(path, sizeof path, dir, e->d_name)) child = opendir(path);
            if (child) {
                struct dirent *item;
                while ((item = readdir(child))) {
                    if (strcmp(item->d_name, ".") && strcmp(item->d_name, "..")) { found = 1; break; }
                }
                closedir(child);
            }
            break;
        }
    }
    closedir(d);
    return found;
}

static int is_game_data_dir(const char *dir) {
    return has_name_ci(dir, "FEData") && has_name_ci(dir, "GameData");
}

/* 1 = exact translated v3.2 pair; 2 = looks like an installed game but the
 * EXE/DLL differ (CD/patch/nocd); 0 = not a candidate; -1 = wrong files. */
static int candidate_valid(const char *dir, int *exact, int *installed_like) {
    char porsche[PATH_MAX], dx7z[PATH_MAX], hex[NFS5_SHA256_HEX_SIZE];
    int have_exe = join(porsche, sizeof porsche, dir, NFS5_PORSCHE_EXE) == 0 &&
                   nfs5_sha256_file(porsche, hex) == 0;
    int exe_ok = have_exe && !strcmp(hex, NFS5_PORSCHE_SHA256);
    int have_dll = join(dx7z, sizeof dx7z, dir, NFS5_DX7Z_DLL) == 0 &&
                   nfs5_sha256_file(dx7z, hex) == 0;
    int dll_ok = have_dll && !strcmp(hex, NFS5_DX7Z_SHA256);
    if (!is_game_data_dir(dir)) return 0;
    if (exe_ok && dll_ok) { *exact = 1; *installed_like = 0; return 1; }
    if (have_exe && have_dll && is_game_data_dir(dir)) {
        *exact = 0; *installed_like = 1;
        return 1;
    }
    if (have_exe || have_dll) return -1;
    return 0;
}

static int scan_source(const char *dir, int depth, char out[PATH_MAX], char wrong[PATH_MAX],
                       int *wrong_found, int *unverified) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    /* Check this directory first: shortest path wins. */
    int exact = 0, installed_like = 0;
    int c = candidate_valid(dir, &exact, &installed_like);
    if (c == 1) {
        snprintf(out, PATH_MAX, "%s", dir);
        if (installed_like) *unverified = 1;
        closedir(d);
        return 1;
    }
    if (c == -1 && !*wrong_found) {
        snprintf(wrong, PATH_MAX, "%s", dir);
        *wrong_found = 1;
    }
    if (depth >= MAX_SCAN_DEPTH) { closedir(d); return 0; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char child[PATH_MAX];
        if (join(child, sizeof child, dir, e->d_name) || !is_dir(child)) continue;
        if (scan_source(child, depth + 1, out, wrong, wrong_found, unverified)) { closedir(d); return 1; }
    }
    closedir(d);
    return 0;
}

int nfs5_find_source_game(const char *root, char out_dir[PATH_MAX], char *msg, size_t msg_n) {
    char src[PATH_MAX];
    if (join(src, sizeof src, root, SOURCE_DIR)) {
        set_msg(msg, msg_n, "source path too long");
        return 1;
    }
    char wrong[PATH_MAX] = {0};
    int wrong_found = 0, unverified = 0;
    /* Unpacked images live in the hidden staging dir, which the directory
     * walk skips on purpose; scan it as its own root. */
    char stage[PATH_MAX];
    if (!join(stage, sizeof stage, root, STAGING_DIR) &&
        scan_source(stage, 0, out_dir, wrong, &wrong_found, &unverified)) {
        set_msg(msg, msg_n, unverified
                ? "found game data in unpacked source (EXE/DLL not the translated v3.2 pair; importing as-is)"
                : "found game files in unpacked source");
        return 0;
    }
    if (scan_source(src, 0, out_dir, wrong, &wrong_found, &unverified)) {
        set_msg(msg, msg_n, unverified
                ? "found game data in source/ (EXE/DLL are not the translated v3.2 pair; importing as-is)"
                : "found game files in source/");
        return 0;
    }
    if (wrong_found) {
        set_msg(msg, msg_n,
                "source contains a different game version (checked %s); need NFS5 v%s",
                wrong, NFS5_VERSION_TEXT);
        return 2;
    }
    set_msg(msg, msg_n, "no game files under %s", src);
    return 1;
}

/* Find a user-supplied image under source/: .iso, .bin/.cue, or .zip (in that
 * order of preference). Returns 0 and the absolute path when found. */
static int has_image(const char *src, char out[PATH_MAX], int *is_iso) {
    DIR *d = opendir(src);
    if (!d) return 0;
    struct dirent *e;
    char iso[PATH_MAX] = {0}, bin[PATH_MAX] = {0}, cue[PATH_MAX] = {0}, zip[PATH_MAX] = {0};
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (e->d_name[0] == '.') continue;
        char path[PATH_MAX];
        if (join(path, sizeof path, src, e->d_name)) continue;
        if (len > 4 && !strcasecmp(e->d_name + len - 4, ".iso")) snprintf(iso, sizeof iso, "%s", path);
        else if (len > 4 && !strcasecmp(e->d_name + len - 4, ".mds")) { if (!cue[0]) snprintf(cue, sizeof cue, "%s", path); }
        else if (len > 4 && !strcasecmp(e->d_name + len - 4, ".mdf")) snprintf(bin, sizeof bin, "%s", path);
        else if (len > 4 && !strcasecmp(e->d_name + len - 4, ".bin")) snprintf(bin, sizeof bin, "%s", path);
        else if (len > 4 && !strcasecmp(e->d_name + len - 4, ".cue")) snprintf(cue, sizeof cue, "%s", path);
        else if (len > 4 && !strcasecmp(e->d_name + len - 4, ".zip")) snprintf(zip, sizeof zip, "%s", path);
    }
    closedir(d);
    if (iso[0]) { snprintf(out, PATH_MAX, "%s", iso); *is_iso = 1; return 1; }
    if (cue[0]) { snprintf(out, PATH_MAX, "%s", cue); *is_iso = 1; return 1; }
    if (bin[0]) { snprintf(out, PATH_MAX, "%s", bin); *is_iso = 1; return 1; }
    if (zip[0]) { snprintf(out, PATH_MAX, "%s", zip); *is_iso = 0; return 1; }
    return 0;
}

int nfs5_extract_source_image(const char *root, char *msg, size_t msg_n) {
    char src[PATH_MAX], image[PATH_MAX], stage[PATH_MAX];
    if (join(src, sizeof src, root, SOURCE_DIR) ||
        join(stage, sizeof stage, root, STAGING_DIR)) {
        set_msg(msg, msg_n, "source path too long");
        return -1;
    }
    int is_iso = 0;
    if (!has_image(src, image, &is_iso)) return 1;   /* no image: caller explains */
    if (mkdir(stage, 0755) && errno != EEXIST) {
        set_msg(msg, msg_n, "cannot create %s: %s", stage, strerror(errno));
        return -1;
    }
    set_msg(msg, msg_n, "unpacking %s", image);
    int rc;
    if (is_iso) rc = nfs5_unpack_iso(image, stage, msg, msg_n);
    else        rc = nfs5_unpack_zip(image, stage, msg, msg_n);
    if (rc != 0) return -1;
    char found[PATH_MAX], wrong[PATH_MAX] = {0};
    int wrong_found = 0, unverified = 0;
    if (scan_source(stage, 0, found, wrong, &wrong_found, &unverified)) {
        set_msg(msg, msg_n, "unpacked %s", image);
        return 0;
    }
    set_msg(msg, msg_n,
            "%s did not contain an installed NFS5 v%s game (looked for %s, %s and the game data dirs). "
            "If this is a CD image that only holds the game installer, install the game on a PC (or use "
            "an archive of an installed copy) and put that folder here instead.",
            image, NFS5_VERSION_TEXT, NFS5_PORSCHE_EXE, NFS5_DX7Z_DLL);
    return -1;
}

int nfs5_extract_source_zip(const char *root, char *msg, size_t msg_n) {
    return nfs5_extract_source_image(root, msg, msg_n);
}

void nfs5_cleanup_staging(const char *root) {
    char stage[PATH_MAX];
    if (join(stage, sizeof stage, root, STAGING_DIR)) return;
    nfs5_rm_rf(stage);   /* native recursive delete: no shell-out */
}

/* ---- import ------------------------------------------------------------ */

static int skip_name(const char *name, int depth) {
    if (depth == 0 && (!strcasecmp(name, "Support") || !strcasecmp(name, "Ereg") ||
                       !strcasecmp(name, "Uninstall")))
        return 1;
    size_t len = strlen(name);
    if (len > 4) {
        const char *ext = name + len - 4;
        if (!strcasecmp(ext, ".zip") || !strcasecmp(ext, ".iso") || !strcasecmp(ext, ".rar") ||
            !strcasecmp(ext, ".7z"))
            return 1;
    }
    return 0;
}

static int walk_count(const char *dir, int depth, int *count, char *msg, size_t msg_n) {
    DIR *d = opendir(dir);
    if (!d) {
        set_msg(msg, msg_n, "cannot read %s: %s", dir, strerror(errno));
        return -1;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (skip_name(e->d_name, depth)) continue;
        char path[PATH_MAX];
        if (join(path, sizeof path, dir, e->d_name)) { closedir(d); return -1; }
        struct stat st;
        if (lstat(path, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            if (walk_count(path, depth + 1, count, msg, msg_n)) { closedir(d); return -1; }
        } else if (S_ISREG(st.st_mode)) {
            if (++*count > MAX_IMPORT_FILES) {
                set_msg(msg, msg_n, "too many files in source tree (>%d)", MAX_IMPORT_FILES);
                closedir(d);
                return -1;
            }
        }
    }
    closedir(d);
    return 0;
}

typedef struct {
    int total, done;
    int last_pct;
    Nfs5ProgressFn fn;
    void *user;
} CopyState;

static void report(CopyState *cs, const char *stage) {
    if (!cs->fn) return;
    int pct = cs->total > 0 ? (int)((int64_t)cs->done * 100 / cs->total) : 100;
    if (pct == cs->last_pct && strcmp(stage, "DONE")) return;
    cs->last_pct = pct;
    cs->fn(stage, pct, cs->user);
}

static int copy_tree(const char *src, const char *dst, int depth, CopyState *cs, char *msg, size_t msg_n) {
    if (mkdir(dst, 0755) && errno != EEXIST) {
        set_msg(msg, msg_n, "cannot create %s: %s", dst, strerror(errno));
        return -1;
    }
    DIR *d = opendir(src);
    if (!d) {
        set_msg(msg, msg_n, "cannot read %s: %s", src, strerror(errno));
        return -1;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (skip_name(e->d_name, depth)) continue;
        char sp[PATH_MAX], dp[PATH_MAX];
        if (join(sp, sizeof sp, src, e->d_name) || join(dp, sizeof dp, dst, e->d_name)) {
            closedir(d);
            return -1;
        }
        struct stat st;
        if (lstat(sp, &st)) continue;
        if (S_ISLNK(st.st_mode)) continue; /* never follow links out of the source tree */
        if (S_ISDIR(st.st_mode)) {
            if (copy_tree(sp, dp, depth + 1, cs, msg, msg_n)) { closedir(d); return -1; }
            continue;
        }
        if (!S_ISREG(st.st_mode)) continue;
        struct stat dstat;
        if (stat(dp, &dstat) == 0 && dstat.st_size == st.st_size &&
            dstat.st_mtime >= st.st_mtime) {
            cs->done++;
            report(cs, e->d_name);
            continue;
        }
        FILE *in = fopen(sp, "rb");
        if (!in) {
            set_msg(msg, msg_n, "cannot read %s: %s", sp, strerror(errno));
            closedir(d);
            return -1;
        }
        FILE *out = fopen(dp, "wb");
        if (!out) {
            set_msg(msg, msg_n, "cannot write %s: %s", dp, strerror(errno));
            fclose(in);
            closedir(d);
            return -1;
        }
        char buf[1 << 16];
        size_t n;
        int failed = 0;
        while ((n = fread(buf, 1, sizeof buf, in)) > 0)
            if (fwrite(buf, 1, n, out) != n) { failed = 1; break; }
        if (ferror(in)) failed = 1;
        if (fclose(in)) failed = 1;
        if (fclose(out)) failed = 1;
        if (failed) {
            set_msg(msg, msg_n, "copy failed for %s", sp);
            closedir(d);
            return -1;
        }
        chmod(dp, (st.st_mode & 0111) ? 0755 : 0644);
        struct timespec times[2] = {st.st_atim, st.st_mtim};
        utimensat(AT_FDCWD, dp, times, 0);
        cs->done++;
        report(cs, e->d_name);
    }
    closedir(d);
    return 0;
}

static int write_manifest(const char *root, const Nfs5InstallState *st, int files) {
    char dir[PATH_MAX], path[PATH_MAX], temp[PATH_MAX];
    if (join(dir, sizeof dir, root, "config")) return -1;
    if (mkdir(dir, 0755) && errno != EEXIST) return -1;
    if (join(path, sizeof path, dir, "install.json") ||
        snprintf(temp, sizeof temp, "%s.tmp", path) < 0)
        return -1;
    FILE *f = fopen(temp, "w");
    if (!f) return -1;
    fprintf(f,
            "{\n"
            "  \"format\": 1,\n"
            "  \"version\": \"%s\",\n"
            "  \"data\": \"data\",\n"
            "  \"porsche_exe_sha256\": \"%s\",\n"
            "  \"dx7z_dll_sha256\": \"%s\",\n"
            "  \"files\": %d\n"
            "}\n",
            NFS5_VERSION_TEXT, st->porsche_sha256, st->dx7z_sha256, files);
    int failed = fflush(f) != 0;
    if (fclose(f)) failed = 1;
    if (failed) { unlink(temp); return -1; }
    if (rename(temp, path)) { unlink(temp); return -1; }
    return 0;
}

int nfs5_count_import_files(const char *src_dir) {
    int count = 0;
    char msg[128];
    if (walk_count(src_dir, 0, &count, msg, sizeof msg)) return -1;
    return count;
}

int nfs5_import(const char *src_dir, const char *root, Nfs5ProgressFn progress, void *user,
                char *msg, size_t msg_n) {
    char porsche[PATH_MAX], dx7z[PATH_MAX], hex[NFS5_SHA256_HEX_SIZE];
    if (join(porsche, sizeof porsche, src_dir, NFS5_PORSCHE_EXE) ||
        join(dx7z, sizeof dx7z, src_dir, NFS5_DX7Z_DLL)) {
        set_msg(msg, msg_n, "source path too long");
        return -1;
    }
    if (!is_game_data_dir(src_dir)) {
        set_msg(msg, msg_n, "source needs non-empty FEData and GameData directories");
        return -1;
    }
    if (progress) progress("VERIFYING SOURCE", 0, user);
    char exe_sha[NFS5_SHA256_HEX_SIZE] = {0}, dll_sha[NFS5_SHA256_HEX_SIZE] = {0};
    if (nfs5_sha256_file(porsche, exe_sha) < 0) {
        set_msg(msg, msg_n, "source is missing %s", NFS5_PORSCHE_EXE);
        return -1;
    }
    if (nfs5_sha256_file(dx7z, dll_sha) < 0) {
        set_msg(msg, msg_n, "source is missing %s", NFS5_DX7Z_DLL);
        return -1;
    }
    int exact = !strcmp(exe_sha, NFS5_PORSCHE_SHA256) && !strcmp(dll_sha, NFS5_DX7Z_SHA256);
    const char *strict = getenv("NFS5_STRICT_VERSION");
    if (!exact && !(is_game_data_dir(src_dir) && !(strict && *strict && strcmp(strict, "0") != 0))) {
        set_msg(msg, msg_n,
                "unsupported game version: %s sha256 %s, %s sha256 %s (need the translated v%s pair, "
                "or a folder that also contains the game data)",
                NFS5_PORSCHE_EXE, exe_sha, NFS5_DX7Z_DLL, dll_sha, NFS5_VERSION_TEXT);
        return -1;
    }
    char data[PATH_MAX], stage[PATH_MAX];
    if (join(data, sizeof data, root, "data")) {
        set_msg(msg, msg_n, "install path too long");
        return -1;
    }
    char stage_msg[64];
    snprintf(stage_msg, sizeof stage_msg, "COUNTING FILES");
    if (progress) progress(stage_msg, 0, user);
    int total = 0;
    if (walk_count(src_dir, 0, &total, msg, msg_n)) return -1;
    if (total <= 0) {
        set_msg(msg, msg_n, "source tree has no files");
        return -1;
    }
    /* Leave a durable incomplete marker until verification and manifest commit.
     * Cancellation or a failed copy must not turn a partial tree into an install. */
    if ((mkdir(data, 0755) && errno != EEXIST) ||
        join(stage, sizeof stage, data, ".nfs5-importing")) return -1;
    int marker_fd = open(stage, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (marker_fd < 0) { set_msg(msg, msg_n, "cannot mark import incomplete: %s", strerror(errno)); return -1; }
    int sync_error = fsync(marker_fd);
    close(marker_fd);
    if (sync_error) { set_msg(msg, msg_n, "cannot sync import marker"); return -1; }
    CopyState cs = {.total = total, .done = 0, .last_pct = -1, .fn = progress, .user = user};
    report(&cs, "COPYING GAME FILES");
    if (copy_tree(src_dir, data, 0, &cs, msg, msg_n)) return -1;
    if (progress) progress("VERIFYING INSTALL", 100, user);
    Nfs5InstallState st;
    int rc = verify_data(data, &st, 1);
    if (rc != 0) {
        set_msg(msg, msg_n, "copied files but verification failed: %s", st.message);
        return -1;
    }
    if (write_manifest(root, &st, total)) {
        set_msg(msg, msg_n, "cannot write config/install.json: %s", strerror(errno));
        return -1;
    }
    if (unlink(stage)) { set_msg(msg, msg_n, "cannot finish import: %s", strerror(errno)); return -1; }
    if (progress) progress("INSTALL COMPLETE", 100, user);
    set_msg(msg, msg_n, "installed %d files; %s", total,
            st.verified ? "verified v" NFS5_VERSION_TEXT : "unverified version (data imported as-is)");
    return 0;
}

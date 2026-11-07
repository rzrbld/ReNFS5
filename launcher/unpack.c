/* unpack.c - see unpack.h. Launcher-only, no external processes. */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include "unpack.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "miniz.h"

static void set_msg(char *msg, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_msg(char *msg, size_t n, const char *fmt, ...) {
    if (!msg || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, n, fmt, ap);
    va_end(ap);
}

/* ---- small filesystem helpers ---- */

static int mkdir_p(const char *path) {
    char tmp[4096];
    size_t len = strlen(path);
    if (!len || len >= sizeof tmp) return -1;
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
    return 0;
}

static int join_path(char *out, size_t n, const char *a, const char *b) {
    int r = snprintf(out, n, "%s/%s", a, b);
    return (r < 0 || (size_t)r >= n) ? -1 : 0;
}

/* Reject absolute paths, ".." components and empty names: a malicious archive
 * must not write outside the staging directory. */
static int safe_rel_path(const char *name) {
    if (!name || !*name || name[0] == '/' || name[0] == '\\') return 0;
    const char *p = name;
    while (*p) {
        while (*p == '/') p++;
        const char *start = p;
        while (*p && *p != '/' && *p != '\\') p++;
        size_t len = (size_t)(p - start);
        if (len == 2 && start[0] == '.' && start[1] == '.') return 0;
        if (len == 0) break;
    }
    return 1;
}

static int mkdirs_for_file(const char *dest_dir, const char *rel) {
    char path[4096];
    if (join_path(path, sizeof path, dest_dir, rel)) return -1;
    char *slash = strrchr(path, '/');
    if (!slash) return 0;
    *slash = 0;
    return mkdir_p(path);
}

int nfs5_rm_rf(const char *path) {
    struct stat st;
    if (!path || !*path || lstat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0 ? 0 : -1;
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[4096];
        if (join_path(child, sizeof child, path, e->d_name)) continue;
        nfs5_rm_rf(child);
    }
    closedir(d);
    return rmdir(path) == 0 ? 0 : -1;
}

/* ---- ZIP (miniz) ---- */

int nfs5_unpack_zip(const char *zip_path, const char *dest_dir, char *msg, size_t msg_n) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof zip);
    if (!mz_zip_reader_init_file(&zip, zip_path, 0)) {
        set_msg(msg, msg_n, "not a readable ZIP: %s", zip_path);
        return -1;
    }
    if (mkdir_p(dest_dir)) {
        mz_zip_reader_end(&zip);
        set_msg(msg, msg_n, "cannot create %s", dest_dir);
        return -1;
    }
    mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; i++) {
        char name[1024];
        if (!mz_zip_reader_get_filename(&zip, i, name, sizeof name)) continue;
        /* The archive may wrap the game in a top folder; keep the whole tree. */
        if (!safe_rel_path(name)) {
            mz_zip_reader_end(&zip);
            set_msg(msg, msg_n, "ZIP contains an unsafe path: %s", name);
            return -1;
        }
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        char out[4096];
        if (join_path(out, sizeof out, dest_dir, name)) continue;
        if (st.m_is_directory) {
            mkdir_p(out);
            continue;
        }
        if (mkdirs_for_file(dest_dir, name)) continue;
        if (!mz_zip_reader_extract_to_file(&zip, i, out, 0)) {
            mz_zip_reader_end(&zip);
            set_msg(msg, msg_n, "ZIP entry failed to extract: %s", name);
            return -1;
        }
    }
    mz_uint total = n;
    mz_zip_reader_end(&zip);
    set_msg(msg, msg_n, "extracted %u ZIP entries", total);
    return 0;
}

/* ---- CD images: ISO9660 (+Joliet), 2048 or raw 2352-byte sectors ---- */

typedef struct {
    FILE *f;
    uint64_t size;
    int sector_size;    /* 2048 or 2352 */
    int data_off;       /* byte offset of the 2048 user bytes in a sector */
} iso_t;

static int iso_open_iso(iso_t *iso, const char *path, int sector_size_hint) {
    memset(iso, 0, sizeof *iso);
    iso->f = fopen(path, "rb");
    if (!iso->f) return -1;
    if (fseeko(iso->f, 0, SEEK_END) != 0) { fclose(iso->f); return -1; }
    iso->size = (uint64_t)ftello(iso->f);
    if (fseeko(iso->f, 0, SEEK_SET) != 0) { fclose(iso->f); return -1; }
    int s2352 = (iso->size % 2352 == 0);
    int s2048 = (iso->size % 2048 == 0);
    if (sector_size_hint == 2352) { iso->sector_size = 2352; }
    else if (sector_size_hint == 2048) { iso->sector_size = 2048; }
    else if (s2352 && !s2048) iso->sector_size = 2352;
    else if (s2352 && s2048 && iso->size > 4096 && (iso->size % 4096 != 0)) iso->sector_size = 2352;
    else iso->sector_size = 2048;
    if (iso->sector_size == 2352) iso->data_off = 16;   /* MODE1 raw sector */
    return 0;
}

static int iso_read_sector(iso_t *iso, uint32_t lba, uint8_t out[2048]) {
    uint64_t off = (uint64_t)lba * (uint64_t)iso->sector_size + (uint64_t)iso->data_off;
    if (off + 2048 > iso->size) return -1;
    if (fseeko(iso->f, (off_t)off, SEEK_SET) != 0) return -1;
    return fread(out, 1, 2048, iso->f) == 2048 ? 0 : -1;
}

/* Parse a cue sheet for the first data track: file name, sector size, mode. */
static int iso_parse_cue(const char *cue_path, char *bin_out, size_t bin_n,
                         int *sector_size, int *data_off) {
    FILE *f = fopen(cue_path, "r");
    if (!f) return -1;
    char line[1024];
    char cue_dir[4096];
    snprintf(cue_dir, sizeof cue_dir, "%s", cue_path);
    char *slash = strrchr(cue_dir, '/');
    if (slash) *slash = 0; else snprintf(cue_dir, sizeof cue_dir, ".");
    int got = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!strncasecmp(p, "FILE ", 5)) {
            char name[1024] = {0};
            if (sscanf(p + 5, "\"%1023[^\"]\"", name) != 1) {
                if (sscanf(p + 5, "%1023s", name) != 1) continue;
            }
            if (join_path(bin_out, bin_n, cue_dir, name)) { fclose(f); return -1; }
        } else if (!strncasecmp(p, "TRACK ", 6)) {
            int track = 0;
            char mode[64] = {0};
            if (sscanf(p + 6, "%d %63s", &track, mode) == 2) {
                if (!strcasecmp(mode, "MODE1/2352")) { *sector_size = 2352; *data_off = 16; got = 1; }
                else if (!strcasecmp(mode, "MODE2/2352")) { *sector_size = 2352; *data_off = 24; got = 1; }
                else if (!strcasecmp(mode, "MODE1/2048")) { *sector_size = 2048; *data_off = 0; got = 1; }
                else if (!strcasecmp(mode, "AUDIO")) continue;
                if (got) { fclose(f); return 0; }
            }
        }
    }
    fclose(f);
    return got ? 0 : -1;
}

/* Alcohol 120% .mds descriptors are a small text/binary file naming the .mdf
 * and the track mode. Scan for the quoted FILE name and a MODE n/xxxx string. */
static int iso_parse_mds(const char *mds_path, char *bin_out, size_t bin_n,
                         int *sector_size, int *data_off) {
    FILE *f = fopen(mds_path, "rb");
    if (!f) return -1;
    size_t cap = 1 << 20, n = 0;
    char *buf = (char *)malloc(cap + 1);
    if (!buf) { fclose(f); return -1; }
    n = fread(buf, 1, cap, f);
    fclose(f);
    buf[n] = 0;
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", mds_path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0; else snprintf(dir, sizeof dir, ".");
    *sector_size = 0; *data_off = 0; bin_out[0] = 0;
    for (size_t i = 0; i + 5 < n; i++) {
        if (!strncasecmp(buf + i, "FILE", 4)) {
            const char *q = strchr(buf + i + 4, '"');
            const char *e = q ? strchr(q + 1, '"') : NULL;
            if (q && e && (size_t)(e - q - 1) < bin_n - 1) {
                memcpy(bin_out, q + 1, (size_t)(e - q - 1));
                bin_out[e - q - 1] = 0;
            }
        } else if (!strncasecmp(buf + i, "MODE1/2048", 10)) { *sector_size = 2048; *data_off = 0; }
        else if (!strncasecmp(buf + i, "MODE1/2352", 10)) { *sector_size = 2352; *data_off = 16; }
        else if (!strncasecmp(buf + i, "MODE2/2352", 10)) { *sector_size = 2352; *data_off = 24; }
    }
    if (!bin_out[0]) {
        /* no FILE section: assume the .mdf next to the .mds */
        snprintf(bin_out, bin_n, "%s", mds_path);
        size_t l = strlen(bin_out);
        if (l > 4) memcpy(bin_out + l - 4, ".mdf", 5);
    }
    char resolved[4096];
    if (bin_out[0] != '/' && join_path(resolved, sizeof resolved, dir, bin_out) == 0)
        snprintf(bin_out, bin_n, "%s", resolved);
    free(buf);
    return (*sector_size || bin_out[0]) ? 0 : -1;
}

static void joliet_name(const uint8_t *raw, size_t len, char *out, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i + 1 < len && o + 4 < n; i += 2) {
        uint32_t c = ((uint32_t)raw[i] << 8) | raw[i + 1];
        if (c == 0 || c == ';' ) break;
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xC0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 0x3F)); }
        else { out[o++] = (char)(0xE0 | (c >> 12)); out[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[o] = 0;
}

static void ascii_name(const uint8_t *raw, size_t len, char *out, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < n; i++) {
        char c = (char)raw[i];
        if (c == ';') break;
        out[o++] = c;
    }
    while (o && out[o - 1] == '.') o--;
    out[o] = 0;
}

typedef struct { uint32_t lba, size; int joliet; } iso_dirrec;

static int iso_walk(iso_t *iso, uint32_t lba, uint32_t size, int joliet,
                    const char *dest, const char *rel, int depth) {
    if (depth > 24) return -1;
    char myrel[4096];
    snprintf(myrel, sizeof myrel, "%s", rel);
    if (*myrel) {
        char dir[4096];
        if (join_path(dir, sizeof dir, dest, myrel)) return -1;
        if (mkdir_p(dir)) return -1;
    }
    uint8_t *dirbuf = (uint8_t *)malloc(size ? size : 2048);
    if (!dirbuf) return -1;
    size_t got = 0;
    for (uint32_t s = 0; s < (size + 2047) / 2048; s++) {
        uint8_t sec[2048];
        if (iso_read_sector(iso, lba + s, sec)) { free(dirbuf); return -1; }
        size_t chunk = size - got;
        if (chunk > 2048) chunk = 2048;
        memcpy(dirbuf + got, sec, chunk);
        got += chunk;
    }
    size_t off = 0;
    while (off + 33 <= got) {
        uint8_t len = dirbuf[off];
        if (len == 0) { off = ((off / 2048) + 1) * 2048; continue; }
        if (off + len > got) break;
        const uint8_t *r = dirbuf + off;
        uint32_t elba = (uint32_t)r[2] | ((uint32_t)r[3] << 8) | ((uint32_t)r[4] << 16) | ((uint32_t)r[5] << 24);
        uint32_t esize = (uint32_t)r[10] | ((uint32_t)r[11] << 8) | ((uint32_t)r[12] << 16) | ((uint32_t)r[13] << 24);
        uint8_t flags = r[25];
        uint8_t nlen = r[32];
        const uint8_t *nm = r + 33;
        char name[512] = {0};
        if (nlen == 1 && nm[0] == 0) { off += len; continue; }
        if (nlen == 1 && nm[0] == 1) { off += len; continue; }
        if (joliet) joliet_name(nm, nlen, name, sizeof name);
        else ascii_name(nm, nlen, name, sizeof name);
        if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..")) { off += len; continue; }
        char childrel[4096];
        if (*myrel) snprintf(childrel, sizeof childrel, "%s/%s", myrel, name);
        else snprintf(childrel, sizeof childrel, "%s", name);
        char out[4096];
        if (join_path(out, sizeof out, dest, childrel)) { off += len; continue; }
        if (flags & 2) {
            if (iso_walk(iso, elba, esize, joliet, dest, childrel, depth + 1)) { free(dirbuf); return -1; }
        } else {
            if (mkdirs_for_file(dest, childrel)) { off += len; continue; }
            FILE *w = fopen(out, "wb");
            if (!w) { free(dirbuf); return -1; }
            uint64_t left = esize;
            uint32_t cur = elba;
            uint8_t sec[2048];
            while (left) {
                if (iso_read_sector(iso, cur++, sec)) { fclose(w); free(dirbuf); return -1; }
                size_t chunk = left > 2048 ? 2048 : (size_t)left;
                if (fwrite(sec, 1, chunk, w) != chunk) { fclose(w); free(dirbuf); return -1; }
                left -= chunk;
            }
            fclose(w);
        }
        off += len;
    }
    free(dirbuf);
    return 0;
}

int nfs5_unpack_iso(const char *image_path, const char *dest_dir, char *msg, size_t msg_n) {
    char bin[4096];
    int sector_size = 0, data_off = 0;
    snprintf(bin, sizeof bin, "%s", image_path);
    size_t len = strlen(bin);
    if (len > 4 && !strcasecmp(bin + len - 4, ".mds")) {
        if (iso_parse_mds(image_path, bin, sizeof bin, &sector_size, &data_off)) {
            set_msg(msg, msg_n, "cannot parse MDS descriptor %s", image_path);
            return -1;
        }
    } else if (len > 4 && !strcasecmp(bin + len - 4, ".mdf")) {
        char mds[4096];
        snprintf(mds, sizeof mds, "%s", image_path);
        size_t l2 = strlen(mds);
        if (l2 > 4) {
            memcpy(mds + l2 - 4, ".mds", 5);
            if (iso_parse_mds(mds, bin, sizeof bin, &sector_size, &data_off)) {
                snprintf(bin, sizeof bin, "%s", image_path);   /* sibling .mds missing */
                sector_size = 0;
            }
        }
    } else if (len > 4 && !strcasecmp(bin + len - 4, ".cue")) {
        if (iso_parse_cue(image_path, bin, sizeof bin, &sector_size, &data_off)) {
            set_msg(msg, msg_n, "cannot parse cue sheet %s", image_path);
            return -1;
        }
    } else if (len > 4 && !strcasecmp(bin + len - 4, ".bin")) {
        snprintf(bin, sizeof bin, "%s", image_path);
        char cue[4096];
        snprintf(cue, sizeof cue, "%s", image_path);
        size_t l = strlen(cue);
        if (l > 4) { memcpy(cue + l - 4, ".cue", 5);
            if (iso_parse_cue(cue, bin, sizeof bin, &sector_size, &data_off)) sector_size = 0; }
    }
    iso_t iso;
    if (iso_open_iso(&iso, bin, sector_size)) {
        set_msg(msg, msg_n, "cannot open CD image %s", bin);
        return -1;
    }
    if (sector_size == 2352) iso.sector_size = 2352;
    if (data_off) iso.data_off = data_off;
    if (mkdir_p(dest_dir)) { fclose(iso.f); set_msg(msg, msg_n, "cannot create %s", dest_dir); return -1; }
    uint32_t root_lba = 0, root_size = 0;
    int joliet = 0, found = 0, have_joliet = 0;
    uint8_t sec[2048];
    for (uint32_t lba = 16; lba < 16 + 64; lba++) {
        if (iso_read_sector(&iso, lba, sec)) break;
        uint8_t type = sec[0];
        if (type == 255) break;
        if (!memcmp(sec + 1, "CD001", 5)) {
            if (type == 1 && !found) {
                root_lba = (uint32_t)sec[156 + 2] | ((uint32_t)sec[156 + 3] << 8) |
                           ((uint32_t)sec[156 + 4] << 16) | ((uint32_t)sec[156 + 5] << 24);
                root_size = (uint32_t)sec[156 + 10] | ((uint32_t)sec[156 + 11] << 8) |
                            ((uint32_t)sec[156 + 12] << 16) | ((uint32_t)sec[156 + 13] << 24);
                found = 1;
            } else if (type == 2) {
                uint8_t *esc = sec + 88;
                if (!memcmp(esc, "%/@", 3) || !memcmp(esc, "%/C", 3) || !memcmp(esc, "%/E", 3)) {
                    root_lba = (uint32_t)sec[156 + 2] | ((uint32_t)sec[156 + 3] << 8) |
                               ((uint32_t)sec[156 + 4] << 16) | ((uint32_t)sec[156 + 5] << 24);
                    root_size = (uint32_t)sec[156 + 10] | ((uint32_t)sec[156 + 11] << 8) |
                                ((uint32_t)sec[156 + 12] << 16) | ((uint32_t)sec[156 + 13] << 24);
                    joliet = 1;
                    have_joliet = 1;
                }
            }
        }
    }
    (void)have_joliet;
    if (!found) { fclose(iso.f); set_msg(msg, msg_n, "no ISO9660 volume in %s", bin); return -1; }
    int rc = iso_walk(&iso, root_lba, root_size, joliet, dest_dir, "", 0);
    fclose(iso.f);
    if (rc) { set_msg(msg, msg_n, "ISO9660 extraction failed (%s)", bin); return -1; }
    set_msg(msg, msg_n, "extracted CD image %s (%s sectors)", bin, iso.sector_size == 2352 ? "2352-byte" : "2048-byte");
    return 0;
}

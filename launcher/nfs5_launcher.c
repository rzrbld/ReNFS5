/* NFS5 port launcher and installer UI.
 *
 * Native C + SDL2 only: no Python, no SDL2_ttf, no bundled fonts. It runs on
 * the handheld before the game and:
 *   1. verifies the user's original NFS5 v3.2 files (SHA-256),
 *   2. imports an extracted game folder or ZIP from <root>/source into
 *      <root>/data (the package itself contains no game assets),
 *   3. edits graphics/FPS/controller settings persisted in <root>/config,
 *   4. exits with status 0 when the Ports script should start the game.
 *
 * The retro blue menu style and bitmap-font idea are adapted from the MIT
 * Desert Storm / The ReThing port launchers by the same porter; see
 * docs/packaging/LICENSE.md.
 *
 * Headless use (automation, CI):
 *   nfs5-launcher --root DIR --verify      # 0 installed, 3 files missing, 2 wrong version
 *   nfs5-launcher --root DIR --selftest    # launcher/configuration self tests
 *   nfs5-launcher --root DIR --import      # headless import from source/
 *   nfs5-launcher --root DIR --screenshot out.bmp [--page settings|install|padmap|rebind|importing|check|error]
 */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <SDL.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "configuration.h"
#include "install.h"
#include "unpack.h"
#include "sha256.h"

#define W 640
#define H 480
#define FONT_W 5
#define FONT_H 7
#define MAX_STATUS_MSG 512

/* ------------------------------------------------------------------ paths */

static char g_root[PATH_MAX];
static char g_data[PATH_MAX];
static char g_config[PATH_MAX];
static char g_userdata[PATH_MAX];
static int g_data_verified;   /* 1 = EXE/DLL match the translated v3.2 pair */

static int join_path(char *out, size_t n, const char *a, const char *b) {
    int r = snprintf(out, n, "%s/%s", a, b);
    return (r < 0 || (size_t)r >= n) ? -1 : 0;
}

static int ensure_dir(const char *path) {
    if (mkdir(path, 0755) == 0 || errno == EEXIST) return 0;
    return -1;
}

/* mkdir -p: the --root value may not exist yet on a fresh install. */
static int ensure_dir_p(const char *path) {
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    if (!len || len >= sizeof tmp) return -1;
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (ensure_dir(tmp) < 0) return -1;
        *p = '/';
    }
    return ensure_dir(tmp);
}

static void exe_dir(char *out, size_t n) {
    char exe[PATH_MAX];
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k > 0) {
        exe[k] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) { *slash = 0; snprintf(out, n, "%s", exe); return; }
    }
    if (!getcwd(out, n)) snprintf(out, n, ".");
}

static int resolve_paths(const char *root_override) {
    if (root_override && *root_override) snprintf(g_root, sizeof g_root, "%s", root_override);
    else {
        const char *env = getenv("NFS5_ROOT");
        if (env && *env) snprintf(g_root, sizeof g_root, "%s", env);
        else exe_dir(g_root, sizeof g_root);
    }
    const char *env;
    if ((env = getenv("NFS5_DATA")) && *env) snprintf(g_data, sizeof g_data, "%s", env);
    else if (join_path(g_data, sizeof g_data, g_root, "data")) return -1;
    /* Player data lives beside the binaries: <root>/saves and <root>/configs.
     * Older installs used <root>/userdata/{saves,configs}; migrate that once. */
    {
        char legacy[PATH_MAX], legacy_saves[PATH_MAX], new_saves[PATH_MAX];
        char legacy_cfg[PATH_MAX], new_cfg[PATH_MAX];
        if (!join_path(legacy, sizeof legacy, g_root, "userdata") &&
            !join_path(legacy_saves, sizeof legacy_saves, legacy, "saves") &&
            !join_path(new_saves, sizeof new_saves, g_root, "saves") &&
            access(legacy_saves, F_OK) == 0 && access(new_saves, F_OK) != 0) {
            rename(legacy_saves, new_saves);
            if (!join_path(legacy_cfg, sizeof legacy_cfg, legacy, "configs") &&
                !join_path(new_cfg, sizeof new_cfg, g_root, "configs") &&
                access(legacy_cfg, F_OK) == 0 && access(new_cfg, F_OK) != 0)
                rename(legacy_cfg, new_cfg);
            rmdir(legacy);   /* only succeeds when empty */
        }
    }
    if ((env = getenv("NFS5_CONFIG")) && *env) snprintf(g_config, sizeof g_config, "%s", env);
    else if (join_path(g_config, sizeof g_config, g_root, "configs")) return -1;
    if ((env = getenv("NFS5_USERDATA")) && *env) snprintf(g_userdata, sizeof g_userdata, "%s", env);
    else snprintf(g_userdata, sizeof g_userdata, "%s", g_root);
    /* First run must create the port's writable directories beside the
     * executable: data/ receives the user's imported original files, while
     * userdata/ (player data: saves/, configs/) is always writable. Fail
     * loudly if the root is not writable instead of silently writing
     * somewhere else. */
    if (ensure_dir_p(g_root) < 0) { fprintf(stderr, "nfs5-launcher: cannot create %s\n", g_root); return -1; }
    ensure_dir(g_data);
    ensure_dir(g_userdata);
    ensure_dir(g_config);
    {   /* player-facing data folders beside the executable, migrating the old
         * flat config/ layout on first run so existing installs keep settings */
        char d[PATH_MAX];
        if (join_path(d, sizeof d, g_userdata, "saves") < 0) return -1;
        ensure_dir(d);
        if (join_path(d, sizeof d, g_userdata, "configs") < 0) return -1;
        ensure_dir(d);
        static const char *const legacy[] = { "launcher.cfg", "controller.cfg", "registry.txt", "install.json" };
        for (size_t i = 0; i < sizeof legacy / sizeof *legacy; i++) {
            char oldp[PATH_MAX], newp[PATH_MAX];
            snprintf(oldp, sizeof oldp, "%s/config/%s", g_root, legacy[i]);
            snprintf(newp, sizeof newp, "%s/%s", g_config, legacy[i]);
            if (access(oldp, F_OK) == 0 && access(newp, F_OK) != 0) rename(oldp, newp);
        }
    }
    if (access(g_userdata, W_OK) != 0 || access(g_config, W_OK) != 0) {
        fprintf(stderr, "nfs5-launcher: %s is not writable\n", g_root);
        return -1;
    }
    return 0;
}

/* ----------------------------------------------------------- status file */

static char g_status_path[PATH_MAX];
static char g_status_state[32];
static char g_status_msg[MAX_STATUS_MSG];
static char g_status_source[PATH_MAX];
static int g_status_pct = -1;

static void status_paths(void) {
    if (g_status_path[0]) return;
    snprintf(g_status_path, sizeof g_status_path, "%s/launcher-status.txt", g_config);
}

static void write_status(const char *state, int pct, const char *msg, const char *source) {
    status_paths();
    char temp[PATH_MAX];
    snprintf(temp, sizeof temp, "%s.tmp", g_status_path);
    FILE *f = fopen(temp, "w");
    if (!f) return;
    char clean[MAX_STATUS_MSG];
    snprintf(clean, sizeof clean, "%s", msg ? msg : "");
    for (char *p = clean; *p; p++)
        if (*p == '\n' || *p == '\r') *p = ' ';
    fprintf(f, "%s\n%d\n%s\n%s\n", state ? state : "", pct, clean, source ? source : "");
    if (fclose(f)) { unlink(temp); return; }
    if (rename(temp, g_status_path)) unlink(temp);
}

static int read_status(void) {
    status_paths();
    FILE *f = fopen(g_status_path, "r");
    if (!f) return -1;
    char state[32] = {0}, pct[32] = {0};
    int ok = 1;
    if (!fgets(state, sizeof state, f) || !fgets(pct, sizeof pct, f) ||
        !fgets(g_status_msg, sizeof g_status_msg, f) || !fgets(g_status_source, sizeof g_status_source, f))
        ok = 0;
    fclose(f);
    if (!ok) return -1;
    state[strcspn(state, "\r\n")] = 0;
    pct[strcspn(pct, "\r\n")] = 0;
    g_status_msg[strcspn(g_status_msg, "\r\n")] = 0;
    g_status_source[strcspn(g_status_source, "\r\n")] = 0;
    g_status_pct = atoi(pct);
    snprintf(g_status_state, sizeof g_status_state, "%s", state);
    return 0;
}

/* --------------------------------------------------------------- worker */
/* The check (possibly extracting a ZIP) and the import run in a child so the
 * UI keeps drawing and can show progress. The child writes the status file. */

enum { WORK_CHECK, WORK_IMPORT };
static pid_t g_worker = -1;
static int g_worker_kind = -1;

static int progress_to_status(const char *stage, int percent, void *user) {
    (void)user;
    write_status("IMPORTING", percent, stage, "");
    return 0;
}

static void run_check_worker(void) {
    Nfs5InstallState st;
    int verified = nfs5_verify_data(g_data, &st);
    g_data_verified = (verified == 0 && st.verified);
    write_status("CHECKING", 0, "CHECKING GAME FILES", "");
    char src[PATH_MAX] = {0}, msg[MAX_STATUS_MSG] = {0};
    int found = 1;
    if (verified == 0) {
        write_status("INSTALLED", 100, st.message, "");
        return;
    }
    found = nfs5_find_source_game(g_root, src, msg, sizeof msg);
    if (found != 0) {
        char find_msg[MAX_STATUS_MSG];
        snprintf(find_msg, sizeof find_msg, "%s", msg);
        write_status("EXTRACTING", 0, "LOOKING FOR A SOURCE ZIP", "");
        int extracted = nfs5_extract_source_zip(g_root, msg, sizeof msg);
        if (extracted == 0) {
            found = nfs5_find_source_game(g_root, src, msg, sizeof msg);
        } else if (extracted == 1) {
            /* No ZIP to try: keep the "no game files" explanation. */
            found = 1;
            snprintf(msg, sizeof msg, "%s", find_msg);
        } else {
            found = -100; /* extraction failed; msg has the reason */
        }
    }
    if (found == 0) {
        char line[MAX_STATUS_MSG];
        if (verified == 2) snprintf(line, sizeof line, "FOUND v%s IN SOURCE; EXISTING DATA IS A DIFFERENT VERSION", NFS5_VERSION_TEXT);
        else if (verified == 1) snprintf(line, sizeof line, "GAME FILES FOUND - READY TO COPY (ABOUT 600 MB)");
        else snprintf(line, sizeof line, "GAME FILES FOUND - READY TO COPY");
        write_status("READY", 100, line, src);
    } else if (found == 2) {
        write_status("WRONGVER", 0, msg, "");
    } else if (found == -100) {
        write_status("ERROR", 0, msg, "");
    } else {
        write_status("MISSING", 0, msg, "");
    }
}

static void run_import_worker(void) {
    char msg[MAX_STATUS_MSG] = {0};
    write_status("IMPORTING", 0, "STARTING IMPORT", g_status_source);
    if (nfs5_import(g_status_source, g_root, progress_to_status, NULL, msg, sizeof msg) == 0) {
        nfs5_cleanup_staging(g_root);
        write_status("DONE", 100, msg, "");
    } else {
        write_status("ERROR", 0, msg, "");
    }
}

static int start_worker(int kind) {
    if (g_worker > 0) return -1;
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setpgid(0, 0);
        char log[PATH_MAX];
        snprintf(log, sizeof log, "%s/launcher-worker.log", g_config);
        int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        if (kind == WORK_CHECK) run_check_worker();
        else run_import_worker();
        _exit(0);
    }
    setpgid(pid, pid);
    g_worker = pid;
    g_worker_kind = kind;
    return 0;
}

static void poll_worker(void) {
    if (g_worker <= 0) return;
    int status = 0;
    pid_t got = waitpid(g_worker, &status, WNOHANG);
    if (got == g_worker) {
        g_worker = -1;
        /* A child that exits without a terminal status leaves CHECKING behind. */
        if (read_status() == 0 &&
            (!strcmp(g_status_state, "CHECKING") || !strcmp(g_status_state, "EXTRACTING") ||
             !strcmp(g_status_state, "IMPORTING")))
            write_status("ERROR", 0, "LAUNCHER WORKER STOPPED UNEXPECTEDLY - SEE CONFIG/LAUNCHER-WORKER.LOG", "");
    }
}

static void stop_worker(void) {
    if (g_worker <= 0) return;
    kill(-g_worker, SIGTERM);
    for (int i = 0; i < 50; i++) {
        if (waitpid(g_worker, NULL, WNOHANG) == g_worker) { g_worker = -1; return; }
        usleep(20000);
    }
    kill(-g_worker, SIGKILL);
    waitpid(g_worker, NULL, 0);
    g_worker = -1;
}

/* ------------------------------------------------------------ 5x7 font */

typedef struct { char c; uint8_t rows[7]; } Glyph;
static const Glyph glyphs[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {' ', {0, 0, 0, 0, 0, 0, 0}}, {'%', {0x19, 0x1A, 0x02, 0x04, 0x08, 0x0B, 0x13}},
    {':', {0, 0x04, 0x04, 0, 0x04, 0x04, 0}}, {'>', {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08}},
    {'<', {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02}}, {'-', {0, 0, 0, 0x1F, 0, 0, 0}},
    {'_', {0, 0, 0, 0, 0, 0, 0x1F}}, {'.', {0, 0, 0, 0, 0, 0x0C, 0x0C}},
    {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}}, {'&', {0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D}},
    {'+', {0, 0x04, 0x04, 0x1F, 0x04, 0x04, 0}}, {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0, 0x04}},
    {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0, 0x04}}, {'\'', {0x04, 0x04, 0, 0, 0, 0, 0}},
    {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}}, {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
    {'[', {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E}}, {']', {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}},
    {'*', {0, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0}}, {'=', {0, 0, 0x1F, 0, 0x1F, 0, 0}},
    {',', {0, 0, 0, 0, 0x0C, 0x04, 0x08}}, {'#', {0x0A, 0x1F, 0x0A, 0x0A, 0x1F, 0x0A, 0}},
};
#define GLYPH_COUNT ((int)(sizeof glyphs / sizeof glyphs[0]))

static void glyph_rows(char c, int rows[7]) {
    for (int i = 0; i < 7; i++) rows[i] = 0;
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (int i = 0; i < GLYPH_COUNT; i++)
        if (glyphs[i].c == c) {
            for (int r = 0; r < 7; r++) rows[r] = glyphs[i].rows[r];
            return;
        }
}

static int text_width(const char *s, int scale) { return (int)strlen(s) * (FONT_W + 1) * scale; }

static void draw_text(SDL_Renderer *ren, const char *s, int x, int y, int scale, SDL_Color color) {
    SDL_SetRenderDrawColor(ren, color.r, color.g, color.b, color.a);
    for (; *s; s++, x += (FONT_W + 1) * scale) {
        int rows[7];
        glyph_rows(*s, rows);
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < FONT_W; c++)
                if (rows[r] & (1 << (FONT_W - 1 - c)))
                    for (int dy = 0; dy < scale; dy++)
                        for (int dx = 0; dx < scale; dx++)
                            SDL_RenderDrawPoint(ren, x + c * scale + dx, y + r * scale + dy);
    }
}

/* ------------------------------------------------------------- look
 * "Porsche design meets DOS setup program": graphite black, Guards Red,
 * silver and gold; CRT scanlines, double-line dialog window with an inset
 * title tab, inverted highlight bar, silver key-chip status bar, segmented
 * LED rev bars. Text, colour and shapes only - no logos. */
#define COL_BG        ((SDL_Color){12, 12, 14, 255})
#define COL_SCAN      ((SDL_Color){20, 20, 23, 255})
#define COL_PANEL     ((SDL_Color){22, 22, 25, 255})
#define COL_SILVER    ((SDL_Color){198, 200, 204, 255})
#define COL_SILVER_DK ((SDL_Color){112, 114, 120, 255})
#define COL_WHITE     ((SDL_Color){244, 243, 238, 255})
#define COL_RED       ((SDL_Color){213, 0, 28, 255})
#define COL_RED_DK    ((SDL_Color){110, 0, 14, 255})
#define COL_GOLD      ((SDL_Color){201, 165, 90, 255})
#define COL_AMBER     ((SDL_Color){255, 182, 40, 255})
#define COL_OK        ((SDL_Color){110, 222, 140, 255})
#define COL_WARN      ((SDL_Color){255, 140, 90, 255})
#define COL_TEXT      ((SDL_Color){178, 180, 186, 255})
#define COL_BRIGHT    COL_WHITE

#define WIN_X 22
#define WIN_Y 96
#define WIN_W (W - 2 * WIN_X)
#define WIN_H 340
#define BAR_H 24

static void set_color(SDL_Renderer *ren, SDL_Color c) { SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a); }
static void fill(SDL_Renderer *ren, int x, int y, int w, int h, SDL_Color c) {
    SDL_Rect r = {x, y, w, h};
    set_color(ren, c);
    SDL_RenderFillRect(ren, &r);
}
static void outline(SDL_Renderer *ren, int x, int y, int w, int h, SDL_Color c) {
    SDL_Rect r = {x, y, w, h};
    set_color(ren, c);
    SDL_RenderDrawRect(ren, &r);
}

static void draw_text_shadow(SDL_Renderer *ren, const char *s, int x, int y, int scale, SDL_Color color) {
    draw_text(ren, s, x + scale, y + scale, scale, (SDL_Color){0, 0, 0, 255});
    draw_text(ren, s, x, y, scale, color);
}

static void draw_text_center(SDL_Renderer *ren, const char *s, int y, int scale, SDL_Color color) {
    draw_text_shadow(ren, s, (W - text_width(s, scale)) / 2, y, scale, color);
}

/* Wide letter-spaced wordmark text. */
static int spaced_width(const char *s, int scale, int gap) {
    int n = (int)strlen(s);
    return n ? n * (FONT_W * scale + gap) - gap : 0;
}
static void draw_text_spaced(SDL_Renderer *ren, const char *s, int x, int y, int scale, int gap, SDL_Color c) {
    char one[2] = {0, 0};
    for (; *s; s++, x += FONT_W * scale + gap) {
        one[0] = *s;
        draw_text(ren, one, x + scale, y + scale, scale, (SDL_Color){0, 0, 0, 255});
        draw_text(ren, one, x, y, scale, c);
    }
}

static void draw_wrapped(SDL_Renderer *ren, const char *s, int x, int y, int max_chars, int line_h,
                         int scale, SDL_Color color) {
    char line[128];
    size_t len = strlen(s), at = 0;
    for (int row = 0; row < 6 && at < len; row++) {
        size_t n = len - at;
        if (n > (size_t)max_chars) {
            n = (size_t)max_chars;
            while (n > 0 && s[at + n] != ' ') n--;
            if (n == 0) n = (size_t)max_chars;
        }
        memcpy(line, s + at, n);
        line[n] = 0;
        draw_text(ren, line, x, y + row * line_h, scale, color);
        at += n;
        while (s[at] == ' ') at++;
    }
}

/* Small rev counter: dark face, silver ring, ticks with a red zone, needle. */
static void draw_tacho(SDL_Renderer *ren, int cx, int cy, int r, float value) {
    const float a0 = 3.92699f, a1 = -0.78540f;            /* 225 deg .. -45 deg */
    for (int dy = -r; dy <= r; dy++) {                    /* face */
        int dx = (int)SDL_sqrtf((float)(r * r - dy * dy));
        set_color(ren, (SDL_Color){6, 6, 8, 255});
        SDL_RenderDrawLine(ren, cx - dx, cy + dy, cx + dx, cy + dy);
    }
    for (int i = 0; i < 96; i++) {                        /* ring */
        float a = i * 6.28318f / 96;
        set_color(ren, COL_SILVER_DK);
        SDL_RenderDrawPoint(ren, cx + (int)(SDL_cosf(a) * r), cy + (int)(SDL_sinf(a) * r));
        SDL_RenderDrawPoint(ren, cx + (int)(SDL_cosf(a) * (r + 1)), cy + (int)(SDL_sinf(a) * (r + 1)));
    }
    for (int i = 0; i <= 16; i++) {                       /* ticks */
        float t = i / 16.0f, a = a0 + (a1 - a0) * t;
        float ca = SDL_cosf(a), sa = SDL_sinf(a);
        int inner = (i % 2) ? r - 5 : r - 9;
        set_color(ren, t >= 0.75f ? COL_RED : COL_SILVER);
        SDL_RenderDrawLine(ren, cx + (int)(ca * inner), cy - (int)(sa * inner),
                           cx + (int)(ca * (r - 2)), cy - (int)(sa * (r - 2)));
    }
    draw_text(ren, "RPM", cx - 8, cy + r / 2 - 1, 1, COL_SILVER_DK);
    float a = a0 + (a1 - a0) * value;
    set_color(ren, COL_RED);
    for (int d = -1; d <= 1; d++)
        SDL_RenderDrawLine(ren, cx + d, cy, cx + d + (int)(SDL_cosf(a) * (r - 7)), cy - (int)(SDL_sinf(a) * (r - 7)));
    fill(ren, cx - 3, cy - 3, 7, 7, COL_SILVER);
}

/* Segmented LED rev bar: green -> amber -> red, lit up to pct. */
static void draw_rev_bar(SDL_Renderer *ren, int x, int y, int segs, int seg_w, int h, int lit) {
    for (int i = 0; i < segs; i++) {
        float t = i / (float)(segs - 1);
        SDL_Color on = t < 0.6f ? COL_OK : t < 0.85f ? COL_AMBER : COL_RED;
        SDL_Color off = {(Uint8)(on.r / 6 + 10), (Uint8)(on.g / 6 + 10), (Uint8)(on.b / 6 + 10), 255};
        fill(ren, x + i * (seg_w + 3), y, seg_w, h, i < lit ? on : off);
    }
}

static const char *g_frame_title = "";

static void draw_frame(SDL_Renderer *ren, const char *subtitle) {
    g_frame_title = subtitle;
    /* CRT background */
    fill(ren, 0, 0, W, H, COL_BG);
    set_color(ren, COL_SCAN);
    for (int y = 0; y < H; y += 3) SDL_RenderDrawLine(ren, 0, y, W, y);

    /* header wordmark */
    draw_text_spaced(ren, "RENFS5", 40, 16, 2, 4, COL_RED);
    draw_text_spaced(ren, "OPEN UNLEASHED", 40, 38, 3, 5, COL_SILVER);
    draw_text(ren, "ENGINE: OPEN-UNLEASHED BY SECOND-BOOT", 42, 64, 1, COL_SILVER_DK);
    draw_tacho(ren, W - 62, 40, 30, 0.62f + 0.06f * SDL_sinf(SDL_GetTicks() / 400.0f));

    /* livery stripe */
    fill(ren, 0, 74, W, 4, COL_RED);
    fill(ren, 0, 80, W, 1, COL_GOLD);
    fill(ren, 0, 83, W, 2, COL_RED_DK);

    /* DOS dialog window: drop shadow, panel, double silver border */
    fill(ren, WIN_X + 6, WIN_Y + 6, WIN_W, WIN_H, (SDL_Color){0, 0, 0, 255});
    fill(ren, WIN_X, WIN_Y, WIN_W, WIN_H, COL_PANEL);
    outline(ren, WIN_X + 2, WIN_Y + 2, WIN_W - 4, WIN_H - 4, COL_SILVER);
    outline(ren, WIN_X + 5, WIN_Y + 5, WIN_W - 10, WIN_H - 10, COL_SILVER);

    /* title tab inset into the top border */
    if (subtitle && *subtitle) {
        int tw = text_width(subtitle, 2) + 24;
        int tx = (W - tw) / 2;
        fill(ren, tx, WIN_Y - 7, tw, 22, COL_RED);
        outline(ren, tx, WIN_Y - 7, tw, 22, COL_WHITE);
        draw_text(ren, subtitle, tx + 12, WIN_Y - 3, 2, COL_WHITE);
    }

    /* status bar */
    fill(ren, 0, H - BAR_H, W, BAR_H, COL_SILVER);
    fill(ren, 0, H - BAR_H, W, 1, COL_WHITE);
}

static void draw_row(SDL_Renderer *ren, const char *label, const char *value, int y, int selected,
                     SDL_Color normal, SDL_Color hot) {
    int bx = WIN_X + 12, bw = WIN_W - 24;
    if (selected) {
        fill(ren, bx, y - 6, bw, 26, COL_RED);
        fill(ren, bx, y - 6, 4, 26, COL_GOLD);
        draw_text(ren, label, 64, y, 2, COL_WHITE);
        if (value && *value) {
            char buf[64];
            snprintf(buf, sizeof buf, "< %s >", value);
            draw_text(ren, buf, W - 64 - text_width(buf, 2), y, 2, COL_WHITE);
        }
        return;
    }
    SDL_Color c = (normal.r == hot.r && normal.g == hot.g && normal.b == hot.b) ? COL_SILVER : COL_TEXT;
    draw_text_shadow(ren, label, 64, y, 2, c);
    if (value && *value) {
        int vw = text_width(value, 2);
        draw_text_shadow(ren, value, W - 76 - vw, y, 2, COL_AMBER);
    }
    set_color(ren, (SDL_Color){34, 34, 38, 255});
    SDL_RenderDrawLine(ren, bx + 8, y + 19, bx + bw - 8, y + 19);
}

/* Status bar: groups separated by two spaces; the first word of each group
 * is a key, drawn as a red chip ("A SELECT  B EXIT"). Text without groups is
 * drawn plain. */
static void draw_hint(SDL_Renderer *ren, const char *text) {
    int y = H - BAR_H + 9;
    if (!strstr(text, "  ") && !(text[0] && text[1] == ' ')) {
        draw_text(ren, text, (W - text_width(text, 1)) / 2, y, 1, (SDL_Color){20, 20, 22, 255});
        return;
    }
    char groups[8][64];
    int n = 0;
    const char *p = text;
    while (*p && n < 8) {
        while (*p == ' ') p++;
        const char *e = strstr(p, "  ");
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len >= sizeof groups[0]) len = sizeof groups[0] - 1;
        memcpy(groups[n], p, len);
        groups[n][len] = 0;
        if (len) n++;
        p += len;
    }
    int total = 0;
    for (int i = 0; i < n; i++) total += text_width(groups[i], 1) + 10 + 18;
    int x = (W - total) / 2;
    for (int i = 0; i < n; i++) {
        char key[24] = "", *sp = strchr(groups[i], ' ');
        const char *label = "";
        if (sp) {
            size_t kl = (size_t)(sp - groups[i]);
            if (kl >= sizeof key) kl = sizeof key - 1;
            memcpy(key, groups[i], kl);
            key[kl] = 0;
            label = sp + 1;
        } else {
            snprintf(key, sizeof key, "%s", groups[i]);
        }
        int kw = text_width(key, 1) + 6;
        fill(ren, x, y - 4, kw, 15, COL_RED);
        draw_text(ren, key, x + 3, y, 1, COL_WHITE);
        x += kw + 5;
        draw_text(ren, label, x, y, 1, (SDL_Color){20, 20, 22, 255});
        x += text_width(label, 1) + 18;
    }
}

/* "A  IMPORT GAME FILES" style choice: red key chip, then the label. */
static void draw_option(SDL_Renderer *ren, const char *key, const char *label, int x, int y, int primary) {
    fill(ren, x, y - 5, 24, 24, primary ? COL_RED : COL_RED_DK);
    outline(ren, x, y - 5, 24, 24, primary ? COL_WHITE : COL_SILVER_DK);
    draw_text(ren, key, x + 7, y, 2, COL_WHITE);
    draw_text_shadow(ren, label, x + 36, y, 2, primary ? COL_WHITE : COL_TEXT);
}

/* ----------------------------------------------------------- persistent */

static Nfs5Settings g_settings;
static Nfs5PadMap g_padmap;
static char g_source[PATH_MAX];
static char g_install_msg[MAX_STATUS_MSG];

static int load_persistent(void) {
    nfs5_settings_defaults(&g_settings);
    if (nfs5_settings_load(g_config, &g_settings) < 0) {
        snprintf(g_install_msg, sizeof g_install_msg, "INVALID CONFIG/LAUNCHER.CFG - USING DEFAULTS");
        nfs5_settings_defaults(&g_settings);
    }
    nfs5_padmap_defaults(&g_padmap);
    if (nfs5_padmap_load(g_config, &g_padmap) < 0) {
        snprintf(g_install_msg, sizeof g_install_msg, "INVALID CONFIG/CONTROLLER.CFG - USING DEFAULTS");
        nfs5_padmap_defaults(&g_padmap);
    }
    return 0;
}

static int save_persistent(void) {
    if (nfs5_settings_save(g_config, &g_settings) < 0) return -1;
    if (nfs5_padmap_save(g_config, &g_padmap) < 0) return -1;
    return 0;
}

static const char *frame_cap_label(int cap) {
    if (cap == 30) return "30 FPS";
    if (cap == 60) return "60 FPS";
    return "UNCAPPED";
}

/* --------------------------------------------------------------- UI state */

enum Page { PAGE_CHECK, PAGE_SETTINGS, PAGE_INSTALL, PAGE_IMPORTING, PAGE_PADMAP, PAGE_ERROR, PAGE_EXITING };
static enum Page page = PAGE_CHECK;
static int selection = 0;
static int pad_selection = 0;
static int capture_action = -1;
static int chosen_launch = 0;
static int interrupted = 0;
static int toast_frames = 0;

static int pad_page_count(void) { return NFS5_ACTION_COUNT + 2; }
/* Settings page rows, top to bottom. GAME SCREEN only shows on handhelds with
 * more than one connected display. */
enum { ROW_FRAME_CAP, ROW_FPS, ROW_VIBRATION, ROW_GRAPHICS, ROW_RESOLUTION, ROW_ASPECT, ROW_MOVIES, ROW_SCREEN, ROW_PADMAP,
       ROW_INSTALLED, ROW_LAUNCH, ROW_EXIT, ROW_COUNT };
static int settings_page_count(void) { return ROW_COUNT; }

/* Connected display connectors (DRM, e.g. DSI-1 / DSI-2), by name. */
#define MAX_SCREENS 4
static char g_screens[MAX_SCREENS][16];
static int g_nscreens = -1;
static void scan_screens(void) {
    g_nscreens = 0;
    for (int card = 0; card < 4; card++) {
        static const char *kinds[] = {"DSI", "eDP", "HDMI-A", "DP", "LVDS", "VGA", "DPI", "Composite"};
        for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++)
            for (int n = 1; n <= 4 && g_nscreens < MAX_SCREENS; n++) {
                char path[96], st[32] = "";
                snprintf(path, sizeof path, "/sys/class/drm/card%d-%s-%d/status", card, kinds[k], n);
                FILE *f = fopen(path, "r");
                if (!f) continue;
                if (fgets(st, sizeof st, f) && !strncmp(st, "connected", 9)) {
                    char name[16];
                    snprintf(name, sizeof name, "%s-%d", kinds[k], n);
                    int dup = 0;
                    for (int i = 0; i < g_nscreens; i++) dup |= !strcmp(g_screens[i], name);
                    if (!dup) snprintf(g_screens[g_nscreens++], sizeof g_screens[0], "%s", name);
                }
                fclose(f);
            }
    }
}
/* Whether this firmware can run the engine on Vulkan (checked once). */
static int vulkan_available(void) {
    static int known = -1;
    if (known < 0) known = getenv("NFS5_FAKE_VULKAN") ? atoi(getenv("NFS5_FAKE_VULKAN")) != 0 : nfs5_vulkan_available();
    return known;
}

static int screen_row_shown(void) {
    if (g_nscreens < 0) scan_screens();
    return g_nscreens > 1 || g_settings.screen[0];   /* a saved choice stays editable */
}
/* Rows the engine has no use for: it renders at the panel's own size and keeps its
 * controls in its own Options menu. */
static int row_shown(int r) {
    if (r == ROW_RESOLUTION || r == ROW_ASPECT || r == ROW_PADMAP) return 0;
    if (r == ROW_SCREEN) return screen_row_shown();
    return 1;
}
/* Position of a row among the shown ones. */
static int row_slot(int r) {
    int n = 0;
    for (int i = 0; i < r; i++) n += row_shown(i);
    return n;
}
static void move_selection(int dir) {
    do selection = (selection + dir + ROW_COUNT) % ROW_COUNT;
    while (!row_shown(selection));
}
static void enter_settings(void) {
    load_persistent();
    selection = 0;
    page = PAGE_SETTINGS;
}

static void start_check(void) {
    write_status("CHECKING", 0, "CHECKING GAME FILES", "");
    if (start_worker(WORK_CHECK) < 0) {
        write_status("ERROR", 0, "CANNOT START CHECKER", "");
        page = PAGE_ERROR;
        return;
    }
    page = PAGE_CHECK;
}

static void start_import(void) {
    if (!g_status_source[0]) {
        snprintf(g_install_msg, sizeof g_install_msg, "NO SOURCE DIRECTORY SELECTED");
        page = PAGE_INSTALL;
        return;
    }
    snprintf(g_source, sizeof g_source, "%s", g_status_source);
    selection = 0;
    if (start_worker(WORK_IMPORT) < 0) {
        snprintf(g_install_msg, sizeof g_install_msg, "CANNOT START IMPORTER");
        page = PAGE_ERROR;
        return;
    }
    page = PAGE_IMPORTING;
}

/* Apply status-file transitions. Returns 1 when the page changed. */
static int apply_status_transition(void) {
    if (!g_status_state[0]) return 0;
    if (!strcmp(g_status_state, "INSTALLED") && page == PAGE_CHECK) {
        enter_settings();
        return 1;
    }
    if (!strcmp(g_status_state, "READY") && page != PAGE_INSTALL) {
        if (g_status_source[0]) snprintf(g_source, sizeof g_source, "%s", g_status_source);
        page = PAGE_INSTALL;
        selection = 0;
        return 1;
    }
    if (!strcmp(g_status_state, "WRONGVER") || !strcmp(g_status_state, "MISSING") ||
        !strcmp(g_status_state, "ERROR")) {
        if (page != PAGE_ERROR && page != PAGE_INSTALL) {
            page = (!strcmp(g_status_state, "MISSING") || !strcmp(g_status_state, "WRONGVER")) ? PAGE_INSTALL : PAGE_ERROR;
            selection = 0;
            return 1;
        }
    }
    if (!strcmp(g_status_state, "DONE") && page == PAGE_IMPORTING) {
        enter_settings();
        toast_frames = 90;
        return 1;
    }
    return 0;
}

/* --------------------------------------------------------------- drawing */

static void draw_check_page(SDL_Renderer *ren) {
    draw_frame(ren, "CHECKING GAME FILES");
    const char *msg = g_status_msg[0] ? g_status_msg : "SCANNING DATA AND SOURCE FOLDERS";
    draw_text_center(ren, "PLEASE WAIT", 210, 3, COL_BRIGHT);
    draw_wrapped(ren, msg, 80, 260, 48, 16, 2, COL_TEXT);
    int sweep = (int)((SDL_GetTicks() / 70) % 40);
    draw_rev_bar(ren, (W - (20 * 21 - 3)) / 2, 330, 20, 18, 12, sweep < 20 ? sweep + 1 : 40 - sweep);
    draw_hint(ren, "B CANCEL");
}

static void draw_install_page(SDL_Renderer *ren) {
    SDL_Color text = COL_TEXT, bright = COL_BRIGHT;
    if (!strcmp(g_status_state, "READY")) {
        draw_frame(ren, "GAME FILES FOUND");
        draw_text_center(ren, "READY TO COPY TO DEVICE", 150, 2, bright);
        char line[128];
        snprintf(line, sizeof line, "SOURCE: %s", g_source[0] ? g_source : "(SOURCE)");
        draw_wrapped(ren, line, 60, 185, 52, 16, 1, text);
        draw_wrapped(ren, "ABOUT 600 MB WILL BE COPIED TO DATA/. THE ORIGINAL FILES STAY IN SOURCE/.", 60,
                     240, 52, 16, 1, text);
        draw_wrapped(ren, g_status_msg, 60, 275, 52, 16, 1, text);
        draw_option(ren, "A", "IMPORT GAME FILES", 60, 340, 1);
        draw_option(ren, "B", "EXIT WITHOUT CHANGING", 60, 370, 0);
        draw_hint(ren, "A IMPORT   B EXIT   UP/DOWN NOT USED");
    } else if (!strcmp(g_status_state, "WRONGVER")) {
        draw_frame(ren, "UNSUPPORTED GAME VERSION");
        draw_wrapped(ren, "THE GAME FILES IN SOURCE/ ARE NOT THE SUPPORTED V3.2 VERSION.", 60, 160, 42, 18, 2,
                     COL_WARN);
        draw_wrapped(ren, g_status_msg, 60, 220, 52, 16, 1, text);
        draw_wrapped(ren, "REPLACE SOURCE/ WITH AN EXTRACTED V3.2 INSTALL (PORSCHE.EXE + DRIVERS/DX7Z.DLL), THEN PRESS A.", 60,
                     270, 52, 16, 1, text);
        draw_option(ren, "A", "RESCAN SOURCE", 60, 350, 1);
        draw_option(ren, "B", "EXIT", 60, 380, 0);
        draw_hint(ren, "A RESCAN   B EXIT");
    } else {
        draw_frame(ren, "GAME FILES NOT INSTALLED");
        char path[PATH_MAX];
        snprintf(path, sizeof path, "COPY YOUR EXTRACTED V%s INSTALL (OR A .ZIP OF IT) INTO:", NFS5_VERSION_TEXT);
        draw_wrapped(ren, path, 60, 150, 52, 16, 1, bright);
        snprintf(path, sizeof path, "%s/source", g_root);
        if (strlen(path) <= 42) draw_wrapped(ren, path, 60, 185, 42, 18, 2, COL_AMBER);
        else draw_wrapped(ren, path, 60, 185, 84, 12, 1, COL_AMBER);   /* long path: small type, max 3 lines */
        draw_wrapped(ren, "KEEP PORSCHE.EXE, DRIVERS/DX7Z.DLL, FEDATA/ AND GAMEDATA/ TOGETHER. THE PACKAGE NEVER CONTAINS GAME FILES.", 60,
                     245, 52, 16, 1, text);
        draw_wrapped(ren, g_status_msg, 60, 290, 52, 16, 1, text);
        draw_option(ren, "A", "RESCAN SOURCE FOLDER", 60, 350, 1);
        draw_option(ren, "B", "EXIT", 60, 382, 0);
        draw_hint(ren, "A RESCAN   B EXIT");
    }
}

static void draw_importing_page(SDL_Renderer *ren) {
    draw_frame(ren, "INSTALLING GAME FILES");
    draw_text_center(ren, g_status_msg[0] ? g_status_msg : "COPYING", 170, 2, COL_BRIGHT);
    int pct = g_status_pct < 0 ? 0 : g_status_pct > 100 ? 100 : g_status_pct;
    fill(ren, 46, 236, W - 92, 32, (SDL_Color){0, 0, 0, 255});
    outline(ren, 46, 236, W - 92, 32, COL_SILVER_DK);
    draw_rev_bar(ren, 52, 242, 40, 10, 20, pct * 40 / 100);
    char line[64];
    snprintf(line, sizeof line, "%d%%", pct);
    draw_text_center(ren, line, 285, 2, COL_TEXT);
    draw_wrapped(ren, "DO NOT REMOVE THE SD CARD. THE ORIGINAL FILES IN SOURCE/ ARE KEPT.", 60, 340, 52, 16, 1,
                 COL_TEXT);
    draw_hint(ren, "B CANCEL - PARTIAL FILES ARE SAFE TO RE-IMPORT");
}

static void draw_settings_page(SDL_Renderer *ren) {
    draw_frame(ren, "SETTINGS");
    SDL_Color normal = COL_TEXT, hot = COL_BRIGHT;
    int y = 124;
    int step = 30;
    /* hidden rows take no space (row_shown) */
#define ROW(r, label, value, gap, n, h) \
    if (row_shown(r)) draw_row(ren, label, value, y + row_slot(r) * step + (gap), selection == (r), n, h)
    ROW(ROW_FRAME_CAP, "FRAME CAP", frame_cap_label(g_settings.frame_cap), 0, normal, hot);
    ROW(ROW_FPS, "FPS COUNTER", g_settings.fps_counter ? "ON" : "OFF", 0, normal, hot);
    ROW(ROW_VIBRATION, "VIBRATION", g_settings.vibration ? "ON" : "OFF", 0, normal, hot);
    ROW(ROW_GRAPHICS, "GRAPHICS", vulkan_available() ? nfs5_graphics_name(g_settings.graphics) : "GLES (NO VULKAN)", 0, normal, hot);
    ROW(ROW_RESOLUTION, "RESOLUTION", nfs5_resolution_name(g_settings.resolution), 0, normal, hot);
    ROW(ROW_ASPECT, "ASPECT", nfs5_aspect_name(g_settings.aspect), 0, normal, hot);
    ROW(ROW_MOVIES, "MOVIES", nfs5_movies_name(g_settings.movies), 0, normal, hot);
    ROW(ROW_SCREEN, "GAME SCREEN", g_settings.screen[0] ? g_settings.screen : "DEFAULT", 0, normal, hot);
    ROW(ROW_PADMAP, "CONTROLLER MAPPING", "EDIT >", 0, normal, hot);
    ROW(ROW_INSTALLED, "INSTALLED GAME", g_data_verified ? "VERIFIED" : "CHECK >", 0, normal, hot);
    ROW(ROW_LAUNCH, "SAVE & LAUNCH", "", 6, hot, hot);
    ROW(ROW_EXIT, "EXIT", "", 6, normal, hot);
#undef ROW
    if (toast_frames > 0) {
        fill(ren, 0, H - BAR_H, W, BAR_H, COL_OK);
        draw_text(ren, "GAME FILES INSTALLED AND VERIFIED", (W - text_width("GAME FILES INSTALLED AND VERIFIED", 1)) / 2,
                  H - BAR_H + 9, 1, (SDL_Color){10, 30, 14, 255});
    } else {
        draw_hint(ren, "UP/DOWN MOVE  LEFT/RIGHT CHANGE  A SELECT  B EXIT");
    }
}

static void draw_padmap_page(SDL_Renderer *ren) {
    draw_frame(ren, "CONTROLLER MAPPING");
    SDL_Color normal = COL_TEXT, hot = COL_BRIGHT;
    int y = 122;
    for (int i = 0; i < NFS5_ACTION_COUNT; i++) {
        char label[32];
        snprintf(label, sizeof label, "%s", nfs5_action_name(i));
        for (char *p = label; *p; p++)
            if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
        for (char *p = label; *p; p++)
            if (*p == '_') *p = ' ';
        const char *value = g_padmap.button[i];
        char upper[24];
        snprintf(upper, sizeof upper, "%s", value);
        for (char *p = upper; *p; p++)
            if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
        draw_row(ren, label, upper, y + i * 25, pad_selection == i, normal, hot);
    }
    draw_row(ren, "RESET DEFAULTS", "", y + NFS5_ACTION_COUNT * 25 + 6,
             pad_selection == NFS5_ACTION_COUNT, hot, hot);
    draw_row(ren, "BACK", "", y + NFS5_ACTION_COUNT * 25 + 34, pad_selection == NFS5_ACTION_COUNT + 1,
             normal, hot);
    draw_hint(ren, "A REBIND   LEFT/RIGHT CYCLE   B BACK");
}

static void draw_capture_overlay(SDL_Renderer *ren) {
    int x = 80, y = 176, w = W - 160, h = 132;
    fill(ren, x + 6, y + 6, w, h, (SDL_Color){0, 0, 0, 255});
    fill(ren, x, y, w, h, COL_PANEL);
    outline(ren, x + 2, y + 2, w - 4, h - 4, COL_SILVER);
    outline(ren, x + 5, y + 5, w - 10, h - 10, COL_SILVER);
    fill(ren, x + 2, y + 2, w - 4, 22, COL_RED);
    draw_text(ren, "REBIND", (W - text_width("REBIND", 2)) / 2, y + 6, 2, COL_WHITE);
    draw_text_center(ren, "PRESS A CONTROLLER BUTTON", y + 40, 2, COL_BRIGHT);
    char line[64];
    const char *name = nfs5_action_name(capture_action);
    snprintf(line, sizeof line, "FOR: %s", name ? name : "?");
    for (char *q = line; *q; q++) {
        if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 'a' + 'A');
        if (*q == '_') *q = ' ';
    }
    draw_text_center(ren, line, y + 70, 2, COL_AMBER);
    draw_text_center(ren, "ESC CANCELS", y + 102, 1, COL_TEXT);
}

static void draw_error_page(SDL_Renderer *ren) {
    draw_frame(ren, "ERROR");
    draw_wrapped(ren, g_status_msg[0] ? g_status_msg : "UNKNOWN ERROR", 60, 175, 42, 18, 2,
                 COL_WARN);
    draw_option(ren, "A", "RETRY", 60, 340, 1);
    draw_option(ren, "B", "EXIT", 60, 374, 0);
    draw_hint(ren, "SEE CONFIG/LAUNCHER-WORKER.LOG FOR DETAILS");
}

/* --------------------------------------------------------------- input */

static void change_setting(int dir) {
    switch (selection) {
        case ROW_FRAME_CAP:
            if (dir > 0) g_settings.frame_cap = g_settings.frame_cap == 60 ? 30 : g_settings.frame_cap == 30 ? 0 : 60;
            else g_settings.frame_cap = g_settings.frame_cap == 60 ? 0 : g_settings.frame_cap == 0 ? 30 : 60;
            break;
        case ROW_FPS: g_settings.fps_counter = !g_settings.fps_counter; break;
        case ROW_VIBRATION: g_settings.vibration = !g_settings.vibration; break;
        case ROW_GRAPHICS:      /* Vulkan only where the firmware has a driver for it */
            if (vulkan_available()) g_settings.graphics = g_settings.graphics == NFS5_GFX_VULKAN ? NFS5_GFX_GLES : NFS5_GFX_VULKAN;
            break;
        case ROW_RESOLUTION:
            g_settings.resolution = (g_settings.resolution + (dir > 0 ? 1 : NFS5_RES_COUNT - 1)) % NFS5_RES_COUNT;
            break;
        case ROW_ASPECT:
            g_settings.aspect = (g_settings.aspect + (dir > 0 ? 1 : NFS5_ASPECT_COUNT - 1)) % NFS5_ASPECT_COUNT;
            break;
        case ROW_MOVIES:
            g_settings.movies = !g_settings.movies;
            break;
        case ROW_SCREEN: {      /* DEFAULT, then each connected display by name */
            if (g_nscreens < 0) scan_screens();
            int cur = -1, n = g_nscreens + 1;
            for (int i = 0; i < g_nscreens; i++)
                if (!strcmp(g_settings.screen, g_screens[i])) cur = i;
            int next = ((cur + 1) + (dir > 0 ? 1 : n - 1)) % n - 1;   /* -1 = DEFAULT */
            if (next < 0) g_settings.screen[0] = 0;
            else snprintf(g_settings.screen, sizeof g_settings.screen, "%s", g_screens[next]);
            break;
        }
        default: break;
    }
}

static int confirm_settings(void) {
    if (selection == ROW_LAUNCH) {
        if (save_persistent() < 0) {
            snprintf(g_install_msg, sizeof g_install_msg, "CANNOT WRITE CONFIG");
            return -1;
        }
        chosen_launch = 1;
        return 1;
    }
    if (selection == ROW_EXIT || selection == ROW_INSTALLED) {
        if (selection == ROW_INSTALLED) return 0;
        return 2;
    }
    if (selection == ROW_PADMAP) {
        pad_selection = 0;
        page = PAGE_PADMAP;
        return 0;
    }
    change_setting(1);
    return 0;
}

/* --------------------------------------------------------------- actions */

static int run_verify_mode(void) {
    Nfs5InstallState st;
    int rc = nfs5_verify_data(g_data, &st);
    printf("NFS5 VERIFY root=%s data=%s installed=%d status=%d message=%s\n", g_root, g_data,
           st.installed, rc, st.message);
    if (st.porsche_sha256[0]) printf("NFS5 PORSCHE_SHA256 %s\n", st.porsche_sha256);
    if (st.dx7z_sha256[0]) printf("NFS5 DX7Z_SHA256 %s\n", st.dx7z_sha256);
    if (rc == 0) return 0;
    char src[PATH_MAX] = {0}, msg[MAX_STATUS_MSG] = {0};
    int found = nfs5_find_source_game(g_root, src, msg, sizeof msg);
    if (found == 0) {
        printf("NFS5 SOURCE_READY %s\n", src);
        return 3;
    }
    printf("NFS5 SOURCE_STATUS %d %s\n", found, msg);
    return rc == 2 ? 2 : 3;
}

static int run_import_mode(const char *source_override) {
    char src[PATH_MAX] = {0}, msg[MAX_STATUS_MSG] = {0};
    if (source_override && *source_override) {
        snprintf(src, sizeof src, "%s", source_override);
    } else {
        int found = nfs5_find_source_game(g_root, src, msg, sizeof msg);
        if (found != 0) {
            fprintf(stderr, "import: %s\n", msg);
            if (nfs5_extract_source_zip(g_root, msg, sizeof msg) != 0) {
                fprintf(stderr, "import: %s\n", msg);
                return 1;
            }
            found = nfs5_find_source_game(g_root, src, msg, sizeof msg);
            if (found != 0) {
                fprintf(stderr, "import: %s\n", msg);
                return 1;
            }
        }
    }
    printf("NFS5 IMPORT source=%s root=%s\n", src, g_root);
    if (nfs5_import(src, g_root, NULL, NULL, msg, sizeof msg) != 0) {
        fprintf(stderr, "import failed: %s\n", msg);
        return 1;
    }
    nfs5_cleanup_staging(g_root);
    printf("NFS5 IMPORT_OK %s\n", msg);
    return 0;
}

static int selftest_report(const char *name, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    return ok ? 0 : 1;
}

static int run_selftest(void) {
    int failures = 0;
    /* SHA-256 standard vectors. */
    struct { const char *in, *want; } vectors[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"The quick brown fox jumps over the lazy dog",
         "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"},
    };
    for (size_t i = 0; i < sizeof vectors / sizeof *vectors; i++) {
        Nfs5Sha256 ctx;
        uint8_t digest[32];
        char hex[65];
        nfs5_sha256_init(&ctx);
        nfs5_sha256_update(&ctx, vectors[i].in, strlen(vectors[i].in));
        nfs5_sha256_final(&ctx, digest);
        nfs5_sha256_hex(digest, hex);
        char name[64];
        snprintf(name, sizeof name, "sha256 vector %zu", i);
        failures += selftest_report(name, !strcmp(hex, vectors[i].want));
    }
    char tmpl[PATH_MAX];
    snprintf(tmpl, sizeof tmpl, "%s/selftest-XXXXXX", g_root);
    char *tmp = mkdtemp(tmpl);
    if (!tmp) {
        printf("FAIL mkdtemp in %s\n", g_root);
        return 1;
    }
    char path[PATH_MAX], cfg[PATH_MAX];
    snprintf(path, sizeof path, "%s/abc.txt", tmp);
    snprintf(cfg, sizeof cfg, "%s/config", tmp);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite("abc", 1, 3, f); fclose(f); }
    char hex[65];
    failures += selftest_report("sha256 file", nfs5_sha256_file(path, hex) == 0 &&
                                                   !strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    /* Settings round-trip and rejection of invalid files. */
    Nfs5Settings s;
    nfs5_settings_defaults(&s);
    s.frame_cap = 30;
    s.fps_counter = 1;
    s.detail = NFS5_DETAIL_LOW;
    s.resolution = NFS5_RES_800x600;
    s.aspect = NFS5_ASPECT_STRETCH;
    s.movies = 0;
    failures += selftest_report("settings save", nfs5_settings_save(cfg, &s) == 0);
    Nfs5Settings loaded;
    nfs5_settings_defaults(&loaded);
    int lr = nfs5_settings_load(cfg, &loaded);
    failures += selftest_report("settings load", lr == 1 && loaded.frame_cap == 30 &&
                                                     loaded.fps_counter == 1 &&
                                                     loaded.detail == NFS5_DETAIL_LOW &&
                                                     loaded.resolution == NFS5_RES_800x600 &&
                                                     loaded.movies == 0 &&
                                                     loaded.aspect == NFS5_ASPECT_STRETCH);
    snprintf(path, sizeof path, "%s/launcher.cfg", cfg);
    f = fopen(path, "wb");
    if (f) { fwrite("frame_cap=45\n", 1, 13, f); fclose(f); }
    Nfs5Settings keep = loaded;
    failures += selftest_report("invalid settings rejected", nfs5_settings_load(cfg, &loaded) == -1 &&
                                                                 loaded.frame_cap == keep.frame_cap);
    /* Controller map round-trip. */
    Nfs5PadMap m;
    nfs5_padmap_defaults(&m);
    snprintf(m.button[0], sizeof m.button[0], "leftshoulder");
    failures += selftest_report("padmap save", nfs5_padmap_save(cfg, &m) == 0);
    Nfs5PadMap ml;
    nfs5_padmap_defaults(&ml);
    failures += selftest_report("padmap load", nfs5_padmap_load(cfg, &ml) == 1 &&
                                                   !strcmp(ml.button[0], "leftshoulder"));
    failures += selftest_report("button name mapping", nfs5_button_index("dp_left") == 13 &&
                                                          nfs5_button_index("bogus") == -1);
    /* Missing data and rejection of a fake game tree. */
    Nfs5InstallState st;
    char missing[PATH_MAX];
    snprintf(missing, sizeof missing, "%s/no-data", tmp);
    failures += selftest_report("missing data detected", nfs5_verify_data(missing, &st) == 1);
    failures += selftest_report("runtime directories created", access(g_data, F_OK) == 0 &&
                                                             access(g_userdata, W_OK) == 0 &&
                                                             access(g_config, W_OK) == 0);
    char fake[PATH_MAX], fake_exe[PATH_MAX];
    snprintf(fake, sizeof fake, "%s/fake", tmp);
    snprintf(fake_exe, sizeof fake_exe, "%s/%s", fake, NFS5_PORSCHE_EXE);
    mkdir(fake, 0755);
    f = fopen(fake_exe, "wb");
    if (f) { fwrite("not the game", 1, 12, f); fclose(f); }
    char msg[256] = {0};
    failures += selftest_report("unsupported source rejected",
                                nfs5_import(fake, tmp, NULL, NULL, msg, sizeof msg) == -1);
    /* A completed worker must not override navigation or unsaved FPS edits. */
    const char *terminal[] = {"INSTALLED", "DONE"};
    for (int i = 0; i < 2; i++) {
        snprintf(g_status_state, sizeof g_status_state, "%s", terminal[i]);
        page = PAGE_SETTINGS;
        g_settings.fps_counter = 1;
        selection = ROW_PADMAP;
        confirm_settings();
        apply_status_transition();
        failures += selftest_report("controller page survives terminal worker status",
                                    page == PAGE_PADMAP && g_settings.fps_counter == 1);
    }
    nfs5_rm_rf(tmp);   /* native recursive delete: the launcher shells out to nothing */
    printf("%s: %d failure(s)\n", failures ? "SELFTEST FAILED" : "SELFTEST PASSED", failures);
    return failures ? 1 : 0;
}

/* --------------------------------------------------------------- GUI main */

static void save_screenshot(SDL_Renderer *ren, const char *path) {
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) {
        fprintf(stderr, "screenshot: %s\n", SDL_GetError());
        return;
    }
    if (SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, surface->pixels, surface->pitch) == 0) {
        if (SDL_SaveBMP(surface, path) != 0) fprintf(stderr, "screenshot: %s: %s\n", path, SDL_GetError());
        else printf("NFS5 SCREENSHOT %s\n", path);
    } else {
        fprintf(stderr, "screenshot read: %s\n", SDL_GetError());
    }
    SDL_FreeSurface(surface);
}

static int gui_main(const char *screenshot, const char *screenshot_page, int autotest) {
    /* Autotest has no user at the window: prefer the headless dummy driver, but
     * device SDL builds may not ship it — then fall back to the real backend. */
    int autotest_dummy = 0;
    if ((screenshot || autotest) && !getenv("SDL_VIDEODRIVER")) {
        setenv("SDL_VIDEODRIVER", "dummy", 1);
        autotest_dummy = 1;
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS)) {
        if (autotest_dummy) {
            fprintf(stderr, "nfs5-launcher: dummy video unavailable (%s); using the device driver\n",
                    SDL_GetError());
            unsetenv("SDL_VIDEODRIVER");
            autotest_dummy = 0;
            if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) == 0)
                goto sdl_ready;
        }
        fprintf(stderr, "nfs5-launcher: SDL_Init: %s\n", SDL_GetError());
        return 2;
    }
sdl_ready:;
    uint32_t flags = (screenshot || autotest || getenv("NFS5_LAUNCHER_WINDOWED"))
                         ? SDL_WINDOW_SHOWN
                         : (SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN);
    SDL_Window *win = SDL_CreateWindow("ReNFS5", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H, flags);
    if (!win) win = SDL_CreateWindow("ReNFS5", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H, SDL_WINDOW_SHOWN);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (!ren && win) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) {
        fprintf(stderr, "nfs5-launcher: no renderer: %s\n", SDL_GetError());
        if (win) SDL_DestroyWindow(win);
        SDL_Quit();
        return 2;
    }
    SDL_RenderSetLogicalSize(ren, W, H);
    SDL_ShowCursor(SDL_DISABLE);
    SDL_GameController *pad = NULL;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) {
            pad = SDL_GameControllerOpen(i);
            if (pad) break;
        }

    /* Synchronous first check for deterministic screenshot mode. */
    if (screenshot) {
        if (screenshot_page && !strcmp(screenshot_page, "install")) {
            snprintf(g_status_state, sizeof g_status_state, "MISSING");
            snprintf(g_status_msg, sizeof g_status_msg, "NO GAME FILES FOUND UNDER SOURCE/");
            page = PAGE_INSTALL;
        } else if (screenshot_page && !strcmp(screenshot_page, "padmap")) {
            load_persistent();
            page = PAGE_PADMAP;
        } else if (screenshot_page && !strcmp(screenshot_page, "rebind")) {
            load_persistent();
            page = PAGE_PADMAP;
            capture_action = 0;
        } else if (screenshot_page && !strcmp(screenshot_page, "importing")) {
            snprintf(g_status_msg, sizeof g_status_msg, "COPYING GAMEDATA/CARMODEL");
            g_status_pct = 64;
            page = PAGE_IMPORTING;
        } else if (screenshot_page && !strcmp(screenshot_page, "check")) {
            page = PAGE_CHECK;
        } else if (screenshot_page && !strcmp(screenshot_page, "error")) {
            snprintf(g_status_msg, sizeof g_status_msg, "CANNOT READ SOURCE/PORSCHE.EXE - CHECK THE COPY IS COMPLETE");
            page = PAGE_ERROR;
        } else if (screenshot_page && !strcmp(screenshot_page, "settings")) {
            load_persistent();
            page = PAGE_SETTINGS;
        } else {
            Nfs5InstallState st;
            if (nfs5_verify_data(g_data, &st) == 0) {
                load_persistent();
                page = PAGE_SETTINGS;
            } else {
                snprintf(g_status_state, sizeof g_status_state, "MISSING");
                snprintf(g_status_msg, sizeof g_status_msg, "%s", st.message);
                page = PAGE_INSTALL;
            }
        }
    } else {
        start_check();
    }

    int running = 1, autotest_rc = 3;
    uint32_t last_axis = 0;
    Uint32 last_activity = SDL_GetTicks();
    while (running && !interrupted) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                if (page == PAGE_IMPORTING || page == PAGE_CHECK) stop_worker();
                running = 0;
            } else if (ev.type == SDL_CONTROLLERDEVICEADDED && !pad && SDL_IsGameController(ev.cdevice.which)) {
                pad = SDL_GameControllerOpen(ev.cdevice.which);
            } else if (ev.type == SDL_CONTROLLERDEVICEREMOVED && pad && !SDL_GameControllerGetAttached(pad)) {
                SDL_GameControllerClose(pad);
                pad = NULL;
            } else if (ev.type == SDL_KEYDOWN) {
                int k = ev.key.keysym.sym;
                last_activity = SDL_GetTicks();
                if (k == SDLK_ESCAPE) {
                    if (capture_action >= 0) capture_action = -1;
                    else if (page == PAGE_PADMAP) page = PAGE_SETTINGS;
                    else if (page == PAGE_IMPORTING) { stop_worker(); page = PAGE_INSTALL; }
                    else if (page == PAGE_SETTINGS || page == PAGE_INSTALL || page == PAGE_ERROR) {
                        chosen_launch = 0;
                        running = 0;
                    }
                } else if (page == PAGE_SETTINGS) {
                    if (k == SDLK_UP) move_selection(-1);
                    else if (k == SDLK_DOWN) move_selection(1);
                    else if (k == SDLK_LEFT) change_setting(-1);
                    else if (k == SDLK_RIGHT) change_setting(1);
                    else if (k == SDLK_RETURN || k == SDLK_SPACE) {
                        int r = confirm_settings();
                        if (r == 1 || r == 2) running = 0;
                    }
                } else if (page == PAGE_PADMAP) {
                    if (capture_action >= 0) { capture_action = -1; }
                    else if (k == SDLK_UP) pad_selection = (pad_selection + pad_page_count() - 1) % pad_page_count();
                    else if (k == SDLK_DOWN) pad_selection = (pad_selection + 1) % pad_page_count();
                    else if (k == SDLK_RETURN || k == SDLK_SPACE) {
                        if (pad_selection < NFS5_ACTION_COUNT) capture_action = pad_selection;
                        else if (pad_selection == NFS5_ACTION_COUNT) {
                            nfs5_padmap_defaults(&g_padmap);
                            nfs5_padmap_save(g_config, &g_padmap);
                        } else page = PAGE_SETTINGS;
                    }
                } else if (page == PAGE_INSTALL || page == PAGE_ERROR) {
                    if (k == SDLK_RETURN || k == SDLK_SPACE || k == SDLK_a) {
                        if (page == PAGE_ERROR) start_check();
                        else if (g_status_state[0] && !strcmp(g_status_state, "READY")) start_import();
                        else start_check();
                    } else if (k == SDLK_b) {
                        chosen_launch = 0;
                        running = 0;
                    }
                }
            } else if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                int b = ev.cbutton.button;
                last_activity = SDL_GetTicks();
                if (capture_action >= 0) {
                    if (b >= 0 && b < NFS5_BUTTON_COUNT) {
                        const char *name = nfs5_button_name(b);
                        if (name) {
                            snprintf(g_padmap.button[capture_action], sizeof g_padmap.button[capture_action], "%s", name);
                            nfs5_padmap_save(g_config, &g_padmap);
                        }
                    }
                    capture_action = -1;
                } else if (b == SDL_CONTROLLER_BUTTON_B) {
                    if (page == PAGE_PADMAP) page = PAGE_SETTINGS;
                    else if (page == PAGE_IMPORTING) { stop_worker(); page = PAGE_INSTALL; }
                    else if (page == PAGE_SETTINGS || page == PAGE_INSTALL || page == PAGE_ERROR) {
                        chosen_launch = 0;
                        running = 0;
                    }
                } else if (page == PAGE_SETTINGS) {
                    if (b == SDL_CONTROLLER_BUTTON_DPAD_UP) move_selection(-1);
                    else if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) move_selection(1);
                    else if (b == SDL_CONTROLLER_BUTTON_DPAD_LEFT) change_setting(-1);
                    else if (b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) change_setting(1);
                    else if (b == SDL_CONTROLLER_BUTTON_A) {
                        int r = confirm_settings();
                        if (r == 1 || r == 2) running = 0;
                    }
                } else if (page == PAGE_PADMAP) {
                    if (b == SDL_CONTROLLER_BUTTON_DPAD_UP) pad_selection = (pad_selection + pad_page_count() - 1) % pad_page_count();
                    else if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) pad_selection = (pad_selection + 1) % pad_page_count();
                    else if (b == SDL_CONTROLLER_BUTTON_DPAD_LEFT && pad_selection < NFS5_ACTION_COUNT) {
                        int cur = nfs5_button_index(g_padmap.button[pad_selection]);
                        if (cur < 0) cur = 0;
                        cur = (cur + NFS5_BUTTON_COUNT - 1) % NFS5_BUTTON_COUNT;
                        snprintf(g_padmap.button[pad_selection], sizeof g_padmap.button[pad_selection], "%s",
                                 nfs5_button_name(cur));
                        nfs5_padmap_save(g_config, &g_padmap);
                    } else if (b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT && pad_selection < NFS5_ACTION_COUNT) {
                        int cur = nfs5_button_index(g_padmap.button[pad_selection]);
                        if (cur < 0) cur = -1;
                        cur = (cur + 1) % NFS5_BUTTON_COUNT;
                        snprintf(g_padmap.button[pad_selection], sizeof g_padmap.button[pad_selection], "%s",
                                 nfs5_button_name(cur));
                        nfs5_padmap_save(g_config, &g_padmap);
                    } else if (b == SDL_CONTROLLER_BUTTON_A) {
                        if (pad_selection < NFS5_ACTION_COUNT) capture_action = pad_selection;
                        else if (pad_selection == NFS5_ACTION_COUNT) {
                            nfs5_padmap_defaults(&g_padmap);
                            nfs5_padmap_save(g_config, &g_padmap);
                        } else page = PAGE_SETTINGS;
                    }
                } else if (page == PAGE_INSTALL || page == PAGE_ERROR) {
                    if (b == SDL_CONTROLLER_BUTTON_A) {
                        if (page == PAGE_ERROR) start_check();
                        else if (g_status_state[0] && !strcmp(g_status_state, "READY")) start_import();
                        else start_check();
                    }
                }
            } else if (ev.type == SDL_CONTROLLERAXISMOTION) {
                Sint16 v = ev.caxis.value;
                uint32_t now = SDL_GetTicks();
                if (capture_action < 0 && now - last_axis > 200 && (v < -20000 || v > 20000)) {
                    int dir = v < 0 ? -1 : 1;
                    if (page == PAGE_SETTINGS && ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY)
                        move_selection(dir);
                    else if (page == PAGE_PADMAP && ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY)
                        pad_selection = (pad_selection + dir + pad_page_count()) % pad_page_count();
                    last_axis = now;
                }
            }
        }
        if (!screenshot) {
            poll_worker();
            if ((page == PAGE_CHECK || page == PAGE_IMPORTING) && read_status() == 0)
                apply_status_transition();
            if (toast_frames > 0 && SDL_GetTicks() - last_activity > 120) toast_frames--;
        }
        if (autotest) {
            if (page == PAGE_SETTINGS) {
                /* Verified existing install, or import just finished: success. */
                load_persistent();
                running = 0;
                autotest_rc = 0;
            } else if (page == PAGE_INSTALL && g_status_state[0] &&
                       !strcmp(g_status_state, "READY")) {
                printf("NFS5 AUTOTEST importing from %s\n", g_status_source);
                start_import();
            } else if (page == PAGE_INSTALL && g_status_state[0] &&
                       (!strcmp(g_status_state, "MISSING") || !strcmp(g_status_state, "WRONGVER"))) {
                printf("NFS5 AUTOTEST no supported source: %s\n", g_status_msg);
                running = 0;
                autotest_rc = 3;
            } else if (page == PAGE_ERROR) {
                printf("NFS5 AUTOTEST error: %s\n", g_status_msg);
                running = 0;
                autotest_rc = 3;
            }
            if (SDL_GetTicks() > 180000u) {
                printf("NFS5 AUTOTEST timeout\n");
                running = 0;
                autotest_rc = 4;
            }
        }

        switch (page) {
            case PAGE_CHECK: draw_check_page(ren); break;
            case PAGE_SETTINGS: draw_settings_page(ren); break;
            case PAGE_INSTALL: draw_install_page(ren); break;
            case PAGE_IMPORTING: draw_importing_page(ren); break;
            case PAGE_PADMAP: draw_padmap_page(ren); break;
            case PAGE_ERROR: draw_error_page(ren); break;
            default: break;
        }
        if (capture_action >= 0) draw_capture_overlay(ren);
        if (screenshot) {
            save_screenshot(ren, screenshot);
            SDL_RenderPresent(ren);
            break;
        }
        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }
    stop_worker();
    if (pad) SDL_GameControllerClose(pad);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (screenshot) return 0;
    if (autotest) return autotest_rc;
    if (chosen_launch) {
        if (save_persistent() < 0) {
            fprintf(stderr, "nfs5-launcher: cannot save settings\n");
            return 2;
        }
        return 0;
    }
    return 10;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv) {
    const char *root_override = NULL;
    const char *screenshot = NULL;
    const char *shot_page = NULL;
    const char *source_override = NULL;
    const char *mode = NULL;
    int autotest = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) root_override = argv[++i];
        else if (!strcmp(argv[i], "--source") && i + 1 < argc) source_override = argv[++i];
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
        else if (!strcmp(argv[i], "--page") && i + 1 < argc) shot_page = argv[++i];
        else if (!strcmp(argv[i], "--verify") || !strcmp(argv[i], "--selftest") ||
                 !strcmp(argv[i], "--import")) mode = argv[i];
        else if (!strcmp(argv[i], "--autotest")) autotest = 1;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: %s [--root DIR] [--verify | --selftest | --import [--source DIR] | --autotest] [--screenshot FILE.bmp [--page settings|install]]\n",
                   argv[0]);
            return 0;
        } else {
            fprintf(stderr, "nfs5-launcher: unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (resolve_paths(root_override) < 0) {
        fprintf(stderr, "nfs5-launcher: cannot use port root (not writable, or path too long)\n");
        return 2;
    }
    if (mode && !strcmp(mode, "--verify")) return run_verify_mode();
    if (mode && !strcmp(mode, "--import")) return run_import_mode(source_override);
    if (mode && !strcmp(mode, "--selftest")) {
        load_persistent();
        return run_selftest();
    }
    return gui_main(screenshot, shot_page, autotest);
}

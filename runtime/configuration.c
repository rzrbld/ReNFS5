/* NFS5 port configuration; see runtime/configuration.h. */
#define _POSIX_C_SOURCE 200809L
#include "configuration.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *frame_cap_text(int cap) {
    if (cap == NFS5_FRAME_CAP_30) return "30";
    if (cap == NFS5_FRAME_CAP_60) return "60";
    return "0";
}

void nfs5_settings_defaults(Nfs5Settings *s) {
    memset(s, 0, sizeof *s);
    s->frame_cap = NFS5_FRAME_CAP_60;
    s->fps_counter = 0;
    s->detail = NFS5_DETAIL_MEDIUM;
    s->resolution = NFS5_RES_640x480;
    s->movies = 1;
    s->aspect = NFS5_ASPECT_4_3;
}

const char *nfs5_detail_name(int detail) {
    switch (detail) {
        case NFS5_DETAIL_LOW: return "LOW";
        case NFS5_DETAIL_HIGH: return "HIGH";
        default: return "MEDIUM";
    }
}

const char *nfs5_aspect_name(int aspect) { return aspect == NFS5_ASPECT_STRETCH ? "STRETCH" : "4:3"; }

const char *nfs5_movies_name(int movies) { return movies ? "ON" : "OFF"; }

const char *nfs5_resolution_name(int resolution) {
    switch (resolution) {
        case NFS5_RES_800x600: return "800x600";
        case NFS5_RES_1024x768: return "1024x768";
        default: return "640x480";
    }
}

static int make_dir(const char *path) {
    if (mkdir(path, 0755) == 0 || errno == EEXIST) return 0;
    return -1;
}

static int settings_path(const char *config_dir, const char *suffix, char *out, size_t size) {
    int n = snprintf(out, size, "%s/%s%s", config_dir, NFS5_SETTINGS_FILE, suffix);
    return (n < 0 || (size_t)n >= size) ? -1 : 0;
}

static char *trim(char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    char *e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return p;
}

int nfs5_settings_load(const char *config_dir, Nfs5Settings *s) {
    char path[PATH_MAX];
    if (!config_dir || !*config_dir) { errno = EINVAL; return -1; }
    if (settings_path(config_dir, "", path, sizeof path)) { errno = ENAMETOOLONG; return -1; }
    FILE *f = fopen(path, "r");
    if (!f) return errno == ENOENT ? 0 : -1;
    Nfs5Settings candidate;
    nfs5_settings_defaults(&candidate);
    char line[256];
    int ok = 1;
    while (fgets(line, sizeof line, f)) {
        if (!strchr(line, '\n') && !feof(f)) { ok = 0; break; }
        char *p = trim(line);
        if (!*p || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) { ok = 0; break; }
        *eq = 0;
        char *key = trim(p), *val = trim(eq + 1);
        if (!strcmp(key, "frame_cap")) {
            if (!strcmp(val, "0")) candidate.frame_cap = 0;
            else if (!strcmp(val, "30")) candidate.frame_cap = 30;
            else if (!strcmp(val, "60")) candidate.frame_cap = 60;
            else { ok = 0; break; }
        } else if (!strcmp(key, "fps_counter")) {
            if (!strcmp(val, "0")) candidate.fps_counter = 0;
            else if (!strcmp(val, "1")) candidate.fps_counter = 1;
            else { ok = 0; break; }
        } else if (!strcmp(key, "detail")) {
            if (!strcmp(val, "low")) candidate.detail = NFS5_DETAIL_LOW;
            else if (!strcmp(val, "medium")) candidate.detail = NFS5_DETAIL_MEDIUM;
            else if (!strcmp(val, "high")) candidate.detail = NFS5_DETAIL_HIGH;
            else { ok = 0; break; }
        } else if (!strcmp(key, "resolution")) {
            if (!strcmp(val, "640x480")) candidate.resolution = NFS5_RES_640x480;
            else if (!strcmp(val, "800x600")) candidate.resolution = NFS5_RES_800x600;
            else if (!strcmp(val, "1024x768")) candidate.resolution = NFS5_RES_1024x768;
            else { ok = 0; break; }
        } else if (!strcmp(key, "movies")) {
            if (!strcmp(val, "on") || !strcmp(val, "1")) candidate.movies = 1;
            else if (!strcmp(val, "off") || !strcmp(val, "0")) candidate.movies = 0;
            else { ok = 0; break; }
        } else if (!strcmp(key, "aspect")) {
            if (!strcmp(val, "4:3")) candidate.aspect = NFS5_ASPECT_4_3;
            else if (!strcmp(val, "stretch")) candidate.aspect = NFS5_ASPECT_STRETCH;
            else { ok = 0; break; }
        } else {
            ok = 0;
            break;
        }
    }
    if (ferror(f)) ok = 0;
    fclose(f);
    if (!ok) { errno = EINVAL; return -1; }
    *s = candidate;
    return 1;
}

int nfs5_settings_save(const char *config_dir, const Nfs5Settings *s) {
    char path[PATH_MAX], temp[PATH_MAX];
    if (!config_dir || !*config_dir) { errno = EINVAL; return -1; }
    if (make_dir(config_dir) < 0) return -1;
    if (settings_path(config_dir, "", path, sizeof path) ||
        settings_path(config_dir, ".tmp", temp, sizeof temp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (s->frame_cap != 0 && s->frame_cap != 30 && s->frame_cap != 60) { errno = EINVAL; return -1; }
    if (s->fps_counter < 0 || s->fps_counter > 1) { errno = EINVAL; return -1; }
    if (s->detail < 0 || s->detail >= NFS5_DETAIL_COUNT) { errno = EINVAL; return -1; }
    if (s->resolution < 0 || s->resolution >= NFS5_RES_COUNT) { errno = EINVAL; return -1; }
    if (s->movies < 0 || s->movies > 1) { errno = EINVAL; return -1; }
    if (s->aspect < 0 || s->aspect >= NFS5_ASPECT_COUNT) { errno = EINVAL; return -1; }
    FILE *f = fopen(temp, "w");
    if (!f) return -1;
    fprintf(f,
            "# NFS5 port settings (written by nfs5-launcher; safe to edit)\n"
            "# frame_cap: 30 | 60 | 0 (uncapped). 30 is recommended on H700.\n"
            "frame_cap=%s\nfps_counter=%d\ndetail=%s\nresolution=%s\nmovies=%s\naspect=%s\n",
            frame_cap_text(s->frame_cap), s->fps_counter,
            s->detail == NFS5_DETAIL_LOW ? "low" : s->detail == NFS5_DETAIL_HIGH ? "high" : "medium",
            nfs5_resolution_name(s->resolution),
            s->movies ? "on" : "off",
            s->aspect == NFS5_ASPECT_STRETCH ? "stretch" : "4:3");
    int failed = fflush(f) != 0;
    if (!failed && fsync(fileno(f))) failed = 1;
    if (fclose(f)) failed = 1;
    if (failed) { unlink(temp); return -1; }
    if (rename(temp, path)) { unlink(temp); return -1; }
    return 0;
}

int nfs5_settings_apply_env(const char *config_dir, const Nfs5Settings *s) {
    char buf[32];
    snprintf(buf, sizeof buf, "%s", frame_cap_text(s->frame_cap));
    setenv("NFS5_FRAME_CAP", buf, 1);
    snprintf(buf, sizeof buf, "%d", s->fps_counter ? 1 : 0);
    setenv("NFS5_FPS_OVERLAY", buf, 1);
    const char *detail = s->detail == NFS5_DETAIL_LOW ? "low" : s->detail == NFS5_DETAIL_HIGH ? "high" : "medium";
    setenv("NFS5_DETAIL", detail, 1);
    setenv("NFS5_VIDEO_MODE", nfs5_resolution_name(s->resolution), 1);
    setenv("NFS5_PLAY_MOVIES", s->movies ? "1" : "0", 1);
    setenv("NFS5_SKIP_MOVIES", s->movies ? "0" : "1", 1);
    setenv("NFS5_ASPECT", s->aspect == NFS5_ASPECT_STRETCH ? "stretch" : "4:3", 1);
    if (config_dir && *config_dir && !getenv("NFS5_PAD_MAP")) {
        char path[PATH_MAX];
        int n = snprintf(path, sizeof path, "%s/%s", config_dir, NFS5_PADMAP_FILE);
        if (n > 0 && (size_t)n < sizeof path) setenv("NFS5_PAD_MAP", path, 1);
    }
    return 0;
}

/* ---- controller mapping ------------------------------------------------ */

/* Index == SDL_GameControllerButton ordering; keep in sync with SDL2. */
static const char *const button_names[NFS5_BUTTON_COUNT] = {
    "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick",
    "leftshoulder", "rightshoulder", "dp_up", "dp_down", "dp_left", "dp_right"};

static const char *const action_names[NFS5_ACTION_COUNT] = {
    "throttle", "brake", "steer_left", "steer_right", "handbrake",
    "shift_up", "shift_down", "camera", "horn", "pause"};

static const char *const action_defaults[NFS5_ACTION_COUNT] = {
    "rightshoulder", "leftshoulder", "dp_left", "dp_right", "a",
    "b", "x", "y", "rightstick", "start"};

const char *nfs5_action_name(int index) {
    return (index >= 0 && index < NFS5_ACTION_COUNT) ? action_names[index] : NULL;
}

const char *nfs5_button_name(int sdl_button) {
    return (sdl_button >= 0 && sdl_button < NFS5_BUTTON_COUNT) ? button_names[sdl_button] : NULL;
}

int nfs5_button_index(const char *name) {
    for (int i = 0; i < NFS5_BUTTON_COUNT; i++)
        if (!strcmp(name, button_names[i])) return i;
    return -1;
}

void nfs5_padmap_defaults(Nfs5PadMap *m) {
    memset(m, 0, sizeof *m);
    for (int i = 0; i < NFS5_ACTION_COUNT; i++)
        snprintf(m->button[i], sizeof m->button[i], "%s", action_defaults[i]);
}

static int padmap_path(const char *config_dir, const char *suffix, char *out, size_t size) {
    int n = snprintf(out, size, "%s/%s%s", config_dir, NFS5_PADMAP_FILE, suffix);
    return (n < 0 || (size_t)n >= size) ? -1 : 0;
}

int nfs5_padmap_load(const char *config_dir, Nfs5PadMap *m) {
    char path[PATH_MAX];
    if (!config_dir || !*config_dir) { errno = EINVAL; return -1; }
    if (padmap_path(config_dir, "", path, sizeof path)) { errno = ENAMETOOLONG; return -1; }
    FILE *f = fopen(path, "r");
    if (!f) return errno == ENOENT ? 0 : -1;
    Nfs5PadMap candidate;
    nfs5_padmap_defaults(&candidate);
    char line[128];
    int ok = 1;
    while (fgets(line, sizeof line, f)) {
        if (!strchr(line, '\n') && !feof(f)) { ok = 0; break; }
        char *p = trim(line);
        if (!*p || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) { ok = 0; break; }
        *eq = 0;
        char *key = trim(p), *val = trim(eq + 1);
        int action = -1;
        for (int i = 0; i < NFS5_ACTION_COUNT; i++)
            if (!strcmp(key, action_names[i])) { action = i; break; }
        if (action < 0 || nfs5_button_index(val) < 0) { ok = 0; break; }
        snprintf(candidate.button[action], sizeof candidate.button[action], "%s", val);
    }
    if (ferror(f)) ok = 0;
    fclose(f);
    if (!ok) { errno = EINVAL; return -1; }
    *m = candidate;
    return 1;
}

int nfs5_padmap_save(const char *config_dir, const Nfs5PadMap *m) {
    char path[PATH_MAX], temp[PATH_MAX];
    if (!config_dir || !*config_dir) { errno = EINVAL; return -1; }
    if (make_dir(config_dir) < 0) return -1;
    if (padmap_path(config_dir, "", path, sizeof path) ||
        padmap_path(config_dir, ".tmp", temp, sizeof temp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    FILE *f = fopen(temp, "w");
    if (!f) return -1;
    fprintf(f, "# NFS5 controller mapping: action=controller button name\n");
    fprintf(f, "# Valid buttons: a b x y back guide start leftstick rightstick\n");
    fprintf(f, "#                leftshoulder rightshoulder dp_up dp_down dp_left dp_right\n");
    for (int i = 0; i < NFS5_ACTION_COUNT; i++) {
        if (nfs5_button_index(m->button[i]) < 0) { fclose(f); unlink(temp); errno = EINVAL; return -1; }
        fprintf(f, "%s=%s\n", action_names[i], m->button[i]);
    }
    int failed = fflush(f) != 0;
    if (!failed && fsync(fileno(f))) failed = 1;
    if (fclose(f)) failed = 1;
    if (failed) { unlink(temp); return -1; }
    if (rename(temp, path)) { unlink(temp); return -1; }
    return 0;
}

/* ---- diagnostics ------------------------------------------------------- */

int nfs_platform_configure(int argc, char **argv) {
    (void)argc;
    (void)argv;
    const char *frame_cap = getenv("NFS5_FRAME_CAP");
    const char *overlay = getenv("NFS5_FPS_OVERLAY");
    const char *detail = getenv("NFS5_DETAIL");
    const char *video = getenv("NFS5_VIDEO_MODE");
    const char *movies = getenv("NFS5_PLAY_MOVIES");
    const char *aspect = getenv("NFS5_ASPECT");
    const char *pad = getenv("NFS5_PAD_MAP");
    fprintf(stderr,
            "[nfs5] settings frame_cap=%s fps_overlay=%s detail=%s video=%s movies=%s aspect=%s pad_map=%s\n"
            "[nfs5] the settings UI is nfs5-launcher.aarch64 (config/launcher.cfg)\n",
            frame_cap ? frame_cap : "60", overlay ? overlay : "0", detail ? detail : "medium",
            video ? video : "640x480", movies ? movies : "1", aspect ? aspect : "4:3", pad ? pad : "(none)");
    return 0;
}

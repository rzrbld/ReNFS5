/* NFS5 port configuration shared by the launcher and (as diagnostics) the game.
 *
 * File format: <root>/config/launcher.cfg, one key=value per line, '#' comments.
 *   frame_cap=30|60|0     (0 = uncapped)
 *   fps_counter=0|1
 *   detail=low|medium|high
 *   aspect=4:3|stretch
 *
 * The launcher writes this file and the Ports shell script exports the same
 * values as environment variables before starting the game:
 *   NFS5_FRAME_CAP, NFS5_FPS_OVERLAY, NFS5_DETAIL, NFS5_ASPECT, NFS5_PAD_MAP.
 * That keeps the game binary free of any UI dependency and lets a shell user
 * override a setting for one run without touching the file.
 */
#ifndef NFS5_CONFIGURATION_H
#define NFS5_CONFIGURATION_H

#include <stddef.h>

#define NFS5_SETTINGS_FILE "launcher.cfg"
#define NFS5_PADMAP_FILE "controller.cfg"

#define NFS5_DETAIL_LOW 0
#define NFS5_DETAIL_MEDIUM 1
#define NFS5_DETAIL_HIGH 2
#define NFS5_DETAIL_COUNT 3

#define NFS5_ASPECT_4_3 0
#define NFS5_ASPECT_STRETCH 1
#define NFS5_ASPECT_COUNT 2

#define NFS5_RES_640x480 0
#define NFS5_RES_800x600 1
#define NFS5_RES_1024x768 2
#define NFS5_RES_COUNT 3

#define NFS5_FRAME_CAP_UNCAPPED 0
#define NFS5_FRAME_CAP_60 60
#define NFS5_FRAME_CAP_30 30

typedef struct {
    int frame_cap;   /* 0, 30 or 60 */
    int fps_counter; /* 0/1 */
    int detail;      /* NFS5_DETAIL_* */
    int resolution;  /* NFS5_RES_* */
    int movies;      /* 1 = play EA-MAD movies in-process, 0 = skip them */
    int aspect;      /* NFS5_ASPECT_* */
    char screen[16]; /* "" = the firmware's default, else a display connector (e.g. "DSI-1") */
} Nfs5Settings;

void nfs5_settings_defaults(Nfs5Settings *s);
/* 1 = loaded, 0 = absent (defaults kept), -1 = invalid file (settings untouched). */
int nfs5_settings_load(const char *config_dir, Nfs5Settings *s);
/* 0 on success; file is replaced atomically. */
int nfs5_settings_save(const char *config_dir, const Nfs5Settings *s);
/* Export NFS5_FRAME_CAP / NFS5_FPS_OVERLAY / NFS5_DETAIL / NFS5_ASPECT and
 * NFS5_PAD_MAP (unless already set). Returns 0. */
int nfs5_settings_apply_env(const char *config_dir, const Nfs5Settings *s);
const char *nfs5_detail_name(int detail);
const char *nfs5_aspect_name(int aspect);
const char *nfs5_resolution_name(int resolution);   /* "640x480" etc. */
const char *nfs5_movies_name(int movies);           /* "ON" / "OFF" */

/* Action names and their default controller bindings. The map file is read by
 * the launcher for remapping and by the game's DirectInput HLE layer; the
 * canonical button names are the lowercase SDL_GameControllerButton names. */
#define NFS5_ACTION_COUNT 10
typedef struct {
    char button[NFS5_ACTION_COUNT][24];
} Nfs5PadMap;

void nfs5_padmap_defaults(Nfs5PadMap *m);
/* 1 = loaded, 0 = absent (defaults kept), -1 = invalid (map untouched). */
int nfs5_padmap_load(const char *config_dir, Nfs5PadMap *m);
int nfs5_padmap_save(const char *config_dir, const Nfs5PadMap *m);
const char *nfs5_action_name(int index);
/* SDL_GameControllerButton value 0..14 -> canonical name, else NULL. */
const char *nfs5_button_name(int sdl_button);
int nfs5_button_index(const char *name); /* -1 if unknown */
#define NFS5_BUTTON_COUNT 15

/* Diagnostics hook used by runtime/main.c for `nfs5.aarch64 --configure`:
 * prints the effective settings and returns 0. It is not a settings UI. */
int nfs_platform_configure(int argc, char **argv);

#endif

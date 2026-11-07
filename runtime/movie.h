/* movie.h - in-process EA-MAD movie player (video + audio) for the port.
 *
 * The game's own MAD player (lifted guest JPEG/MMX code + its async I/O
 * layer) is fragile on this port (docs/KNOWN_ISSUES.md), so when a *.mad file
 * is opened and movies are enabled the HLE layer plays it here instead: the
 * embedded minimal FFmpeg (demuxer ea, decoders eamad + adpcm_ea_r1) decodes
 * frames, SDL presents YUV directly in the existing game window and the audio
 * goes through the host stream mixer. No host ffmpeg binary and no shell
 * script is involved.
 *
 * Env:
 *   NFS5_PLAY_MOVIES=0   disable playback (the file is then hidden as before)
 *   NFS5_MOVIE_TRACE=1   per-frame diagnostics
 *   NFS5_MOVIE_MAX_SEC=N optional diagnostic wall-clock cap (default disabled)
 */
#ifndef NFS5_MOVIE_H
#define NFS5_MOVIE_H

/* 1 when this build can play movies and playback is enabled. */
int nfs5_movie_available(void);

/* Decode and present one movie from a host path. Returns presented frames, or
 * -1 when unavailable/failed. The caller treats the movie as consumed either
 * way (the game falls back to its "file missing" path). */
int nfs5_movie_play(const char *host_path);

#endif

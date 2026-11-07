/*
 * dsound.c - DirectSound on SDL2 audio: DirectSoundCreate / DirectSoundCreate8
 * and the IDirectSound / IDirectSoundBuffer / IDirectSound3DBuffer /
 * IDirectSound3DListener / IKsPropertySet interfaces, with a software mixer.
 *
 * A secondary buffer is guest memory the game Locks and writes, as it would be
 * on Windows; the mixer (SDL's audio thread) reads it at the buffer's
 * frequency, volume, pan and (for a 3D buffer) 3D position, and moves its play
 * cursor in real time, so a game that streams into a looping buffer by
 * watching GetCurrentPosition keeps doing so. The primary buffer is its
 * format and its volume; everything is mixed at the device's rate. One mutex
 * covers the buffers' state between the mixer and the shims.
 *
 * NFS5 (Porsche Unleashed, 2000) is the reason the 3D interfaces are here:
 * Porsche.exe's SND sound engine (strings "SNDAUTHOR: ... V4.1g", "SCHlu",
 * "SCDlu", "=BNKltD3") creates the primary buffer with DSBCAPS_CTRL3D, creates
 * its voices as 3D-secondary buffers (DSBCAPS_CTRL3D | DSBCAPS_LOCHARDWARE |
 * DSBCAPS_GLOBALFOCUS) and QueryInterfaces each buffer for
 * IID_IDirectSound3DBuffer, failing DirectSound init outright if that QI
 * fails; it detects EAX through IID_IKsPropertySet on a small 11 kHz mono
 * probe buffer. Feed the mixer PCM (8/16-bit, mono/stereo) and it plays.
 *
 * With no audio device (SDL_AUDIODRIVER=dummy, a headless box) SDL's dummy
 * driver still runs the callback in real time, so the cursors still move.
 *
 * Evidence / trace: NFS5_TRACE_AUDIO=1 logs every call (method, arguments,
 * decoded WAVEFORMATEX and DSBUFFERDESC flags) to stderr, or to the file named
 * by NFS5_TRACE_AUDIO_FILE. NFS5_AUDIO_RECORD=<file.wav> records the mixed
 * output (debug feature; does file I/O from the audio callback).
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <SDL2/SDL.h>
#include "win32hle.h"

/* ---- DirectSound results and flags ---- */
#define DS_OK                    0u
#define E_NOINTERFACE            0x80004002u
#define E_NOTIMPL                0x80004001u
#define DSERR_INVALIDPARAM       0x80070057u
#define DSERR_NODRIVER           0x88780078u
#define DSERR_INVALIDCALL        0x88780032u
#define DSERR_PRIOLEVELNEEDED    0x88780046u
#define DSERR_CONTROLUNAVAIL     0x8878001Eu
#define DSERR_ALREADYINITIALIZED 0x88780082u

#define DSBCAPS_PRIMARYBUFFER    0x00000001u
#define DSBCAPS_STATIC           0x00000002u
#define DSBCAPS_LOCHARDWARE      0x00000004u
#define DSBCAPS_LOCSOFTWARE      0x00000008u
#define DSBCAPS_CTRL3D           0x00000010u
#define DSBCAPS_CTRLFREQUENCY    0x00000020u
#define DSBCAPS_CTRLPAN          0x00000040u
#define DSBCAPS_CTRLVOLUME       0x00000080u
#define DSBCAPS_CTRLPOSITIONNOTIFY 0x00000100u
#define DSBCAPS_CTRLFX           0x00000200u
#define DSBCAPS_GLOBALFOCUS      0x00008000u
#define DSBCAPS_GETCURRENTPOSITION2 0x00010000u
#define DSBCAPS_LOCDEFER         0x00040000u

#define DSBPLAY_LOOPING          0x00000001u
#define DSBSTATUS_PLAYING        0x00000001u
#define DSBSTATUS_BUFFERLOST     0x00000002u
#define DSBSTATUS_LOOPING        0x00000004u
#define DSBLOCK_FROMWRITECURSOR  0x00000001u
#define DSBLOCK_ENTIREBUFFER     0x00000002u

#define DS3D_IMMEDIATE           0u
#define DS3D_DEFERRED            1u
#define DS3DMODE_NORMAL          0u
#define DS3DMODE_DISABLE         1u
#define DS3DMODE_HEADRELATIVE    2u

/* DSCAPS */
#define DSCAPS_PRIMARYMONO       0x0001u
#define DSCAPS_PRIMARYSTEREO     0x0002u
#define DSCAPS_PRIMARY8BIT       0x0004u
#define DSCAPS_PRIMARY16BIT      0x0008u
#define DSCAPS_SECONDARYMONO     0x0010u
#define DSCAPS_SECONDARYSTEREO   0x0020u
#define DSCAPS_SECONDARY8BIT     0x0040u
#define DSCAPS_SECONDARY16BIT    0x0080u
#define DSCAPS_EMULDRIVER        0x0200u
#define DSCAPS_CONTINUOUSRATE    0x1000u

#define DSSPEAKER_STEREO         4u

#define OUT_RATE 44100

/* ---- trace: NFS5_TRACE_AUDIO=1 (or a file name) ---- */
static int g_trace = -1;
static FILE *g_trace_fp;
static int trace_on(void) {
    if (g_trace < 0) {
        const char *e = getenv("NFS5_TRACE_AUDIO");
        g_trace = e && *e && strcmp(e, "0") != 0;
        if (g_trace) {
            const char *f = getenv("NFS5_TRACE_AUDIO_FILE");
            g_trace_fp = f && *f ? fopen(f, "a") : stderr;
            if (!g_trace_fp) g_trace_fp = stderr;
        }
    }
    return g_trace;
}
static void atr(const char *fmt, ...) {
    if (!trace_on()) return;
    va_list ap;
    FILE *f = g_trace_fp ? g_trace_fp : stderr;
    va_start(ap, fmt);
    flockfile(f);
    fprintf(f, "[dsound %8u] ", hle_ticks_ms());
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    fflush(f);
    funlockfile(f);
    va_end(ap);
}
static const char *result_name(uint32_t hr) {
    switch (hr) {
    case DS_OK: return "DS_OK";
    case DSERR_INVALIDPARAM: return "DSERR_INVALIDPARAM";
    case DSERR_NODRIVER: return "DSERR_NODRIVER";
    case DSERR_INVALIDCALL: return "DSERR_INVALIDCALL";
    case DSERR_PRIOLEVELNEEDED: return "DSERR_PRIOLEVELNEEDED";
    case DSERR_CONTROLUNAVAIL: return "DSERR_CONTROLUNAVAIL";
    case DSERR_ALREADYINITIALIZED: return "DSERR_ALREADYINITIALIZED";
    case E_NOINTERFACE: return "E_NOINTERFACE";
    case E_NOTIMPL: return "E_NOTIMPL";
    default: return "?";
    }
}
static const char *fmt_tag_name(unsigned tag) {
    switch (tag) {
    case 1: return "PCM";
    case 2: return "MS-ADPCM";
    case 0x11: return "IMA-ADPCM";
    case 0x31: return "GSM610";
    case 0x55: return "MP3";
    case 0xFFFE: return "EXTENSIBLE";
    default: return "other";
    }
}
/* a WAVEFORMATEX the guest pointed at, decoded into a trace line */
static void atr_wfx(const char *what, uint32_t wfx) {
    if (!trace_on() || !wfx) return;
    unsigned tag = MEM16(wfx), ch = MEM16(wfx + 2), bits = MEM16(wfx + 14), align = MEM16(wfx + 12);
    uint32_t rate = MEM32(wfx + 4), avg = MEM32(wfx + 8);
    unsigned cb = 0;
    if (tag == 1 || tag == 2 || tag == 0x11) cb = MEM16(wfx + 16);
    atr("%s WAVEFORMATEX tag=%u(%s) ch=%u rate=%u bits=%u block=%u avg=%u cbSize=%u",
        what, tag, fmt_tag_name(tag), ch, rate, bits, align, avg, cb);
}
static void atr_caps(const char *what, uint32_t flags) {
    if (!trace_on()) return;
    char buf[256];
    int n = snprintf(buf, sizeof buf, "flags=%#x(", flags);
#define CAPF(b, s) do { if (flags & (b)) n += snprintf(buf + n, sizeof buf - (size_t)n, "%s ", s); } while (0)
    CAPF(DSBCAPS_PRIMARYBUFFER, "PRIMARY");
    CAPF(DSBCAPS_STATIC, "STATIC");
    CAPF(DSBCAPS_LOCHARDWARE, "LOCHARDWARE");
    CAPF(DSBCAPS_LOCSOFTWARE, "LOCSOFTWARE");
    CAPF(DSBCAPS_CTRL3D, "CTRL3D");
    CAPF(DSBCAPS_CTRLFREQUENCY, "CTRLFREQUENCY");
    CAPF(DSBCAPS_CTRLPAN, "CTRLPAN");
    CAPF(DSBCAPS_CTRLVOLUME, "CTRLVOLUME");
    CAPF(DSBCAPS_CTRLPOSITIONNOTIFY, "CTRLPOSITIONNOTIFY");
    CAPF(DSBCAPS_CTRLFX, "CTRLFX");
    CAPF(DSBCAPS_GLOBALFOCUS, "GLOBALFOCUS");
    CAPF(DSBCAPS_GETCURRENTPOSITION2, "GETCURRENTPOSITION2");
    CAPF(DSBCAPS_LOCDEFER, "LOCDEFER");
#undef CAPF
    snprintf(buf + n, sizeof buf - (size_t)n, ")");
    atr("%s %s", what, buf);
}

/* ---- buffer state (host side); the samples themselves are guest memory ---- */
typedef struct dsbuf {
    uint8_t *data;              /* the samples, guest-visible */
    int *data_refs;             /* shared with duplicates */
    uint32_t bytes, flags;
    int channels, bits, rate, block;
    int playing, looping, primary;
    double pos;                 /* the play cursor, in frames */
    int volume, pan;            /* hundredths of a dB; -10000..10000 */
    float gl, gr;
    uint8_t wfx[40];            /* the format the game asked for (GetFormat) */
    uint32_t wfxlen;
    uint32_t slot;
    uint32_t obj;               /* IDirectSoundBuffer object (guest VA) */
    uint32_t w3d, wks, wlst;    /* interface objects handed out by QI, cached */
    int dead;                   /* the buffer interface's refcount reached 0 */
    /* IDirectSound3DBuffer state */
    float px, py, pz;
    float vx, vy, vz;
    uint32_t mode;
    float min_dist, max_dist;
    long cone_outside_volume;
    uint32_t cone_inside, cone_outside;
    float cox, coy, coz;
    int is3d;                   /* QI'd as an IDirectSound3DBuffer */
} dsbuf;

static pthread_mutex_t g_mix = PTHREAD_MUTEX_INITIALIZER;
#define MAX_BUFS 256
static dsbuf *g_buf[MAX_BUFS];
static SDL_AudioDeviceID g_dev;
static int g_audio_ok = -1;
static float g_master = 1.0f;
static float g_primary_gain = 1.0f;      /* IDirectSoundBuffer::SetVolume on the primary */
void hle_dsound_set_master(float gain) { g_master = gain; }
/* Last mixer callback's per-channel RMS (0..1), for tests/evidence. Host-side
 * debug only; benign racy float reads, no locks added to the callback. */
static volatile float g_dbg_l, g_dbg_r;
void hle_dsound_debug_levels(float *left, float *right) {
    if (left) *left = g_dbg_l;
    if (right) *right = g_dbg_r;
}

/* The one 3D listener (DirectSound has one per device; the game may QueryInterface
 * it from the primary buffer; its default is at the origin facing +Z). */
static struct {
    float px, py, pz;
    float fx, fy, fz;
    float ux, uy, uz;
    float dist_factor, doppler, rolloff;
    uint32_t obj;
    int dead;
} g_list = { 0, 0, 0, 0, 0, 1, 0, 1, 0, 1.0f, 1.0f, 1.0f, 0, 0 };

/* ---- the mixer ---- */
static void gains(dsbuf *b) {
    float v = b->volume <= -10000 ? 0.0f : powf(10.0f, (float)b->volume / 2000.0f);
    int pan = b->pan;
    if (b->is3d && b->mode != DS3DMODE_DISABLE) {
        /* Listener-relative position, DirectSound's left-handed frame:
         * +X right, +Y up, +Z forward. right = top x front. */
        float dx = b->px, dy = b->py, dz = b->pz;
        if (b->mode != DS3DMODE_HEADRELATIVE) {
            dx -= g_list.px, dy -= g_list.py, dz -= g_list.pz;
        }
        float rx = g_list.uy * g_list.fz - g_list.uz * g_list.fy;
        float ry = g_list.uz * g_list.fx - g_list.ux * g_list.fz;
        float rz = g_list.ux * g_list.fy - g_list.uy * g_list.fx;
        float lx = dx * rx + dy * ry + dz * rz;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        if (dist < 0.001f) dist = 0.001f;
        float att = 1.0f;
        if (dist > b->min_dist) {
            att = b->min_dist / dist;
            if (b->max_dist > b->min_dist && dist > b->max_dist) att = 0.0f;
        }
        v *= att;
        pan = (int)(lx / dist * 10000.0f);    /* +X is the listener's right; DS pan +10000 is right */
        if (pan > 10000) pan = 10000;
        if (pan < -10000) pan = -10000;
    }
    float l = 1.0f, r = 1.0f;
    if (pan < 0) r = pan <= -10000 ? 0.0f : powf(10.0f, (float)pan / 2000.0f);
    if (pan > 0) l = pan >= 10000 ? 0.0f : powf(10.0f, (float)-pan / 2000.0f);
    b->gl = v * l, b->gr = v * r;
}

static inline float sample(const dsbuf *b, uint32_t frame, int ch) {
    const uint8_t *p = b->data + (size_t)frame * (uint32_t)b->block + (b->channels > 1 ? (size_t)ch * (uint32_t)(b->bits / 8) : 0);
    return b->bits == 16 ? (float)*(const int16_t *)p / 32768.0f : ((float)*p - 128.0f) / 128.0f;
}

static int audio_open(void);
static int audio_open_once(void) { return audio_open(); }

/* Host streams: PCM a host pushes (a movie's sound), mixed with the
 * buffers. A ring of interleaved s16 at the stream's rate, played at that
 * rate. */
#define MAX_STREAMS 4
#define STREAM_FRAMES (1 << 17)
static struct { int used, channels, rate; float gain; int16_t *ring; uint32_t rd, wr; double pos; } g_stream[MAX_STREAMS];
int hle_audio_stream_open(int rate, int channels) {
    if (!audio_open_once()) return -1;
    pthread_mutex_lock(&g_mix);
    int id = -1;
    for (int i = 0; i < MAX_STREAMS && id < 0; i++)
        if (!g_stream[i].used) {
            id = i;
            g_stream[i].ring = (int16_t *)calloc(STREAM_FRAMES * 2, 2);
            g_stream[i].used = 1, g_stream[i].channels = channels, g_stream[i].rate = rate, g_stream[i].gain = 1.0f;
            g_stream[i].rd = g_stream[i].wr = 0, g_stream[i].pos = 0;
        }
    pthread_mutex_unlock(&g_mix);
    return id;
}
void hle_audio_stream_push(int id, const int16_t *pcm, int frames) {
    if (id < 0 || id >= MAX_STREAMS) return;
    pthread_mutex_lock(&g_mix);
    for (int f = 0; f < frames && g_stream[id].wr - g_stream[id].rd < STREAM_FRAMES - 1; f++, g_stream[id].wr++) {
        uint32_t k = g_stream[id].wr % STREAM_FRAMES;
        g_stream[id].ring[2 * k] = pcm[f * g_stream[id].channels];
        g_stream[id].ring[2 * k + 1] = pcm[f * g_stream[id].channels + (g_stream[id].channels > 1)];
    }
    pthread_mutex_unlock(&g_mix);
}
int hle_audio_stream_queued(int id) {                    /* frames not yet played */
    if (id < 0 || id >= MAX_STREAMS) return 0;
    pthread_mutex_lock(&g_mix);
    int n = (int)(g_stream[id].wr - g_stream[id].rd);
    pthread_mutex_unlock(&g_mix);
    return n;
}
void hle_audio_stream_gain(int id, float gain) { if (id >= 0 && id < MAX_STREAMS) g_stream[id].gain = gain; }
void hle_audio_stream_close(int id) {
    if (id < 0 || id >= MAX_STREAMS) return;
    pthread_mutex_lock(&g_mix);
    free(g_stream[id].ring);
    memset(&g_stream[id], 0, sizeof g_stream[id]);
    pthread_mutex_unlock(&g_mix);
}

/* ---- mixed-output recording (NFS5_AUDIO_RECORD=<file.wav>): a debug feature
 * for capturing what the game actually feeds the mixer, so "non-silent" is a
 * measurement, not a claim. Only off by default; does buffered stdio from the
 * callback, which is fine for a capture run and never for a shipped default. */
static FILE *g_rec;
static uint32_t g_rec_data;
static int g_rec_ready;
static void wav_header(FILE *f, uint32_t data_bytes) {
    uint8_t h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E','f','m','t',' ',
                      16,0,0,0, 1,0, 2,0, 0x44,0xAC,0,0, 0x10,0xB1,0x02,0, 4,0, 16,0,
                      'd','a','t','a', 0,0,0,0 };
    uint32_t sz = data_bytes + 36;
    memcpy(h + 4, &sz, 4);
    memcpy(h + 40, &data_bytes, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}
static void rec_close(void) {
    if (!g_rec) return;
    wav_header(g_rec, g_rec_data);                 /* the real size goes in the header now */
    fclose(g_rec);
    g_rec = NULL;
}
static void rec_open(void) {
    if (g_rec_ready) return;
    g_rec_ready = 1;
    atexit(rec_close);
    const char *p = getenv("NFS5_AUDIO_RECORD");
    if (!p || !*p) return;
    g_rec = fopen(p, "wb");
    if (!g_rec) { fprintf(stderr, "[dsound] cannot record to %s\n", p); return; }
    wav_header(g_rec, 0);
    atr("recording mixed output to %s", p);
}
static void rec_write(const int16_t *pcm, int samples) {
    if (!g_rec) return;
    fwrite(pcm, 2, (size_t)samples, g_rec);
    g_rec_data += (uint32_t)samples * 2;
    /* keep the header's sizes current: a game that exits through ExitProcess
     * (not exit()) never runs atexit, and the capture should still be a valid
     * WAV. Every 16 callbacks (~10 ms each) is plenty. */
    static unsigned n;
    if ((++n & 15) == 0) {
        long end = ftell(g_rec);
        wav_header(g_rec, g_rec_data);
        fseek(g_rec, end, SEEK_SET);
        fflush(g_rec);
    }
}

static void mix(void *ud, Uint8 *stream, int len) {
    (void)ud;
    int frames = len / 4;
    static float acc[2 * 8192];
    if (frames > 8192) frames = 8192;
    memset(acc, 0, sizeof(float) * 2 * (size_t)frames);
    pthread_mutex_lock(&g_mix);
    for (int i = 0; i < MAX_BUFS; i++) {
        dsbuf *b = g_buf[i];
        if (!b || !b->playing || b->primary || !b->block) continue;
        if (b->is3d) gains(b);
        uint32_t nframes = b->bytes / (uint32_t)b->block;
        if (!nframes) continue;
        double step = (double)b->rate / OUT_RATE;
        for (int f = 0; f < frames && b->playing; f++) {
            uint32_t at = (uint32_t)b->pos;
            if (at >= nframes) {
                if (b->looping) { b->pos -= nframes; at = (uint32_t)b->pos; if (at >= nframes) b->pos = 0, at = 0; }
                else { b->playing = 0; b->pos = 0; break; }
            }
            float l = sample(b, at, 0), r = b->channels > 1 ? sample(b, at, 1) : l;
            acc[2 * f] += l * b->gl;
            acc[2 * f + 1] += r * b->gr;
            b->pos += step;
        }
    }
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (!g_stream[i].used) continue;
        double step = (double)g_stream[i].rate / OUT_RATE;
        for (int f = 0; f < frames && g_stream[i].rd < g_stream[i].wr; f++) {
            uint32_t k = g_stream[i].rd % STREAM_FRAMES;
            acc[2 * f] += g_stream[i].ring[2 * k] / 32768.0f * g_stream[i].gain;
            acc[2 * f + 1] += g_stream[i].ring[2 * k + 1] / 32768.0f * g_stream[i].gain;
            g_stream[i].pos += step;
            while (g_stream[i].pos >= 1.0 && g_stream[i].rd < g_stream[i].wr) g_stream[i].pos -= 1.0, g_stream[i].rd++;
        }
    }
    float master = g_master * g_primary_gain;
    pthread_mutex_unlock(&g_mix);
    int16_t *out = (int16_t *)stream;
    for (int k = 0; k < 2 * frames; k++) {
        float v = acc[k] * master * 32767.0f;
        out[k] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
    }
    if (g_rec) rec_write(out, 2 * frames);
    if (frames > 0) {
        double el = 0, er = 0;
        for (int f = 0; f < frames; f++) {
            double l = out[2 * f] / 32768.0, r = out[2 * f + 1] / 32768.0;
            el += l * l, er += r * r;
        }
        g_dbg_l = (float)sqrt(el / frames);
        g_dbg_r = (float)sqrt(er / frames);
    }
}

static int audio_open(void) {
    if (g_audio_ok >= 0) return g_audio_ok;
    g_audio_ok = 0;
    rec_open();
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");      /* SIGINT and SIGTERM end the process, not WM_CLOSE */
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = OUT_RATE, want.format = AUDIO_S16SYS, want.channels = 2, want.samples = 1024, want.callback = mix;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_dev) {
        /* No sound card (a server, a container): a silent device whose
         * cursors still move, rather than DirectSound failing. A game whose
         * sound DLL takes the failure for granted (Civilization III's
         * sound.dll) would fault on it. */
        fprintf(stderr, "[dsound] no audio device (%s): silent\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (!g_dev) { fprintf(stderr, "[dsound] no audio: %s\n", SDL_GetError()); return 0; }
    }
    SDL_PauseAudioDevice(g_dev, 0);
    fprintf(stderr, "[dsound] %s, %d Hz\n", SDL_GetCurrentAudioDriver(), have.freq);
    return g_audio_ok = 1;
}

/* ======================================================================
 * objects: [vtable][refcount][slot][kind]
 * A buffer object is the IDirectSoundBuffer interface; the 3D buffer, the
 * IKsPropertySet and the 3D listener handed out by QueryInterface are their
 * own guest objects with their own vtables and refcounts, all pointing at the
 * same dsbuf slot. A dsbuf lives until the buffer interface AND every
 * interface QueryInterface handed out from it are released.
 * ====================================================================== */
#define SLOT(self) MEM32((self) + 8)
#define KIND(self) MEM32((self) + 12)
enum { W_3D = 1, W_KS = 2, W_LISTENER = 3 };

static dsbuf *buf_slot(uint32_t slot) { return slot < MAX_BUFS ? g_buf[slot] : NULL; }
static dsbuf *buf_of(uint32_t self) { return buf_slot(SLOT(self)); }

static uint32_t g_vt_ds, g_vt_buf, g_vt_3d, g_vt_ks, g_vt_list;
static uint32_t new_object(uint32_t vtable, uint32_t slot, uint32_t kind) {
    uint32_t o = hle_com_new(vtable, 16);
    MEM32(o + 8) = slot;
    MEM32(o + 12) = kind;
    return o;
}

/* free a dsbuf once no interface points at it any more */
static void maybe_free(dsbuf *b) {
    if (!b || !b->dead || b->w3d || b->wks || b->wlst) return;
    pthread_mutex_lock(&g_mix);
    if (b->slot < MAX_BUFS && g_buf[b->slot] == b) g_buf[b->slot] = NULL;
    pthread_mutex_unlock(&g_mix);
    if (b->obj) hle_com_free(b->obj);
    if (--*b->data_refs == 0) { hle_free(b->data); free(b->data_refs); }
    free(b);
}

/* interface objects handed out by QueryInterface, created once per buffer and
 * cached; the cache is under g_mix because the game may QI/Release from more
 * than one thread while the mixer walks the buffers */
static uint32_t ensure_w3d(dsbuf *b) {
    pthread_mutex_lock(&g_mix);
    int existed = b->w3d != 0;
    if (!existed) b->w3d = new_object(g_vt_3d, b->slot, W_3D), b->is3d = 1;
    uint32_t r = b->w3d;
    if (existed) MEM32(r + 4)++;
    pthread_mutex_unlock(&g_mix);
    atr("QueryInterface(buffer %u, IID_IDirectSound3DBuffer) -> %#x", b->slot, r);
    return r;
}
static uint32_t ensure_wks(dsbuf *b) {
    pthread_mutex_lock(&g_mix);
    int existed = b->wks != 0;
    if (!existed) b->wks = new_object(g_vt_ks, b->slot, W_KS);
    uint32_t r = b->wks;
    if (existed) MEM32(r + 4)++;
    pthread_mutex_unlock(&g_mix);
    atr("QueryInterface(buffer %u, IID_IKsPropertySet) -> %#x (EAX properties reported unsupported)", b->slot, r);
    return r;
}
static uint32_t ensure_wlst(dsbuf *b) {
    pthread_mutex_lock(&g_mix);
    if (!g_list.obj) g_list.obj = new_object(g_vt_list, 0xFFFFFFFFu, W_LISTENER);
    else MEM32(g_list.obj + 4)++;
    uint32_t r = g_list.obj;
    pthread_mutex_unlock(&g_mix);
    atr("QueryInterface(buffer %u, IID_IDirectSound3DListener) -> %#x", b->slot, r);
    return r;
}

/* ---- GUIDs (little-endian bytes, as the guest passes them) ---- */
static const uint8_t IID_IUnknown[16]      = { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 };
static const uint8_t IID_IDirectSound[16]  = { 0x83,0xFA,0x9A,0x27,0x81,0x49,0xCE,0x11,0xA5,0x21,0x00,0x20,0xAF,0x0B,0xE5,0x60 };
static const uint8_t IID_IDirectSound8[16] = { 0x89,0x55,0xC4,0x79,0x40,0x8C,0x6F,0x4A,0x9A,0x4C,0x2D,0x4B,0x4B,0x2B,0xB3,0x95 };
static const uint8_t IID_IDirectSoundBuffer[16]  = { 0x85,0xFA,0x9A,0x27,0x81,0x49,0xCE,0x11,0xA5,0x21,0x00,0x20,0xAF,0x0B,0xE5,0x60 };
static const uint8_t IID_IDirectSoundBuffer8[16] = { 0x49,0xA4,0x25,0x68,0x24,0x75,0x82,0x4D,0x92,0x00,0x50,0xDC,0x6C,0x4B,0x66,0xEB };
static const uint8_t IID_IDirectSound3DBuffer[16]  = { 0x86,0xFA,0x9A,0x27,0x81,0x49,0xCE,0x11,0xA5,0x21,0x00,0x20,0xAF,0x0B,0xE5,0x60 };
static const uint8_t IID_IDirectSound3DListener[16] = { 0x84,0xFA,0x9A,0x27,0x81,0x49,0xCE,0x11,0xA5,0x21,0x00,0x20,0xAF,0x0B,0xE5,0x60 };
static const uint8_t IID_IKsPropertySet[16] = { 0x30,0xAC,0xEF,0x31,0x5C,0x51,0xD0,0x11,0xA9,0xAA,0x00,0xAA,0x00,0x61,0xBE,0x93 };
static const uint8_t IID_IDirectSoundNotify[16] = { 0x83,0x07,0x21,0xB0,0xCD,0x89,0xD0,0x11,0xAF,0x08,0x00,0xA0,0xC9,0x25,0xCD,0x16 };

static int iid_is(uint32_t iid, const uint8_t *want) {
    return iid && !memcmp(hle_ptr(iid), want, 16);
}
/* the DirectSound object answers IUnknown/IDirectSound/IDirectSound8 with itself */
static void d_QueryInterface(void) {
    uint32_t self = A32(0), out = A32(2);
    if (out) MEM32(out) = 0;
    if (iid_is(A32(1), IID_IUnknown) || iid_is(A32(1), IID_IDirectSound) || iid_is(A32(1), IID_IDirectSound8)) {
        MEM32(self + 4)++;
        if (out) MEM32(out) = self;
        RET(DS_OK, 3);
    }
    atr("IDirectSound::QueryInterface: E_NOINTERFACE (not IDirectSound/IDirectSound8)");
    RET(E_NOINTERFACE, 3);
}
/* the buffer interface: IUnknown/IDirectSoundBuffer/IDirectSoundBuffer8 itself,
 * IDirectSound3DBuffer/IKsPropertySet/IDirectSound3DListener as separate
 * interface objects on the same slot (overriding ole32.c's generic self-QI,
 * which would hand a 3D buffer's vtable calls to the buffer's methods). */
static void b_QueryInterface(void) {
    uint32_t self = A32(0), iid = A32(1), out = A32(2);
    dsbuf *b = buf_of(self);
    uint32_t r = 0;
    if (out) MEM32(out) = 0;
    if (!b) { atr("IDirectSoundBuffer::QueryInterface on a dead buffer"); RET(E_NOINTERFACE, 3); }
    if (iid_is(iid, IID_IUnknown) || iid_is(iid, IID_IDirectSoundBuffer) || iid_is(iid, IID_IDirectSoundBuffer8)) {
        MEM32(self + 4)++;
        r = self;
    } else if (iid_is(iid, IID_IDirectSound3DBuffer)) {
        r = ensure_w3d(b);
    } else if (iid_is(iid, IID_IKsPropertySet)) {
        r = ensure_wks(b);
    } else if (iid_is(iid, IID_IDirectSound3DListener)) {
        r = ensure_wlst(b);
    } else if (iid_is(iid, IID_IDirectSoundNotify)) {
        atr("IDirectSoundBuffer::QueryInterface(IDirectSoundNotify): E_NOINTERFACE (the game polls GetCurrentPosition, it does not use notifications)");
        RET(E_NOINTERFACE, 3);
    }
    if (!r) {
        atr("IDirectSoundBuffer::QueryInterface(%08x%08x...): E_NOINTERFACE", iid ? MEM32(iid + 4) : 0, iid ? MEM32(iid) : 0);
        RET(E_NOINTERFACE, 3);
    }
    if (out) MEM32(out) = r;
    RET(DS_OK, 3);
}
/* a 3D/ks/listener interface object: what it does not serve goes back to the
 * buffer it came from */
static void w_QueryInterface(void) {
    uint32_t self = A32(0), iid = A32(1), out = A32(2), kind = KIND(self);
    dsbuf *b = NULL;
    if (out) MEM32(out) = 0;
    if (kind == W_LISTENER && g_list.obj == self) {
        if (iid_is(iid, IID_IUnknown) || iid_is(iid, IID_IDirectSound3DListener)) {
            MEM32(self + 4)++;
            if (out) MEM32(out) = self;
            RET(DS_OK, 3);
        }
    } else if (kind == W_3D) {
        if (iid_is(iid, IID_IUnknown) || iid_is(iid, IID_IDirectSound3DBuffer)) {
            MEM32(self + 4)++;
            if (out) MEM32(out) = self;
            RET(DS_OK, 3);
        }
    } else if (kind == W_KS) {
        if (iid_is(iid, IID_IUnknown) || iid_is(iid, IID_IKsPropertySet)) {
            MEM32(self + 4)++;
            if (out) MEM32(out) = self;
            RET(DS_OK, 3);
        }
    }
    b = buf_of(self);
    if (b) {
        uint32_t r = 0;
        if (iid_is(iid, IID_IDirectSoundBuffer) || iid_is(iid, IID_IDirectSoundBuffer8)) {
            MEM32(b->obj + 4)++;
            r = b->obj;
        } else if (iid_is(iid, IID_IDirectSound3DBuffer)) r = ensure_w3d(b);
        else if (iid_is(iid, IID_IKsPropertySet)) r = ensure_wks(b);
        else if (iid_is(iid, IID_IDirectSound3DListener)) r = ensure_wlst(b);
        if (r) { if (out) MEM32(out) = r; RET(DS_OK, 3); }
    }
    atr("wrapper(%u)::QueryInterface: E_NOINTERFACE", kind);
    RET(E_NOINTERFACE, 3);
}

/* decrement an interface object's refcount; free it (and possibly the slot) at 0 */
static uint32_t release_wrapper(uint32_t self) {
    uint32_t kind = KIND(self), n = --MEM32(self + 4);
    if (n) return n;
    if (kind == W_LISTENER) {
        hle_com_free(self);
        g_list.obj = 0, g_list.dead = 1;
    } else {
        dsbuf *b = buf_of(self);
        if (b) {
            pthread_mutex_lock(&g_mix);
            if (kind == W_3D) b->w3d = 0;
            else if (kind == W_KS) b->wks = 0;
            pthread_mutex_unlock(&g_mix);
            hle_com_free(self);
            maybe_free(b);
        } else hle_com_free(self);
    }
    return 0;
}
static void w_Release(void) { RET(release_wrapper(A32(0)), 1); }

/* buffer interface release: never freed while a wrapper lives */
static void b_Release(void) {
    uint32_t self = A32(0), n = --MEM32(self + 4);
    { dsbuf *b = buf_of(self); atr("IDirectSoundBuffer::Release(obj=%#x slot %u) refs=%u%s", self, b ? b->slot : 999u, n, n ? "" : " -> freed"); }
    if (!n) {
        dsbuf *b = buf_of(self);
        if (b) {
            pthread_mutex_lock(&g_mix);
            b->dead = 1;
            pthread_mutex_unlock(&g_mix);
            maybe_free(b);
        } else hle_com_free(self);
    }
    RET(n, 1);
}

/* ======================================================================
 * IDirectSoundBuffer
 * ====================================================================== */
static void b_GetCaps(void) {                            /* (this, &DSBCAPS) */
    dsbuf *b = buf_of(A32(0));
    uint32_t c = A32(1);
    if (!b || !c) RET(DSERR_INVALIDPARAM, 2);
    MEM32(c + 4) = b->flags | (b->primary ? 0u : DSBCAPS_LOCSOFTWARE);
    MEM32(c + 8) = b->bytes;
    MEM32(c + 12) = 0;                                   /* dwUnlockTransferRate */
    MEM32(c + 16) = 0;                                   /* dwPlayCpuOverhead */
    atr("IDirectSoundBuffer::GetCaps(slot %u) -> %s bytes=%u", b->slot, result_name(DS_OK), b->bytes);
    RET(DS_OK, 2);
}
static void b_GetCurrentPosition(void) {                 /* (this, &play, &write) */
    dsbuf *b = buf_of(A32(0));
    if (!b) RET(DSERR_INVALIDPARAM, 3);
    pthread_mutex_lock(&g_mix);
    uint32_t play = b->block ? (uint32_t)b->pos * (uint32_t)b->block : 0;
    uint32_t lead = b->block ? (uint32_t)(b->rate / 100) * (uint32_t)b->block : 0;   /* 10 ms ahead */
    int playing = b->playing;
    uint32_t bytes = b->bytes;
    pthread_mutex_unlock(&g_mix);
    if (bytes) play %= bytes;
    if (A32(1)) MEM32(A32(1)) = play;
    if (A32(2)) MEM32(A32(2)) = bytes ? (play + (playing ? lead : 0)) % bytes : 0;
    if (trace_on()) {
        static unsigned long n;
        if ((n++ % 60) == 0)
            atr("IDirectSoundBuffer::GetCurrentPosition(slot %u) -> play=%u write=%u%s", b->slot, play,
                bytes ? (play + (playing ? lead : 0)) % bytes : 0, playing ? " (playing)" : "");
    }
    RET(DS_OK, 3);
}
static void b_GetFormat(void) {                          /* (this, &wfx, size, &written) */
    dsbuf *b = buf_of(A32(0));
    uint32_t w = A32(1), size = A32(2);
    if (!b) RET(DSERR_INVALIDPARAM, 4);
    if (w && size) {
        uint32_t n = b->wfxlen && b->wfxlen <= size ? b->wfxlen : size < 18 ? size : 18;
        memcpy(hle_ptr(w), b->wfx, n);
    }
    if (A32(3)) MEM32(A32(3)) = b->wfxlen ? b->wfxlen : 18;
    atr("IDirectSoundBuffer::GetFormat(slot %u, size=%u)", b->slot, size);
    RET(DS_OK, 4);
}
static void b_GetVolume(void)    { dsbuf *b = buf_of(A32(0)); if (A32(1) && b) MEM32(A32(1)) = (uint32_t)b->volume; RET(b ? DS_OK : DSERR_INVALIDPARAM, 2); }
static void b_GetPan(void)       { dsbuf *b = buf_of(A32(0)); if (A32(1) && b) MEM32(A32(1)) = (uint32_t)b->pan; RET(b ? DS_OK : DSERR_INVALIDPARAM, 2); }
static void b_GetFrequency(void) { dsbuf *b = buf_of(A32(0)); if (A32(1) && b) MEM32(A32(1)) = (uint32_t)b->rate; RET(b ? DS_OK : DSERR_INVALIDPARAM, 2); }
static void b_GetStatus(void) {
    dsbuf *b = buf_of(A32(0));
    if (trace_on()) { static unsigned long n; if ((n++ % 60) == 0) atr("IDirectSoundBuffer::GetStatus(slot %u) status=%#x", b ? b->slot : 999u, b ? (uint32_t)((b->playing ? 1 : 0) | (b->playing && b->looping ? 4 : 0)) : 0u); }
    if (A32(1) && b) MEM32(A32(1)) = (b->playing ? DSBSTATUS_PLAYING : 0u) | (b->playing && b->looping ? DSBSTATUS_LOOPING : 0u);
    RET(b ? DS_OK : DSERR_INVALIDPARAM, 2);
}
static void b_Initialize(void) { RET(DSERR_ALREADYINITIALIZED, 3); }   /* already initialized by CreateSoundBuffer */
static void b_Lock(void) {                               /* (this, offset, bytes, &p1, &n1, &p2, &n2, flags) */
    dsbuf *b = buf_of(A32(0));
    if (!b) RET(DSERR_INVALIDPARAM, 8);
    if (b->primary) RET(DSERR_PRIOLEVELNEEDED, 8);
    uint32_t off = A32(1), n = A32(2), fl = A32(7);
    if (fl & DSBLOCK_ENTIREBUFFER) off = 0, n = b->bytes;
    else if (fl & DSBLOCK_FROMWRITECURSOR) {
        pthread_mutex_lock(&g_mix);
        off = b->block ? ((uint32_t)b->pos * (uint32_t)b->block + (uint32_t)(b->rate / 100) * (uint32_t)b->block) % b->bytes : 0;
        pthread_mutex_unlock(&g_mix);
    }
    if (off >= b->bytes || n > b->bytes) RET(DSERR_INVALIDPARAM, 8);
    uint32_t n1 = off + n <= b->bytes ? n : b->bytes - off;
    if (A32(3)) MEM32(A32(3)) = hle_va(b->data + off);
    if (A32(4)) MEM32(A32(4)) = n1;
    if (A32(5)) MEM32(A32(5)) = n1 < n ? hle_va(b->data) : 0;
    if (A32(6)) MEM32(A32(6)) = n - n1;
    if (trace_on()) {
        static unsigned long ncall;
        if ((ncall++ % 120) == 0)
            atr("IDirectSoundBuffer::Lock(slot %u, off=%u, bytes=%u, flags=%#x) -> p1=%#x n1=%u p2=%#x n2=%u", b->slot, A32(1), A32(2), fl, hle_va(b->data + off), n1, n1 < n ? hle_va(b->data) : 0, n - n1);
    }
    RET(DS_OK, 8);
}
static void b_Unlock(void) { RET(DS_OK, 5); }            /* (this, p1, n1, p2, n2): the memory is the buffer */
static void b_Play(void) {                               /* (this, reserved, priority, flags) */
    dsbuf *b = buf_of(A32(0));
    if (!b) RET(DSERR_INVALIDPARAM, 4);
    pthread_mutex_lock(&g_mix);
    b->playing = 1, b->looping = (A32(3) & DSBPLAY_LOOPING) != 0;
    pthread_mutex_unlock(&g_mix);
    atr("IDirectSoundBuffer::Play(slot %u, flags=%#x%s)", b->slot, A32(3), (A32(3) & DSBPLAY_LOOPING) ? " LOOPING" : "");
    RET(DS_OK, 4);
}
static void b_Stop(void) {
    dsbuf *b = buf_of(A32(0));
    if (!b) RET(DSERR_INVALIDPARAM, 1);
    pthread_mutex_lock(&g_mix);
    b->playing = 0;
    pthread_mutex_unlock(&g_mix);
    atr("IDirectSoundBuffer::Stop(slot %u)", b->slot);
    RET(DS_OK, 1);
}
static void b_SetCurrentPosition(void) {
    dsbuf *b = buf_of(A32(0));
    if (!b || !b->block) RET(DSERR_INVALIDPARAM, 2);
    pthread_mutex_lock(&g_mix);
    b->pos = (double)((A32(1) % (b->bytes ? b->bytes : 1)) / (uint32_t)b->block);
    pthread_mutex_unlock(&g_mix);
    atr("IDirectSoundBuffer::SetCurrentPosition(slot %u, %u) -> frame %.3f", b->slot, A32(1), b->pos);
    RET(DS_OK, 2);
}
/* the primary's format: mixed at the device rate anyway; a secondary's
 * format is fixed by its DSBUFFERDESC on Windows, so SetFormat there fails */
static void b_SetFormat(void) {
    dsbuf *b = buf_of(A32(0));
    uint32_t w = A32(1);
    if (!b || !w) RET(DSERR_INVALIDPARAM, 2);
    if (!b->primary) { atr("IDirectSoundBuffer::SetFormat on a secondary buffer: DSERR_INVALIDCALL"); RET(DSERR_INVALIDCALL, 2); }
    unsigned tag = MEM16(w), ch = MEM16(w + 2), bits = MEM16(w + 14), align = MEM16(w + 12);
    uint32_t rate = MEM32(w + 4);
    atr_wfx("IDirectSoundBuffer::SetFormat(primary)", w);
    if (tag != 1 || (bits != 8 && bits != 16) || ch < 1 || ch > 2 || !rate) {
        atr("IDirectSoundBuffer::SetFormat(primary): tag=%u bits=%u ch=%u rate=%u is not PCM this mixer plays -> DSERR_INVALIDPARAM",
            tag, bits, ch, rate);
        RET(DSERR_INVALIDPARAM, 2);
    }
    b->channels = ch, b->bits = bits, b->rate = (int)rate, b->block = align ? (int)align : (int)(ch * bits / 8);
    uint32_t len = 18;
    if (A32(2) >= 18 && tag == 0xFFFE) len = MEM16(w + 16) + 18;
    if (len > sizeof b->wfx) len = sizeof b->wfx;
    if (len >= 18) memcpy(b->wfx, hle_ptr(w), len);
    b->wfxlen = len;
    RET(DS_OK, 2);
}
static void b_SetVolume(void) {
    dsbuf *b = buf_of(A32(0));
    if (!b) RET(DSERR_INVALIDPARAM, 2);
    int v = (int)A32(1);
    if (v < -10000) v = -10000;
    if (v > 0) v = 0;
    if (b->primary) {
        pthread_mutex_lock(&g_mix);
        g_primary_gain = v <= -10000 ? 0.0f : powf(10.0f, (float)v / 2000.0f);
        pthread_mutex_unlock(&g_mix);
        atr("IDirectSoundBuffer::SetVolume(primary, %d) -> primary gain %.3f", v, g_primary_gain);
    } else {
        pthread_mutex_lock(&g_mix);
        b->volume = v;
        gains(b);
        pthread_mutex_unlock(&g_mix);
        atr("IDirectSoundBuffer::SetVolume(slot %u, %d)", b->slot, v);
    }
    RET(DS_OK, 2);
}
static void b_SetPan(void) {
    dsbuf *b = buf_of(A32(0));
    if (!b) RET(DSERR_INVALIDPARAM, 2);
    int v = (int)A32(1);
    if (v < -10000) v = -10000;
    if (v > 10000) v = 10000;
    pthread_mutex_lock(&g_mix);
    b->pan = v;
    gains(b);
    pthread_mutex_unlock(&g_mix);
    atr("IDirectSoundBuffer::SetPan(slot %u, %d)", b->slot, v);
    RET(DS_OK, 2);
}
static void b_SetFrequency(void) {
    dsbuf *b = buf_of(A32(0));
    if (!b) RET(DSERR_INVALIDPARAM, 2);
    pthread_mutex_lock(&g_mix);
    if (A32(1)) b->rate = (int)A32(1);
    pthread_mutex_unlock(&g_mix);
    atr("IDirectSoundBuffer::SetFrequency(slot %u, %u)", b->slot, A32(1));
    RET(DS_OK, 2);
}
static void b_Restore(void) {                            /* never lost in this mixer */
    dsbuf *b = buf_of(A32(0));
    if (!b) RET(DSERR_INVALIDPARAM, 1);
    RET(DS_OK, 1);
}

/* ======================================================================
 * IDirectSound3DBuffer (vtable order includes Get/SetConeOrientation, which
 * shifts SetPosition to slot 19 / byte 0x4C — the offset Porsche.exe calls
 * with (x, y=0, z, DS3D_IMMEDIATE)).
 * ====================================================================== */
static float farg(int i) { float f; uint32_t u = A32(i); memcpy(&f, &u, 4); return f; }
static void fput(uint32_t va, float f) { if (va) memcpy(hle_ptr(va), &f, 4); }
static dsbuf *b3_of(void) {
    uint32_t self = A32(0);
    if (KIND(self) != W_3D) return NULL;
    return buf_of(self);
}
static void b3_GetAllParameters(void) {                  /* (this, &DS3DBUFFER) */
    dsbuf *b = b3_of();
    uint32_t p = A32(1);
    if (!b || !p) RET(DSERR_INVALIDPARAM, 2);
    uint32_t size = MEM32(p);
    memset(hle_ptr(p + 4), 0, size > 4 ? size - 4 : 0);
    fput(p + 4, b->px), fput(p + 8, b->py), fput(p + 12, b->pz);
    fput(p + 16, b->vx), fput(p + 20, b->vy), fput(p + 24, b->vz);
    MEM32(p + 28) = b->cone_inside, MEM32(p + 32) = b->cone_outside;
    fput(p + 36, b->cox), fput(p + 40, b->coy), fput(p + 44, b->coz);
    MEM32(p + 48) = (uint32_t)b->cone_outside_volume;
    fput(p + 52, b->min_dist), fput(p + 56, b->max_dist);
    MEM32(p + 60) = b->mode;
    RET(DS_OK, 2);
}
static void b3_GetConeAngles(void) {
    dsbuf *b = b3_of();
    if (!b) RET(DSERR_INVALIDPARAM, 3);
    if (A32(1)) MEM32(A32(1)) = b->cone_inside;
    if (A32(2)) MEM32(A32(2)) = b->cone_outside;
    RET(DS_OK, 3);
}
static void b3_GetConeOrientation(void) {
    dsbuf *b = b3_of();
    uint32_t p = A32(1);
    if (!b) RET(DSERR_INVALIDPARAM, 2);
    fput(p, b->cox), fput(p + 4, b->coy), fput(p + 8, b->coz);
    RET(DS_OK, 2);
}
static void b3_GetConeOutsideVolume(void) {
    dsbuf *b = b3_of();
    if (!b) RET(DSERR_INVALIDPARAM, 2);
    if (A32(1)) MEM32(A32(1)) = (uint32_t)b->cone_outside_volume;
    RET(DS_OK, 2);
}
static void b3_GetMaxDistance(void) { dsbuf *b = b3_of(); if (!b) RET(DSERR_INVALIDPARAM, 2); fput(A32(1), b->max_dist); RET(DS_OK, 2); }
static void b3_GetMinDistance(void) { dsbuf *b = b3_of(); if (!b) RET(DSERR_INVALIDPARAM, 2); fput(A32(1), b->min_dist); RET(DS_OK, 2); }
static void b3_GetMode(void)        { dsbuf *b = b3_of(); if (!b) RET(DSERR_INVALIDPARAM, 2); if (A32(1)) MEM32(A32(1)) = b->mode; RET(DS_OK, 2); }
static void b3_GetPosition(void) {
    dsbuf *b = b3_of();
    uint32_t p = A32(1);
    if (!b) RET(DSERR_INVALIDPARAM, 2);
    fput(p, b->px), fput(p + 4, b->py), fput(p + 8, b->pz);
    RET(DS_OK, 2);
}
static void b3_GetVelocity(void) {
    dsbuf *b = b3_of();
    uint32_t p = A32(1);
    if (!b) RET(DSERR_INVALIDPARAM, 2);
    fput(p, b->vx), fput(p + 4, b->vy), fput(p + 8, b->vz);
    RET(DS_OK, 2);
}
static void b3_SetAllParameters(void) {                  /* (this, &DS3DBUFFER, apply) */
    dsbuf *b = b3_of();
    uint32_t p = A32(1);
    if (!b || !p) RET(DSERR_INVALIDPARAM, 3);
    pthread_mutex_lock(&g_mix);
    memcpy(&b->px, hle_ptr(p + 4), 12);
    memcpy(&b->vx, hle_ptr(p + 16), 12);
    b->cone_inside = MEM32(p + 28), b->cone_outside = MEM32(p + 32);
    memcpy(&b->cox, hle_ptr(p + 36), 12);
    b->cone_outside_volume = (long)(int32_t)MEM32(p + 48);
    memcpy(&b->min_dist, hle_ptr(p + 52), 8);
    b->mode = MEM32(p + 60);
    gains(b);
    pthread_mutex_unlock(&g_mix);
    atr("IDirectSound3DBuffer::SetAllParameters(slot %u) pos=(%.3f,%.3f,%.3f) min=%.3f max=%.3f mode=%u", b->slot, b->px, b->py, b->pz, b->min_dist, b->max_dist, b->mode);
    RET(DS_OK, 3);
}
static void b3_SetConeAngles(void) {
    dsbuf *b = b3_of();
    if (!b) RET(DSERR_INVALIDPARAM, 4);
    pthread_mutex_lock(&g_mix);
    b->cone_inside = A32(1), b->cone_outside = A32(2);
    gains(b);
    pthread_mutex_unlock(&g_mix);
    RET(DS_OK, 4);
}
static void b3_SetConeOrientation(void) {
    dsbuf *b = b3_of();
    if (!b) RET(DSERR_INVALIDPARAM, 5);
    pthread_mutex_lock(&g_mix);
    b->cox = farg(1), b->coy = farg(2), b->coz = farg(3);
    pthread_mutex_unlock(&g_mix);
    RET(DS_OK, 5);
}
static void b3_SetConeOutsideVolume(void) {
    dsbuf *b = b3_of();
    if (!b) RET(DSERR_INVALIDPARAM, 3);
    pthread_mutex_lock(&g_mix);
    b->cone_outside_volume = (long)(int32_t)A32(1);
    pthread_mutex_unlock(&g_mix);
    RET(DS_OK, 3);
}
static void b3_SetMaxDistance(void) {
    dsbuf *b = b3_of();
    if (!b) RET(DSERR_INVALIDPARAM, 3);
    pthread_mutex_lock(&g_mix);
    b->max_dist = farg(1);
    gains(b);
    pthread_mutex_unlock(&g_mix);
    atr("IDirectSound3DBuffer::SetMaxDistance(slot %u, %.3f, apply=%u)", b->slot, b->max_dist, A32(2));
    RET(DS_OK, 3);
}
static void b3_SetMinDistance(void) {
    dsbuf *b = b3_of();
    if (!b) RET(DSERR_INVALIDPARAM, 3);
    pthread_mutex_lock(&g_mix);
    b->min_dist = farg(1);
    gains(b);
    pthread_mutex_unlock(&g_mix);
    atr("IDirectSound3DBuffer::SetMinDistance(slot %u, %.3f, apply=%u)", b->slot, b->min_dist, A32(2));
    RET(DS_OK, 3);
}
static void b3_SetMode(void) {
    dsbuf *b = b3_of();
    if (!b) RET(DSERR_INVALIDPARAM, 3);
    pthread_mutex_lock(&g_mix);
    b->mode = A32(1);
    gains(b);
    pthread_mutex_unlock(&g_mix);
    atr("IDirectSound3DBuffer::SetMode(slot %u, %u, apply=%u)", b->slot, b->mode, A32(2));
    RET(DS_OK, 3);
}
static void b3_SetPosition(void) {                       /* (this, x, y, z, apply) */
    dsbuf *b = b3_of();
    if (!b) RET(DSERR_INVALIDPARAM, 5);
    pthread_mutex_lock(&g_mix);
    b->px = farg(1), b->py = farg(2), b->pz = farg(3);
    gains(b);
    pthread_mutex_unlock(&g_mix);
    atr("IDirectSound3DBuffer::SetPosition(slot %u, %.3f, %.3f, %.3f, apply=%u)", b->slot, b->px, b->py, b->pz, A32(4));
    RET(DS_OK, 5);
}
static void b3_SetVelocity(void) {
    dsbuf *b = b3_of();
    if (!b) RET(DSERR_INVALIDPARAM, 5);
    pthread_mutex_lock(&g_mix);
    b->vx = farg(1), b->vy = farg(2), b->vz = farg(3);
    pthread_mutex_unlock(&g_mix);
    RET(DS_OK, 5);
}

/* ======================================================================
 * IKsPropertySet: EAX is detected by Porsche.exe through this on a 1-voice
 * probe buffer. We report "known property set, unsupported properties" (which
 * is exactly true: the mixer has no EAX reverb), so the game takes its
 * non-EAX path; the QI itself succeeds because the game dereferences the
 * result on the EAX path and would fault on a failed QI.
 * ====================================================================== */
static dsbuf *wks_slot_obj(void) {
    if (KIND(A32(0)) != W_KS) return NULL;
    return buf_of(A32(0));
}
static void ks_Get(void)        { atr("IKsPropertySet::Get: E_NOTIMPL (no property sets are emulated)"); RET(E_NOTIMPL, 7); }
static void ks_Set(void)        { atr("IKsPropertySet::Set: E_NOTIMPL (no property sets are emulated)"); RET(E_NOTIMPL, 7); }
static void ks_QuerySupport(void) {                      /* (this, guidSet, id, &support) */
    dsbuf *b = wks_slot_obj();
    uint32_t out = A32(3);
    if (!b) RET(DSERR_INVALIDPARAM, 4);
    if (trace_on()) {
        char g[40];
        uint32_t iid = A32(1);
        if (iid) hle_guid_string(hle_ptr(iid), g); else strcpy(g, "(null)");
        atr("IKsPropertySet::QuerySupport(slot %u, set=%s, id=%u) -> S_OK, support=0 (unsupported)", b->slot, g, A32(2));
    }
    if (out) MEM32(out) = 0;                             /* no bits set: not supported */
    RET(DS_OK, 4);
}

/* ======================================================================
 * IDirectSound3DListener (the device's one; Porsche.exe does not QueryInterface
 * it, but a 3D buffer implies it and it is cheap to serve correctly)
 * ====================================================================== */
static void l_GetAllParameters(void) {                   /* (this, &DS3DLISTENER) */
    uint32_t p = A32(1);
    if (!p) RET(DSERR_INVALIDPARAM, 2);
    uint32_t size = MEM32(p);
    memset(hle_ptr(p + 4), 0, size > 4 ? size - 4 : 0);
    fput(p + 4, g_list.px), fput(p + 8, g_list.py), fput(p + 12, g_list.pz);
    fput(p + 16, 0), fput(p + 20, 0), fput(p + 24, 0);
    fput(p + 28, g_list.fx), fput(p + 32, g_list.fy), fput(p + 36, g_list.fz);
    fput(p + 40, g_list.ux), fput(p + 44, g_list.uy), fput(p + 48, g_list.uz);
    fput(p + 52, g_list.dist_factor), fput(p + 56, g_list.rolloff), fput(p + 60, g_list.doppler);
    RET(DS_OK, 2);
}
static void l_GetDistanceFactor(void) { fput(A32(1), g_list.dist_factor); RET(DS_OK, 2); }
static void l_GetDopplerFactor(void)  { fput(A32(1), g_list.doppler); RET(DS_OK, 2); }
static void l_GetOrientation(void) {
    fput(A32(1), g_list.fx), fput(A32(1) + 4, g_list.fy), fput(A32(1) + 8, g_list.fz);
    fput(A32(2), g_list.ux), fput(A32(2) + 4, g_list.uy), fput(A32(2) + 8, g_list.uz);
    RET(DS_OK, 3);
}
static void l_GetPosition(void) { fput(A32(1), g_list.px), fput(A32(1) + 4, g_list.py), fput(A32(1) + 8, g_list.pz); RET(DS_OK, 2); }
static void l_GetRolloffFactor(void) { fput(A32(1), g_list.rolloff); RET(DS_OK, 2); }
static void l_GetVelocity(void) {
    fput(A32(1), 0), fput(A32(1) + 4, 0), fput(A32(1) + 8, 0);
    RET(DS_OK, 2);
}
static void l_SetAllParameters(void) {                   /* (this, &DS3DLISTENER, apply) */
    uint32_t p = A32(1);
    if (!p) RET(DSERR_INVALIDPARAM, 3);
    pthread_mutex_lock(&g_mix);
    memcpy(&g_list.px, hle_ptr(p + 4), 12);
    memcpy(&g_list.fx, hle_ptr(p + 28), 12);
    memcpy(&g_list.ux, hle_ptr(p + 40), 12);
    memcpy(&g_list.dist_factor, hle_ptr(p + 52), 12);
    pthread_mutex_unlock(&g_mix);
    atr("IDirectSound3DListener::SetAllParameters pos=(%.2f,%.2f,%.2f)", g_list.px, g_list.py, g_list.pz);
    RET(DS_OK, 3);
}
static void l_SetDistanceFactor(void) { pthread_mutex_lock(&g_mix); g_list.dist_factor = farg(1); pthread_mutex_unlock(&g_mix); RET(DS_OK, 3); }
static void l_SetDopplerFactor(void)  { pthread_mutex_lock(&g_mix); g_list.doppler = farg(1); pthread_mutex_unlock(&g_mix); RET(DS_OK, 3); }
static void l_SetOrientation(void) {                     /* (this, fx,fy,fz, ux,uy,uz, apply) */
    pthread_mutex_lock(&g_mix);
    g_list.fx = farg(1), g_list.fy = farg(2), g_list.fz = farg(3);
    g_list.ux = farg(4), g_list.uy = farg(5), g_list.uz = farg(6);
    pthread_mutex_unlock(&g_mix);
    atr("IDirectSound3DListener::SetOrientation front=(%.2f,%.2f,%.2f) top=(%.2f,%.2f,%.2f)", g_list.fx, g_list.fy, g_list.fz, g_list.ux, g_list.uy, g_list.uz);
    RET(DS_OK, 8);
}
static void l_SetPosition(void) {                        /* (this, x,y,z, apply) */
    pthread_mutex_lock(&g_mix);
    g_list.px = farg(1), g_list.py = farg(2), g_list.pz = farg(3);
    pthread_mutex_unlock(&g_mix);
    atr("IDirectSound3DListener::SetPosition(%.2f,%.2f,%.2f, apply=%u)", g_list.px, g_list.py, g_list.pz, A32(4));
    RET(DS_OK, 5);
}
static void l_SetRolloffFactor(void) { pthread_mutex_lock(&g_mix); g_list.rolloff = farg(1); pthread_mutex_unlock(&g_mix); RET(DS_OK, 3); }
static void l_SetVelocity(void) { RET(DS_OK, 5); }       /* the mixer does not do Doppler */
static void l_CommitDeferredSettings(void) {
    /* settings are applied immediately by this mixer; nothing to commit */
    RET(DS_OK, 1);
}

/* ======================================================================
 * IDirectSound
 * ====================================================================== */
static void d_Release(void) {
    uint32_t self = A32(0), n = --MEM32(self + 4);
    atr("IDirectSound::Release(obj=%#x) refs=%u", self, n);
    if (!n) hle_com_free(self);
    RET(n, 1);
}
/* A DSBUFFERDESC: dwSize, dwFlags, dwBufferBytes, dwReserved, lpwfxFormat */
static void d_CreateSoundBuffer(void) {                  /* (this, &DSBUFFERDESC, &out, outer) */
    uint32_t d = A32(1), out = A32(2);
    if (!d || !out) RET(DSERR_INVALIDPARAM, 4);
    uint32_t size = MEM32(d), flags = MEM32(d + 4), bytes = MEM32(d + 8), wfx = MEM32(d + 16);
    if (size < 20) { atr("IDirectSound::CreateSoundBuffer: dwSize=%u < 20", size); RET(DSERR_INVALIDPARAM, 4); }
    if (!audio_open()) { atr("IDirectSound::CreateSoundBuffer: no audio device"); RET(DSERR_NODRIVER, 4); }
    atr_caps("IDirectSound::CreateSoundBuffer", flags);
    atr_wfx("  ", wfx);
    dsbuf *b = (dsbuf *)calloc(1, sizeof *b);
    if (!b) RET(DSERR_INVALIDPARAM, 4);
    b->flags = flags, b->primary = (flags & DSBCAPS_PRIMARYBUFFER) != 0;
    b->channels = 2, b->bits = 16, b->rate = 22050, b->block = 4;
    b->volume = 0, b->pan = 0;
    b->min_dist = 1.0f, b->max_dist = 1e9f, b->mode = DS3DMODE_NORMAL;
    b->cone_inside = 360, b->cone_outside = 360, b->cone_outside_volume = 0;
    if (wfx) {
        unsigned tag = MEM16(wfx);
        b->channels = MEM16(wfx + 2), b->rate = (int)MEM32(wfx + 4);
        b->block = MEM16(wfx + 12), b->bits = MEM16(wfx + 14);
        uint32_t len = 18;
        if (tag == 0xFFFE && MEM16(wfx + 16) >= 22) len = 40;
        else if (MEM16(wfx + 16) == 0) len = 18;
        else len = 18 + MEM16(wfx + 16);
        if (len > sizeof b->wfx) len = sizeof b->wfx;
        memcpy(b->wfx, hle_ptr(wfx), len);
        b->wfxlen = len;
        if (tag != 1 || (b->bits != 8 && b->bits != 16) || b->channels < 1 || b->channels > 2 || !b->rate) {
            fprintf(stderr, "[dsound] CreateSoundBuffer: tag=%u(%s) ch=%d bits=%d rate=%d: not PCM this mixer plays\n",
                    tag, fmt_tag_name(tag), b->channels, b->bits, b->rate);
            atr("IDirectSound::CreateSoundBuffer: tag=%u(%s) ch=%d bits=%d rate=%d is not PCM this mixer plays -> DSERR_INVALIDPARAM",
                tag, fmt_tag_name(tag), b->channels, b->bits, b->rate);
            free(b);
            RET(DSERR_INVALIDPARAM, 4);
        }
        if (!b->block) b->block = b->channels * (b->bits / 8);
    }
    if (b->primary) {
        bytes = 4096 * 4;
    }
    if (!bytes) bytes = 4;
    b->bytes = bytes;
    b->data = (uint8_t *)hle_alloc(bytes ? bytes : 4);
    if (b->bits == 8) memset(b->data, 128, bytes);
    b->data_refs = (int *)malloc(sizeof(int));
    *b->data_refs = 1;
    gains(b);
    pthread_mutex_lock(&g_mix);
    int slot = -1;
    for (int i = 0; i < MAX_BUFS && slot < 0; i++) if (!g_buf[i]) slot = i;
    if (slot >= 0) g_buf[slot] = b;
    pthread_mutex_unlock(&g_mix);
    if (slot < 0) {
        fprintf(stderr, "[dsound] CreateSoundBuffer: more than %d buffers\n", MAX_BUFS);
        hle_free(b->data), free(b->data_refs), free(b);
        RET(DSERR_INVALIDPARAM, 4);
    }
    b->slot = (uint32_t)slot;
    b->obj = new_object(g_vt_buf, b->slot, 0);
    MEM32(out) = b->obj;
    atr("IDirectSound::CreateSoundBuffer -> %s obj=%#x slot=%u bytes=%u%s%s", result_name(DS_OK), b->obj, b->slot, b->bytes,
        b->primary ? " PRIMARY" : "", (flags & DSBCAPS_CTRL3D) ? " 3D" : "");
    RET(DS_OK, 4);
}
static void d_GetCaps(void) {                            /* (this, &DSCAPS) */
    uint32_t c = A32(1);
    if (c) {
        uint32_t size = MEM32(c);
        memset(hle_ptr(c + 4), 0, size > 4 ? size - 4 : 0);
        /* PCM 8/16, mono/stereo; software mixer (so EMULDRIVER is the truth);
         * any secondary rate; the max-values say we can mix plenty */
        MEM32(c + 4) = DSCAPS_PRIMARYMONO | DSCAPS_PRIMARYSTEREO | DSCAPS_PRIMARY8BIT | DSCAPS_PRIMARY16BIT |
                       DSCAPS_SECONDARYMONO | DSCAPS_SECONDARYSTEREO | DSCAPS_SECONDARY8BIT | DSCAPS_SECONDARY16BIT |
                       DSCAPS_CONTINUOUSRATE | DSCAPS_EMULDRIVER;
        MEM32(c + 8) = 100, MEM32(c + 12) = 100000;      /* min/max secondary rate */
        MEM32(c + 16) = 1;                               /* primary buffers */
        MEM32(c + 20) = 64, MEM32(c + 24) = 64, MEM32(c + 28) = 64;   /* max hw mixing: all, static, streaming */
        MEM32(c + 32) = 64, MEM32(c + 36) = 64, MEM32(c + 40) = 64;   /* free hw mixing */
        MEM32(c + 44) = 64, MEM32(c + 48) = 64, MEM32(c + 52) = 64;   /* max hw 3D: all, static, streaming */
        MEM32(c + 56) = 64, MEM32(c + 60) = 64, MEM32(c + 64) = 64;   /* free hw 3D */
        MEM32(c + 68) = 0, MEM32(c + 72) = 0, MEM32(c + 76) = 0;      /* hw memory: none (software mixer) */
        MEM32(c + 80) = 0, MEM32(c + 84) = 0;                         /* unlock transfer, play CPU overhead */
        MEM32(c + 88) = 0, MEM32(c + 92) = 0;                         /* reserved */
    }
    atr("IDirectSound::GetCaps -> DSCAPS flags=%#x (emulated software mixer)", 0x12FFu);
    RET(DS_OK, 2);
}
static void d_DuplicateSoundBuffer(void) {               /* (this, original, &out) */
    dsbuf *src = buf_of(A32(1));
    uint32_t out = A32(2);
    if (!src || !out) RET(DSERR_INVALIDPARAM, 3);
    dsbuf *b = (dsbuf *)calloc(1, sizeof *b);
    pthread_mutex_lock(&g_mix);
    *b = *src;
    (*b->data_refs)++;
    pthread_mutex_unlock(&g_mix);
    b->playing = 0, b->pos = 0, b->dead = 0;
    b->w3d = b->wks = b->wlst = 0, b->obj = 0, b->slot = 0;
    pthread_mutex_lock(&g_mix);
    int slot = -1;
    for (int i = 0; i < MAX_BUFS && slot < 0; i++) if (!g_buf[i]) slot = i;
    if (slot >= 0) g_buf[slot] = b;
    pthread_mutex_unlock(&g_mix);
    if (slot < 0) { free(b); RET(DSERR_INVALIDPARAM, 3); }
    b->slot = (uint32_t)slot;
    b->obj = new_object(g_vt_buf, b->slot, 0);
    MEM32(out) = b->obj;
    atr("IDirectSound::DuplicateSoundBuffer(slot %u -> %u) sharing %u bytes", src->slot, b->slot, b->bytes);
    RET(DS_OK, 3);
}
static void d_SetCooperativeLevel(void) {                /* (this, hwnd, level) */
    atr("IDirectSound::SetCooperativeLevel(hwnd=%#x, level=%u)", A32(1), A32(2));
    RET(DS_OK, 3);
}
static void d_Compact(void) { RET(DS_OK, 1); }
static void d_GetSpeakerConfig(void) { if (A32(1)) MEM32(A32(1)) = DSSPEAKER_STEREO; RET(DS_OK, 2); }   /* DSSPEAKER_STEREO */
static void d_SetSpeakerConfig(void) { atr("IDirectSound::SetSpeakerConfig(%#x)", A32(1)); RET(DS_OK, 2); }
static void d_Initialize(void) {                         /* (this, guid): CoCreateInstance(CLSID_DirectSound) path */
    atr("IDirectSound::Initialize(guid=%#x): accepted (one primary device on this host)", A32(1));
    RET(DS_OK, 2);
}

static void vtables(void) {
    if (g_vt_ds) return;
    static const win32hle_shim ds[] = {
        { "IDirectSound::QueryInterface", d_QueryInterface }, { "IDirectSound::AddRef", hle_com_AddRef },
        { "IDirectSound::Release", d_Release }, { "IDirectSound::CreateSoundBuffer", d_CreateSoundBuffer },
        { "IDirectSound::GetCaps", d_GetCaps }, { "IDirectSound::DuplicateSoundBuffer", d_DuplicateSoundBuffer },
        { "IDirectSound::SetCooperativeLevel", d_SetCooperativeLevel }, { "IDirectSound::Compact", d_Compact },
        { "IDirectSound::GetSpeakerConfig", d_GetSpeakerConfig }, { "IDirectSound::SetSpeakerConfig", d_SetSpeakerConfig },
        { "IDirectSound::Initialize", d_Initialize }, { 0, 0 } };
    static const win32hle_shim buf[] = {
        { "IDirectSoundBuffer::QueryInterface", b_QueryInterface }, { "IDirectSoundBuffer::AddRef", hle_com_AddRef },
        { "IDirectSoundBuffer::Release", b_Release }, { "IDirectSoundBuffer::GetCaps", b_GetCaps },
        { "IDirectSoundBuffer::GetCurrentPosition", b_GetCurrentPosition }, { "IDirectSoundBuffer::GetFormat", b_GetFormat },
        { "IDirectSoundBuffer::GetVolume", b_GetVolume }, { "IDirectSoundBuffer::GetPan", b_GetPan },
        { "IDirectSoundBuffer::GetFrequency", b_GetFrequency }, { "IDirectSoundBuffer::GetStatus", b_GetStatus },
        { "IDirectSoundBuffer::Initialize", b_Initialize }, { "IDirectSoundBuffer::Lock", b_Lock },
        { "IDirectSoundBuffer::Play", b_Play }, { "IDirectSoundBuffer::SetCurrentPosition", b_SetCurrentPosition },
        { "IDirectSoundBuffer::SetFormat", b_SetFormat }, { "IDirectSoundBuffer::SetVolume", b_SetVolume },
        { "IDirectSoundBuffer::SetPan", b_SetPan }, { "IDirectSoundBuffer::SetFrequency", b_SetFrequency },
        { "IDirectSoundBuffer::Stop", b_Stop }, { "IDirectSoundBuffer::Unlock", b_Unlock },
        { "IDirectSoundBuffer::Restore", b_Restore }, { 0, 0 } };
    static const win32hle_shim b3d[] = {
        { "IDirectSound3DBuffer::QueryInterface", w_QueryInterface }, { "IDirectSound3DBuffer::AddRef", hle_com_AddRef },
        { "IDirectSound3DBuffer::Release", w_Release }, { "IDirectSound3DBuffer::GetAllParameters", b3_GetAllParameters },
        { "IDirectSound3DBuffer::GetConeAngles", b3_GetConeAngles }, { "IDirectSound3DBuffer::GetConeOrientation", b3_GetConeOrientation },
        { "IDirectSound3DBuffer::GetConeOutsideVolume", b3_GetConeOutsideVolume },
        { "IDirectSound3DBuffer::GetMaxDistance", b3_GetMaxDistance }, { "IDirectSound3DBuffer::GetMinDistance", b3_GetMinDistance },
        { "IDirectSound3DBuffer::GetMode", b3_GetMode }, { "IDirectSound3DBuffer::GetPosition", b3_GetPosition },
        { "IDirectSound3DBuffer::GetVelocity", b3_GetVelocity },
        { "IDirectSound3DBuffer::SetAllParameters", b3_SetAllParameters }, { "IDirectSound3DBuffer::SetConeAngles", b3_SetConeAngles },
        { "IDirectSound3DBuffer::SetConeOrientation", b3_SetConeOrientation },
        { "IDirectSound3DBuffer::SetConeOutsideVolume", b3_SetConeOutsideVolume },
        { "IDirectSound3DBuffer::SetMaxDistance", b3_SetMaxDistance }, { "IDirectSound3DBuffer::SetMinDistance", b3_SetMinDistance },
        { "IDirectSound3DBuffer::SetMode", b3_SetMode }, { "IDirectSound3DBuffer::SetPosition", b3_SetPosition },
        { "IDirectSound3DBuffer::SetVelocity", b3_SetVelocity }, { 0, 0 } };
    static const win32hle_shim ks[] = {
        { "IKsPropertySet::QueryInterface", w_QueryInterface }, { "IKsPropertySet::AddRef", hle_com_AddRef },
        { "IKsPropertySet::Release", w_Release }, { "IKsPropertySet::Get", ks_Get },
        { "IKsPropertySet::Set", ks_Set }, { "IKsPropertySet::QuerySupport", ks_QuerySupport }, { 0, 0 } };
    static const win32hle_shim lst[] = {
        { "IDirectSound3DListener::QueryInterface", w_QueryInterface }, { "IDirectSound3DListener::AddRef", hle_com_AddRef },
        { "IDirectSound3DListener::Release", w_Release }, { "IDirectSound3DListener::GetAllParameters", l_GetAllParameters },
        { "IDirectSound3DListener::GetDistanceFactor", l_GetDistanceFactor }, { "IDirectSound3DListener::GetDopplerFactor", l_GetDopplerFactor },
        { "IDirectSound3DListener::GetOrientation", l_GetOrientation }, { "IDirectSound3DListener::GetPosition", l_GetPosition },
        { "IDirectSound3DListener::GetRolloffFactor", l_GetRolloffFactor }, { "IDirectSound3DListener::GetVelocity", l_GetVelocity },
        { "IDirectSound3DListener::SetAllParameters", l_SetAllParameters }, { "IDirectSound3DListener::SetDistanceFactor", l_SetDistanceFactor },
        { "IDirectSound3DListener::SetDopplerFactor", l_SetDopplerFactor }, { "IDirectSound3DListener::SetOrientation", l_SetOrientation },
        { "IDirectSound3DListener::SetPosition", l_SetPosition }, { "IDirectSound3DListener::SetRolloffFactor", l_SetRolloffFactor },
        { "IDirectSound3DListener::SetVelocity", l_SetVelocity }, { "IDirectSound3DListener::CommitDeferredSettings", l_CommitDeferredSettings },
        { 0, 0 } };
    g_vt_ds = hle_com_vtable(ds);
    g_vt_buf = hle_com_vtable(buf);
    g_vt_3d = hle_com_vtable(b3d);
    g_vt_ks = hle_com_vtable(ks);
    g_vt_list = hle_com_vtable(lst);
}

/* DirectSoundCreate(guid, &out, outer); DirectSoundCreate8 has the same
 * signature and yields an object that also answers IID_IDirectSound8. */
static void create_ds(const char *what, int argc) {
    vtables();
    if (!A32(1)) RET(DSERR_INVALIDPARAM, argc);
    if (!audio_open()) { MEM32(A32(1)) = 0; atr("DirectSoundCreate: no audio device -> DSERR_NODRIVER"); RET(DSERR_NODRIVER, argc); }
    uint32_t o = hle_com_new(g_vt_ds, 12);
    MEM32(A32(1)) = o;
    atr("%s(guid=%#x) -> DS_OK obj=%#x", what, A32(0), o);
    RET(DS_OK, argc);
}
static void s_DirectSoundCreate(void)  { create_ds("DirectSoundCreate", 3); }
static void s_DirectSoundCreate8(void) { create_ds("DirectSoundCreate8", 3); }
static void s_DirectSoundEnumerateA(void) {              /* (callback, context): the one device */
    static uint32_t desc, mod;                           /* the guest may keep them */
    if (!desc) desc = hle_guest_strdup("Primary Sound Driver"), mod = hle_guest_strdup("");
    uint32_t a[4] = { 0, desc, mod, A32(1) };
    atr("DirectSoundEnumerateA: one device");
    if (A32(0)) hle_call_guest(A32(0), 4, a);
    RET(DS_OK, 2);
}

/* CoCreateInstance(CLSID_DirectSound): the object DirectSoundCreate makes;
 * the caller's IDirectSound::Initialize follows. */
static uint32_t ds_create(const uint8_t *iid, uint32_t *out) {
    (void)iid;
    vtables();
    if (!audio_open()) { *out = 0; return DSERR_NODRIVER; }
    *out = hle_com_new(g_vt_ds, 12);
    return DS_OK;
}
void hle_dsound_register_com(void) {
    static const uint8_t CLSID_DirectSound[16] = { 0x46, 0xD9, 0xD4, 0x47, 0xE8, 0x62, 0xCF, 0x11,
                                                   0x93, 0xBC, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
    hle_com_register_class(CLSID_DirectSound, ds_create);
}

const win32hle_shim win32hle_dsound[] = {
    { "dsound.dll#1", s_DirectSoundCreate }, { "DirectSoundCreate", s_DirectSoundCreate },
    { "dsound.dll#6", s_DirectSoundCreate8 }, { "DirectSoundCreate8", s_DirectSoundCreate8 },
    { "dsound.dll#2", s_DirectSoundEnumerateA }, { "DirectSoundEnumerateA", s_DirectSoundEnumerateA },
    { 0, 0 }
};

/*
 * winmm.c - win32hle WINMM: multimedia timers, joystick, waveOut audio, MCI.
 *
 * NFS5 (Porsche Unleashed) imports exactly eight WINMM functions: the five
 * timer calls (timeGetDevCaps, timeBeginPeriod, timeEndPeriod, timeSetEvent,
 * timeKillEvent) and the three joystick calls (joyGetNumDevs, joyGetDevCapsA,
 * joyGetPosEx). Its lifted strings name the timers directly ("TIMER_init -
 * FAILED TO INITIALIZE WINDOWS MULTIMEDIA TIMER", "wininittimer - FAILED TO
 * CREATE SECONDARY TIMER`S THREAD", "framerate - TIMERHZ NOT SET!"), so the
 * timers are the game's frame/simulation clock and must fire at the right rate
 * and call back into lifted code. waveOut and MCI are kept for other titles
 * (this one does not import them); waveOut remains an accept-and-retire silent
 * device and MCI is not implemented.
 *
 * Joystick is real SDL2: joyGetNumDevs counts the attached SDL joysticks,
 * joyGetDevCapsA describes one, joyGetPosEx returns a DIJOYSTATE-compatible
 * JOYINFOEX (16-bit 0..65535 axes, POV in hundredths of a degree, button
 * mask). A game that finds none falls back to keyboard/mouse, which is what
 * the original does with no joystick plugged in.
 *
 * Trace: NFS5_TRACE_AUDIO=1 (or NFS5_TRACE_AUDIO_FILE=<path>) logs timer and
 * joystick calls, as dsound.c does.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "win32hle.h"

#define MMSYSERR_NOERROR  0u
#define MMSYSERR_ERROR    1u
#define MMSYSERR_INVALPARAM 97u
#define JOYERR_PARMS      165u
#define JOYERR_NOCANDO    166u
#define JOYERR_UNPLUGGED  167u
#define WHDR_DONE         0x00000001u
#define WHDR_INQUEUE      0x00000010u
#define WHDR_PREPARED     0x00000002u

/* ---- trace: same switch and file as dsound.c ---- */
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
    fprintf(f, "[winmm  %8u] ", hle_ticks_ms());
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    fflush(f);
    funlockfile(f);
    va_end(ap);
}

/* --- multimedia timer --- */
static void m_timeGetTime(void) { RET(hle_ticks_ms(), 0); }
static unsigned g_period_refs;
static void m_timeBeginPeriod(void) {
    g_period_refs++;
    atr("timeBeginPeriod(%u) -> refs=%u", A32(0), g_period_refs);
    RET(MMSYSERR_NOERROR, 1);
}
static void m_timeEndPeriod(void) {
    if (g_period_refs) g_period_refs--;
    atr("timeEndPeriod(%u) -> refs=%u", A32(0), g_period_refs);
    RET(MMSYSERR_NOERROR, 1);
}
static void m_timeGetDevCaps(void) {           /* (&TIMECAPS, size) */
    if (A32(0) && A32(1) >= 8) MEM32(A32(0)) = 1, MEM32(A32(0) + 4) = 1000000;
    atr("timeGetDevCaps -> wPeriodMin=1 wPeriodMax=1000000 (host timer resolution is ~ms)");
    RET(MMSYSERR_NOERROR, 2);
}

/* --- multimedia timers ---
 * Each timer is a host thread that calls the guest's callback through
 * hle_call_guest, so the callback runs once it gets the machine, as on Windows
 * it runs on winmm's thread. TIME_PERIODIC reschedules from the absolute due
 * time (no drift); a timer that falls far behind does not catch up in a
 * storm. timeKillEvent waits until a callback already under way has finished
 * and the timer's thread is gone (giving the machine up while it waits), so a
 * game may free what its callback uses immediately after the kill — the
 * callback cannot run after timeKillEvent returns. (TIME_KILL_SYNCHRONOUS
 * asks for exactly this behaviour, and we always provide it.)
 */
#define MAX_MMTIMERS 32
#define TIME_PERIODIC 0x0001u
#define TIME_KILL_SYNCHRONOUS 0x0100u
static struct {
    volatile int state;         /* 0 free, 1 running */
    volatile int killed;
    volatile int inflight;
    uint32_t id, delay, proc, user, flags;
    pthread_t thread;
} g_mmt[MAX_MMTIMERS];
static uint32_t g_mmt_next = 1;   /* atomic: timeSetEvent runs on several threads */

/* NFS5_FRAME_CAP: the game's frame clock is a one-shot timeSetEvent timer
 * (proc 0x564EB0) rescheduled every frame. Waiting before running that exact
 * callback paces the guest's frame loop at the requested rate, without
 * touching the timer values the game computes. */
#define NFS5_FRAME_TIMER_PROC 0x564EB0u
static void frame_cap_wait(uint32_t proc) {
    static int ms = -1;
    static uint32_t last;
    if (proc != NFS5_FRAME_TIMER_PROC) return;
    if (ms < 0) {
        const char *v = getenv("NFS5_FRAME_CAP");
        int cap = v && *v ? atoi(v) : 0;
        ms = cap > 0 ? (1000 + cap - 1) / cap : 0;
        if (ms) fprintf(stderr, "[winmm] frame cap %d fps (%d ms frame clock)\n", cap, ms);
    }
    if (ms <= 0) return;
    uint32_t now = hle_ticks_ms();
    if (last) {
        int32_t wait = (int32_t)ms - (int32_t)(now - last);
        if (wait > 0 && wait <= ms) usleep((useconds_t)wait * 1000u);
    }
    last = hle_ticks_ms();
}
static void mmtimer_fire(int i) {
    if (g_mmt[i].proc == 0x564EB0u && getenv("NFS5_FRAMECLOCK")) {
        static unsigned n; static uint32_t t0;
        uint32_t now = hle_ticks_ms();
        if (!t0) t0 = now;
        if (++n % 100 == 0)
            fprintf(stderr, "[frameclock] %u ticks in %u ms -> %.2f/s\n", n, now - t0, n * 1000.0 / (now - t0));
    }
    if (g_mmt[i].flags & 0x18u) {                    /* TIME_CALLBACK_EVENT_SET(8) / _PULSE(0x10): proc is an event */
        uint32_t h = g_mmt[i].proc;
        mach_enter();
        hle_set_event(h);
        mach_leave();
    } else {
        frame_cap_wait(g_mmt[i].proc);
        uint32_t a[5] = { g_mmt[i].id, 0, g_mmt[i].user, 0, 0 };
        hle_call_guest(g_mmt[i].proc, 5, a);
    }
}
static void *mmtimer(void *arg) {
    int i = (int)(intptr_t)arg;
    uint32_t due = hle_ticks_ms() + g_mmt[i].delay;
    unsigned fired = 0;
    for (;;) {
        int32_t wait = (int32_t)(due - hle_ticks_ms());
        if (wait > 0) usleep((useconds_t)wait * 1000u);
        if (__atomic_load_n(&g_mmt[i].killed, __ATOMIC_ACQUIRE)) break;
        __atomic_add_fetch(&g_mmt[i].inflight, 1, __ATOMIC_ACQ_REL);
        if (!__atomic_load_n(&g_mmt[i].killed, __ATOMIC_ACQUIRE)) {
            if (trace_on() && fired < 8)
                atr("timer %u fired (delay=%ums%s)", g_mmt[i].id, g_mmt[i].delay,
                    (g_mmt[i].flags & TIME_PERIODIC) ? ", periodic" : ", one-shot");
            fired++;
            mmtimer_fire(i);
        }
        __atomic_sub_fetch(&g_mmt[i].inflight, 1, __ATOMIC_ACQ_REL);
        if (!(g_mmt[i].flags & TIME_PERIODIC)) break;
        due += g_mmt[i].delay ? g_mmt[i].delay : 1;
        if ((int32_t)(hle_ticks_ms() - due) > 1000) due = hle_ticks_ms();   /* fell far behind: no catch-up storm */
    }
    uint32_t id = g_mmt[i].id;
    __atomic_store_n(&g_mmt[i].killed, 0, __ATOMIC_RELEASE);
    g_mmt[i].id = 0;
    __atomic_store_n(&g_mmt[i].state, 0, __ATOMIC_RELEASE);
    (void)id;
    return NULL;
}
/* A failed timeSetEvent is fatal for NFS5: its frame clock re-arms a one-shot
 * timer from the callback (~128 times a second) and stops for good, zeroing
 * TIMERHZ, if that ever returns 0 ("framerate - TIMERHZ NOT SET!", then the
 * game exits). Thread creation and a full slot table are transient here, so
 * both are retried for up to ~0.5 s with the machine given up, and the cause
 * is always logged. */
#define MMT_RETRY_US 2000
#define MMT_RETRIES 250
static int mmt_claim_slot(void) {
    for (int i = 0; i < MAX_MMTIMERS; i++) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&g_mmt[i].state, &expected, 1, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return i;
    }
    return -1;
}
/* Recent timeSetEvent results, for diagnostics (hle_winmm_dump_recent). */
static struct { uint32_t seq, delay, proc, flags, ret, slot; unsigned long th; } g_tse_ring[16];
static uint32_t g_tse_seq;
static void tse_note(uint32_t delay, uint32_t proc, uint32_t flags, uint32_t ret, int slot) {
    uint32_t n = __atomic_fetch_add(&g_tse_seq, 1, __ATOMIC_RELAXED);
    g_tse_ring[n & 15].seq = n;
    g_tse_ring[n & 15].delay = delay; g_tse_ring[n & 15].proc = proc; g_tse_ring[n & 15].flags = flags;
    g_tse_ring[n & 15].ret = ret; g_tse_ring[n & 15].slot = (uint32_t)slot;
    g_tse_ring[n & 15].th = (unsigned long)pthread_self();
}
void hle_winmm_dump_recent(void) {
    uint32_t n = __atomic_load_n(&g_tse_seq, __ATOMIC_RELAXED);
    for (uint32_t k = n > 16 ? n - 16 : 0; k < n; k++) {
        typeof(g_tse_ring[0]) *e = &g_tse_ring[k & 15];
        fprintf(stderr, "[winmm] recent timeSetEvent #%u thread=%lx delay=%u proc=%#x flags=%#x -> %u (slot %d)\n",
                e->seq, e->th, e->delay, e->proc, e->flags, e->ret, (int)e->slot);
    }
    int live = 0;
    for (int i = 0; i < MAX_MMTIMERS; i++) live += __atomic_load_n(&g_mmt[i].state, __ATOMIC_RELAXED) != 0;
    fprintf(stderr, "[winmm] live timer slots %d, next id %u\n", live, g_mmt_next);
}
static void m_timeSetEvent(void) {             /* (delay, resolution, proc, user, flags) */
    uint32_t delay = A32(0), proc = A32(2), user = A32(3), flags = A32(4);
    int last_err = 0, i = -1;
    for (int attempt = 0; attempt <= MMT_RETRIES; attempt++) {
        if (attempt) {
            hle_block_begin();
            usleep(MMT_RETRY_US);
            hle_block_end();
        }
        i = mmt_claim_slot();
        if (i < 0) { last_err = -1; continue; }
        g_mmt[i].killed = 0, g_mmt[i].inflight = 0;
        /* The id is returned from this local, never re-read from the slot: a
         * one-shot timer can fire, run and free its slot (id = 0) before this
         * thread gets the machine back after pthread_create, and returning 0
         * makes NFS5 stop its frame clock ("framerate - TIMERHZ NOT SET!"). */
        const uint32_t id = __atomic_fetch_add(&g_mmt_next, 1, __ATOMIC_RELAXED);
        g_mmt[i].id = id;
        g_mmt[i].delay = delay, g_mmt[i].proc = proc;
        g_mmt[i].user = user, g_mmt[i].flags = flags;
        /* NFS5 reschedules its one-shot frame clock ~128 times a second;
         * creating the host thread (~100 us) must not hold the machine */
        hle_block_begin();
        int prc = pthread_create(&g_mmt[i].thread, NULL, mmtimer, (void *)(intptr_t)i);
        hle_block_end();
        if (prc != 0) {
            g_mmt[i].id = 0;
            __atomic_store_n(&g_mmt[i].state, 0, __ATOMIC_RELEASE);
            last_err = prc;
            continue;
        }
        pthread_detach(g_mmt[i].thread);
        if (attempt)
            fprintf(stderr, "[winmm] timeSetEvent(proc=%#x) armed after %d retries (%s)\n", proc, attempt,
                    last_err < 0 ? "all timer slots busy" : strerror(last_err));
        tse_note(delay, proc, flags, id, i);
        atr("timeSetEvent(delay=%ums, res=%ums, proc=%#x, user=%#x, flags=%#x%s) -> timer %u",
            delay, A32(1), proc, user, flags, (flags & TIME_PERIODIC) ? " PERIODIC" : " ONESHOT", id);
        RET(id, 5);
    }
    fprintf(stderr, "[winmm] timeSetEvent(proc=%#x) FAILED: %s\n", proc,
            last_err < 0 ? "more than " "32" " timers live" : strerror(last_err));
    tse_note(delay, proc, flags, 0, -1);
    RET(0, 5);
}
static void m_timeKillEvent(void) {
    uint32_t id = A32(0);
    for (int i = 0; i < MAX_MMTIMERS; i++) {
        if (__atomic_load_n(&g_mmt[i].state, __ATOMIC_ACQUIRE) && g_mmt[i].id == id) {
            __atomic_store_n(&g_mmt[i].killed, 1, __ATOMIC_RELEASE);
            if (pthread_equal(g_mmt[i].thread, pthread_self())) {
                /* killed from inside its own callback: do not wait on ourselves */
                atr("timeKillEvent(%u) from its own callback: marked killed, thread exits after return", id);
                RET(MMSYSERR_NOERROR, 1);
            }
            while (__atomic_load_n(&g_mmt[i].inflight, __ATOMIC_ACQUIRE) ||
                   __atomic_load_n(&g_mmt[i].state, __ATOMIC_ACQUIRE)) {
                hle_block_begin();                    /* let the callback have the machine */
                usleep(200);
                hle_block_end();
            }
            atr("timeKillEvent(%u) -> callbacks drained, thread gone", id);
            RET(MMSYSERR_NOERROR, 1);
        }
    }
    atr("timeKillEvent(%u): MMSYSERR_INVALPARAM (no such live timer)", id);
    RET(MMSYSERR_INVALPARAM, 1);
}

/* --- joystick on SDL2 --- */
#define MAX_JOY 16
static int g_joy_init = -1;                     /* -1 untried, 0 failed, 1 up */
static SDL_Joystick *g_joy[MAX_JOY];
/* Headless controller testing (and a way to validate the pad path without
 * hardware): NFS5_VIRTUAL_PAD=1 attaches one SDL virtual game controller when
 * no real joystick is present, and NFS5_PAD_STATE applies a fixed state to it,
 * e.g. "1=32767" (axis 1 full forward) or "b0=1" (button 0 pressed).
 * Axes are index=value entries, buttons bN=1, hats hN=0..7, separated by
 * commas or semicolons. Read once at start; the device is revisited on every
 * joyGetPosEx so the game can poll it like real hardware. */
#define VPAD_AXES 6
static int g_vpad_axes[VPAD_AXES];
static uint8_t g_vpad_buttons[32];
static uint8_t g_vpad_hats[4];
static int g_vpad_parsed;
static void vpad_parse(void) {
    if (g_vpad_parsed) return;
    g_vpad_parsed = 1;
    for (int i = 0; i < VPAD_AXES; i++) g_vpad_axes[i] = 0;
    for (int i = 0; i < 32; i++) g_vpad_buttons[i] = 0;
    const char *st = getenv("NFS5_PAD_STATE");
    if (!st) return;
    char *copy = strdup(st), *p = copy;
    while (p && *p) {
        char *sep = strpbrk(p, ",;");
        if (sep) *sep = 0;
        while (*p == ' ') p++;
        if (*p == 'b' && p[1] >= '0' && p[1] <= '9') {
            int i = atoi(p + 1);
            const char *eq = strchr(p, '=');
            if (eq && i >= 0 && i < 32) g_vpad_buttons[i] = atoi(eq + 1) ? 0x80 : 0;
        } else if (*p == 'h' && p[1] >= '0' && p[1] <= '9') {
            int i = atoi(p + 1);
            const char *eq = strchr(p, '=');
            if (eq && i >= 0 && i < 4) g_vpad_hats[i] = (uint8_t)atoi(eq + 1);
        } else {
            const char *eq = strchr(p, '=');
            if (eq) {
                int i = atoi(p);
                if (i >= 0 && i < VPAD_AXES) g_vpad_axes[i] = atoi(eq + 1);
            }
        }
        if (!sep) break;
        p = sep + 1;
    }
    free(copy);
}
static void vpad_apply(SDL_Joystick *j) {
    if (!j) return;
    vpad_parse();
    if (SDL_JoystickNumAxes(j) == VPAD_AXES) {   /* the virtual pad we attached */
        for (int i = 0; i < VPAD_AXES; i++)
            SDL_JoystickSetVirtualAxis(j, i, (Sint16)g_vpad_axes[i]);
        for (int i = 0; i < 32 && i < SDL_JoystickNumButtons(j); i++)
            SDL_JoystickSetVirtualButton(j, i, g_vpad_buttons[i]);
        for (int i = 0; i < 4 && i < SDL_JoystickNumHats(j); i++)
            SDL_JoystickSetVirtualHat(j, i, g_vpad_hats[i]);
    }
}
/* Attach the headless test pad before the game enumerates devices (the lazy
 * attach in joy_init can come after DirectInput enumeration). Called from the
 * host main() when NFS5_VIRTUAL_PAD is set. */
void hle_winmm_pad_preinit(void) {
    if (!getenv("NFS5_VIRTUAL_PAD")) return;
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) != 0 && SDL_WasInit(SDL_INIT_JOYSTICK) == 0) return;
    if (SDL_NumJoysticks() == 0) {
        int idx = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, VPAD_AXES, 32, 2);
        atr("virtual gamepad pre-attached at index %d", idx);
    }
    g_joy_init = 1;
}
static void joy_init(void) {
    if (g_joy_init >= 0) return;
    g_joy_init = SDL_InitSubSystem(SDL_INIT_JOYSTICK) == 0;
    if (!g_joy_init) fprintf(stderr, "[winmm] SDL joystick init failed: %s\n", SDL_GetError());
    if (g_joy_init && getenv("NFS5_VIRTUAL_PAD") && SDL_NumJoysticks() == 0) {
        int idx = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, VPAD_AXES, 32, 2);
        atr("virtual gamepad attached at index %d", idx);
    }
    atr("SDL joystick subsystem: %s", g_joy_init ? "up" : "unavailable");
}
static SDL_Joystick *joy_get(int id) {
    joy_init();
    if (!g_joy_init || id < 0 || id >= MAX_JOY) return NULL;
    int n = SDL_NumJoysticks();
    if (id >= n) return NULL;
    if (!g_joy[id] || !SDL_JoystickGetAttached(g_joy[id])) {
        if (g_joy[id]) { SDL_JoystickClose(g_joy[id]); g_joy[id] = NULL; }
        g_joy[id] = SDL_JoystickOpen(id);
        vpad_apply(g_joy[id]);
        if (g_joy[id]) atr("joystick %d opened: %s (%d axes, %d buttons, %d hats)", id, SDL_JoystickName(g_joy[id]),
                           SDL_JoystickNumAxes(g_joy[id]), SDL_JoystickNumButtons(g_joy[id]), SDL_JoystickNumHats(g_joy[id]));
    }
    return g_joy[id];
}
static uint16_t joy_axis(SDL_Joystick *j, int axis) {
    vpad_apply(j);
    if (axis >= SDL_JoystickNumAxes(j)) return 0x8000u;
    int32_t v = SDL_JoystickGetAxis(j, axis);
    return (uint16_t)(((int64_t)v + 32768) * 65535 / 65535);   /* -32768..32767 -> 0..65535 */
}
static uint32_t joy_pov(SDL_Joystick *j) {
    if (SDL_JoystickNumHats(j) < 1) return 0xFFFFu;             /* JOY_POVCENTERED */
    Uint8 h = SDL_JoystickGetHat(j, 0);
    int x = (h & SDL_HAT_RIGHT) ? 1 : (h & SDL_HAT_LEFT) ? -1 : 0;
    int y = (h & SDL_HAT_UP) ? 1 : (h & SDL_HAT_DOWN) ? -1 : 0;
    if (!x && !y) return 0xFFFFu;
    /* DirectInput reports hundredths of a degree clockwise from north */
    static const int p[3][3] = { { 22500, 18000, 13500 }, { 27000, -1, 9000 }, { 31500, 0, 4500 } };
    return (uint32_t)p[y + 1][x + 1];
}
static void m_joyGetNumDevs(void) {
    joy_init();
    int n = g_joy_init ? SDL_NumJoysticks() : 0;
    if (n > MAX_JOY) n = MAX_JOY;
    atr("joyGetNumDevs -> %d (SDL joysticks attached)", n);
    RET((uint32_t)n, 0);
}
/* JOYCAPS is a fixed 404-byte Win32 structure; fill the fields by offset so
 * the shim is right for any caller's declared size. */
static void m_joyGetDevCapsA(void) {            /* (id, &JOYCAPS, cb) */
    int id = (int)A32(0);
    uint32_t caps = A32(1), cb = A32(2);
    SDL_Joystick *j = joy_get(id);
    if (!j) { atr("joyGetDevCapsA(%d): JOYERR_UNPLUGGED", id); RET(JOYERR_UNPLUGGED, 3); }
    if (!caps || cb < 44) RET(JOYERR_PARMS, 3);
    int axes = SDL_JoystickNumAxes(j), buttons = SDL_JoystickNumButtons(j), hats = SDL_JoystickNumHats(j);
    memset(APTR(1), 0, cb);
    MEM16(caps + 0) = (uint16_t)SDL_JoystickGetVendor(j);           /* wMid */
    MEM16(caps + 2) = (uint16_t)SDL_JoystickGetProduct(j);          /* wPid */
    const char *name = SDL_JoystickName(j);
    if (name) strncpy((char *)hle_ptr(caps + 4), name, 31);         /* szPname[32] */
    MEM32(caps + 36) = 0, MEM32(caps + 40) = 65535;                 /* X */
    MEM32(caps + 44) = 0, MEM32(caps + 48) = 65535;                 /* Y */
    MEM32(caps + 52) = 0, MEM32(caps + 56) = axes > 2 ? 65535 : 0;  /* Z */
    MEM32(caps + 60) = (uint32_t)buttons;                           /* wNumButtons */
    MEM32(caps + 64) = 1, MEM32(caps + 68) = 1000;                  /* wPeriodMin/Max */
    MEM32(caps + 72) = 0, MEM32(caps + 76) = axes > 3 ? 65535 : 0;  /* R */
    MEM32(caps + 80) = 0, MEM32(caps + 84) = axes > 4 ? 65535 : 0;  /* U */
    MEM32(caps + 88) = 0, MEM32(caps + 92) = axes > 5 ? 65535 : 0;  /* V */
    MEM32(caps + 96) = (axes > 2 ? 0x0001u : 0) | (axes > 3 ? 0x0002u : 0) | (axes > 4 ? 0x0004u : 0) |
                       (axes > 5 ? 0x0008u : 0) | (hats > 0 ? 0x0010u | 0x0040u : 0);   /* wCaps: HASZ/R/U/V, HASPOV, POVCTS */
    MEM32(caps + 100) = (uint32_t)(axes > 6 ? axes : 6);            /* wMaxAxes */
    MEM32(caps + 104) = (uint32_t)(axes > 0 ? axes : 1);            /* wNumAxes */
    MEM32(caps + 108) = (uint32_t)(buttons > 0 ? buttons : 1);      /* wMaxButtons */
    atr("joyGetDevCapsA(%d) -> 0: \"%s\" axes=%d buttons=%d hats=%d", id, name ? name : "?", axes, buttons, hats);
    RET(MMSYSERR_NOERROR, 3);
}
/* JOYINFOEX (52 bytes): dwSize, dwFlags, X/Y/Z/R/U/V, dwButtons,
 * dwButtonNumber, dwPOV, reserved. Axes 0..65535 centered at 0x8000. */
static uint32_t joy_pos_ex(SDL_Joystick *j, uint32_t out) {
    MEM32(out + 8)  = joy_axis(j, 0);
    MEM32(out + 12) = joy_axis(j, 1);
    MEM32(out + 16) = joy_axis(j, 2);
    MEM32(out + 20) = joy_axis(j, 3);
    MEM32(out + 24) = joy_axis(j, 4);
    MEM32(out + 28) = joy_axis(j, 5);
    uint32_t mask = 0;
    int first = -1, nb = SDL_JoystickNumButtons(j);
    for (int b = 0; b < nb && b < 32; b++)
        if (SDL_JoystickGetButton(j, b)) { mask |= 1u << b; if (first < 0) first = b; }
    MEM32(out + 32) = mask;                                  /* dwButtons */
    MEM32(out + 36) = (uint32_t)(first >= 0 ? first + 1 : 0);/* dwButtonNumber (1-based) */
    MEM32(out + 40) = joy_pov(j);                            /* dwPOV */
    return 0;
}
static void m_joyGetPosEx(void) {              /* (id, &JOYINFOEX) */
    int id = (int)A32(0);
    uint32_t p = A32(1);
    SDL_Joystick *j = joy_get(id);
    if (!j) { atr("joyGetPosEx(%d): JOYERR_UNPLUGGED", id); RET(JOYERR_UNPLUGGED, 2); }
    if (!p || MEM32(p) < 52) RET(JOYERR_PARMS, 2);
    MEM32(p + 4) = 0x000000FFu;                              /* JOY_RETURNALL-ish: we fill every field */
    joy_pos_ex(j, p);
    if (trace_on()) {
        static unsigned long n;
        if ((n++ % 120) == 0)
            atr("joyGetPosEx(%d) -> X=%u Y=%u Z=%u R=%u buttons=%#x pov=%u", id, MEM32(p + 8), MEM32(p + 12),
                MEM32(p + 16), MEM32(p + 20), MEM32(p + 32), MEM32(p + 40));
    }
    RET(MMSYSERR_NOERROR, 2);
}
static void m_joyGetPos(void) {                /* (id, &JOYINFO): the old 8-byte struct */
    int id = (int)A32(0);
    uint32_t p = A32(1);
    SDL_Joystick *j = joy_get(id);
    if (!j) { atr("joyGetPos(%d): JOYERR_UNPLUGGED", id); RET(JOYERR_UNPLUGGED, 2); }
    if (!p) RET(JOYERR_PARMS, 2);
    MEM16(p + 0) = joy_axis(j, 0);
    MEM16(p + 2) = joy_axis(j, 1);
    MEM16(p + 4) = joy_axis(j, 2);
    uint32_t mask = 0;
    int nb = SDL_JoystickNumButtons(j);
    for (int b = 0; b < nb && b < 16; b++) if (SDL_JoystickGetButton(j, b)) mask |= 1u << b;
    MEM16(p + 6) = (uint16_t)mask;
    RET(MMSYSERR_NOERROR, 2);
}

/* --- waveOut: a silent device that retires every buffer at once ---
 * NFS5 does not import waveOut (its sound engine is DirectSound), so this is
 * for other titles; a caller that polls WAVEHDR.dwFlags sees WHDR_DONE after
 * waveOutWrite and its double-buffered mixer thread keeps cycling. Nothing is
 * played: if a title relied on this for sound it would be silent, and the
 * trace below says so per call. */
static void m_waveOutOpen(void) {              /* (&hwo, devID, &fmt, cb, inst, flags) */
    uint32_t ph = A32(0);
    if (ph) MEM32(ph) = 0x5A5E0001u;           /* a nonzero HWAVEOUT */
    atr("waveOutOpen(dev=%u, flags=%#x): accepted as a silent device (NFS5 does not use waveOut)",
        A32(1), A32(5));
    RET(MMSYSERR_NOERROR, 6);
}
static void m_waveOutWrite(void) {             /* (hwo, lpWaveHdr, cbwh) */
    uint32_t wh = A32(1);
    if (wh) { MEM32(wh + 16) = (MEM32(wh + 16) | WHDR_DONE) & ~WHDR_INQUEUE; }
    if (trace_on()) { static unsigned long n; if ((n++ % 120) == 0) atr("waveOutWrite: WAVEHDR retired silent (%lu)", n); }
    RET(MMSYSERR_NOERROR, 3);
}
static void m_waveOutPrepareHeader(void)   { uint32_t wh=A32(1); if(wh) MEM32(wh+16)|=WHDR_PREPARED; RET(MMSYSERR_NOERROR, 3); }
static void m_waveOutUnprepareHeader(void) { uint32_t wh=A32(1); if(wh) MEM32(wh+16)&=~WHDR_PREPARED; RET(MMSYSERR_NOERROR, 3); }
static void m_waveOutReset(void)       { RET(MMSYSERR_NOERROR, 1); }
static void m_waveOutClose(void)       { RET(MMSYSERR_NOERROR, 1); }
static void m_waveOutGetNumDevs(void)  { RET(1, 0); }          /* one (silent) device */
static void m_waveOutGetDevCapsA(void) { RET(MMSYSERR_NOERROR, 3); }

/* --- MCI (CD audio / command strings): not available --- */
static void m_mciSendStringA(void)     { atr("mciSendStringA(\"%s\"): MMSYSERR_ERROR (MCI not implemented)", ASTR(0)); RET(MMSYSERR_ERROR, 4); }
static void m_mciGetErrorStringA(void) { RET(0, 3); }          /* FALSE: no string */

/* --- mmio: RIFF files (WAVs) for reading ---
 * A file is read whole into memory at mmioOpenA, or a caller's buffer is
 * used as it stands (FOURCC_MEM). The direct-buffer calls (mmioGetInfo,
 * mmioAdvance, mmioSetInfo) then hand out that one buffer, which is the
 * whole file, so an advance never has to refill it. Writing is not done:
 * opening for write fails, as with a read-only disk. */
#define MMIOERR_CANNOTOPEN    257u
#define MMIOERR_CHUNKNOTFOUND 265u
#define FOURCC_MEM  0x204D454Du
#define FOURCC_RIFF 0x46464952u
#define FOURCC_LIST 0x5453494Cu
typedef struct { uint8_t *data; uint32_t len, pos; int owns; } hmmio;
static hmmio *MMIO(uint32_t h) { return (hmmio *)hle_handle_obj(h, HLE_H_MMIO); }
static void mmio_info(const hmmio *m, uint32_t info, uint32_t h) {   /* MMIOINFO over the whole file */
    MEM32(info + 20) = m->len, MEM32(info + 24) = hle_va(m->data);
    MEM32(info + 28) = hle_va(m->data) + m->pos, MEM32(info + 32) = hle_va(m->data) + m->len;
    MEM32(info + 36) = MEM32(info + 32), MEM32(info + 40) = 0, MEM32(info + 44) = m->len, MEM32(info + 68) = h;
}
static void m_mmioOpenA(void) {                          /* (name, MMIOINFO*, flags) */
    const char *name = ASTR(0);
    uint32_t info = A32(1), flags = A32(2);
    hmmio *m = (hmmio *)calloc(1, sizeof *m);
    if (info && MEM32(info + 4) == FOURCC_MEM && MEM32(info + 24)) {
        m->data = (uint8_t *)hle_ptr(MEM32(info + 24)), m->len = MEM32(info + 20);
    } else {
        char host[1024];
        FILE *f = !(flags & 0x1003u) && name && hle_host_path(name, host, sizeof host) ? fopen(host, "rb") : NULL;   /* not WRITE, READWRITE, CREATE */
        long n = -1;
        if (f && fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0) rewind(f);
        m->data = f && n >= 0 ? (uint8_t *)hle_alloc((uint32_t)n + 1) : NULL, m->len = (uint32_t)n, m->owns = 1;
        if (!m->data || fread(m->data, 1, (size_t)n, f) != (size_t)n) {
            if (f) fclose(f);
            hle_free(m->data), free(m);
            if (info) MEM32(info + 12) = MMIOERR_CANNOTOPEN;
            atr("mmioOpenA(\"%s\"): MMIOERR_CANNOTOPEN", name ? name : "(null)");
            RET(0, 3);
        }
        fclose(f);
    }
    uint32_t h = hle_handle_alloc(HLE_H_MMIO, m);
    if (info) MEM32(info + 12) = 0;
    atr("mmioOpenA(\"%s\", flags=%#x) -> handle %#x (%u bytes)", name ? name : "(mem)", flags, h, m->len);
    RET(h, 3);
}
static void m_mmioClose(void) {
    hmmio *m = MMIO(A32(0));
    if (!m) RET(MMSYSERR_ERROR, 2);
    if (m->owns) hle_free(m->data);
    free(m);
    hle_handle_close(A32(0));
    RET(0, 2);
}
static void m_mmioRead(void) {                           /* (h, buf, n) */
    hmmio *m = MMIO(A32(0));
    if (!m) RET(0xFFFFFFFFu, 3);
    uint32_t n = A32(2) < m->len - m->pos ? A32(2) : m->len - m->pos;
    memcpy(APTR(1), m->data + m->pos, n);
    m->pos += n;
    RET(n, 3);
}
static void m_mmioWrite(void) { RET(0xFFFFFFFFu, 3); }
static void m_mmioSeek(void) {                           /* (h, offset, origin) */
    hmmio *m = MMIO(A32(0));
    int64_t to = (int32_t)A32(1) + (int64_t)(A32(2) == 1 ? m ? m->pos : 0 : A32(2) == 2 ? m ? m->len : 0 : 0);
    if (!m || to < 0 || to > m->len) RET(0xFFFFFFFFu, 3);
    m->pos = (uint32_t)to;
    RET(m->pos, 3);
}
static void m_mmioDescend(void) {                        /* (h, MMCKINFO*, parent, flags) */
    hmmio *m = MMIO(A32(0));
    uint32_t ck = A32(1), parent = A32(2), flags = A32(3);
    uint32_t end = parent ? MEM32(parent + 12) + MEM32(parent + 4) : m ? m->len : 0;
    if (!m) RET(MMSYSERR_ERROR, 4);
    if (end > m->len) end = m->len;
    for (;;) {
        if (m->pos + 8 > end) RET(MMIOERR_CHUNKNOTFOUND, 4);
        uint32_t id, size, type = 0;
        memcpy(&id, m->data + m->pos, 4), memcpy(&size, m->data + m->pos + 4, 4);
        uint32_t data = m->pos + 8;
        int list = id == FOURCC_RIFF || id == FOURCC_LIST;
        if (list && data + 4 <= end) memcpy(&type, m->data + data, 4);
        int found = flags & 0x10 ? id == MEM32(ck)                                     /* FINDCHUNK */
                  : flags & 0x20 ? id == FOURCC_RIFF && type == MEM32(ck + 8)          /* FINDRIFF */
                  : flags & 0x40 ? id == FOURCC_LIST && type == MEM32(ck + 8) : 1;     /* FINDLIST */
        if (found) {
            MEM32(ck) = id, MEM32(ck + 4) = size, MEM32(ck + 8) = type, MEM32(ck + 12) = data, MEM32(ck + 16) = 0;
            m->pos = data + (list ? 4 : 0);
            RET(0, 4);
        }
        m->pos = data + size + (size & 1);
    }
}
static void m_mmioAscend(void) {                         /* (h, MMCKINFO*, flags) */
    hmmio *m = MMIO(A32(0));
    uint32_t ck = A32(1), to = MEM32(ck + 12) + MEM32(ck + 4);
    if (!m) RET(MMSYSERR_ERROR, 3);
    to += to & 1;
    m->pos = to < m->len ? to : m->len;
    RET(0, 3);
}
static void m_mmioGetInfo(void) {                        /* (h, MMIOINFO*, flags) */
    hmmio *m = MMIO(A32(0));
    if (!m) RET(MMSYSERR_ERROR, 3);
    memset(APTR(1), 0, 72);
    mmio_info(m, A32(1), A32(0));
    RET(0, 3);
}
static void m_mmioSetInfo(void) {                        /* (h, MMIOINFO*, flags): where pchNext got to */
    hmmio *m = MMIO(A32(0));
    if (!m) RET(MMSYSERR_ERROR, 3);
    uint32_t next = MEM32(A32(1) + 28) - hle_va(m->data);
    m->pos = next < m->len ? next : m->len;
    RET(0, 3);
}
static void m_mmioAdvance(void) {                        /* (h, MMIOINFO*, flags) */
    hmmio *m = MMIO(A32(0));
    if (!m) RET(MMSYSERR_ERROR, 3);
    if (A32(1)) { uint32_t next = MEM32(A32(1) + 28) - hle_va(m->data); m->pos = next < m->len ? next : m->len; mmio_info(m, A32(1), A32(0)); }
    RET(0, 3);
}
static void m_mmioSetBuffer(void) { RET(0, 4); }        /* the buffer is the whole file already */

/* mmioStringToFOURCCA(str, flags): the first four characters, padded with
 * spaces; MMIO_TOUPPER (0x10) upper-cases them. */
static void m_mmioStringToFOURCCA(void) {
    const char *s = ASTR(0);
    uint32_t fcc = 0;
    for (int i = 0, end = 0; i < 4; i++) {
        char c = !end && s && s[i] ? s[i] : (end = 1, ' ');
        if ((A32(1) & 0x10) && c >= 'a' && c <= 'z') c = (char)(c - 32);
        fcc |= (uint32_t)(uint8_t)c << (8 * i);
    }
    RET(fcc, 2);
}

const win32hle_shim win32hle_winmm[] = {
    { "mmioStringToFOURCCA",    m_mmioStringToFOURCCA },
    { "mmioOpenA", m_mmioOpenA }, { "mmioClose", m_mmioClose }, { "mmioRead", m_mmioRead }, { "mmioWrite", m_mmioWrite },
    { "mmioSeek", m_mmioSeek }, { "mmioDescend", m_mmioDescend }, { "mmioAscend", m_mmioAscend },
    { "mmioGetInfo", m_mmioGetInfo }, { "mmioSetInfo", m_mmioSetInfo }, { "mmioAdvance", m_mmioAdvance },
    { "mmioSetBuffer", m_mmioSetBuffer },
    { "timeGetTime",            m_timeGetTime },
    { "timeBeginPeriod",        m_timeBeginPeriod },
    { "timeEndPeriod",          m_timeEndPeriod },
    { "timeGetDevCaps",         m_timeGetDevCaps },
    { "timeSetEvent",           m_timeSetEvent },
    { "timeKillEvent",          m_timeKillEvent },
    { "joyGetNumDevs",          m_joyGetNumDevs },
    { "joyGetDevCapsA",         m_joyGetDevCapsA },
    { "joyGetPos",              m_joyGetPos },
    { "joyGetPosEx",            m_joyGetPosEx },
    { "waveOutOpen",            m_waveOutOpen },
    { "waveOutWrite",           m_waveOutWrite },
    { "waveOutPrepareHeader",   m_waveOutPrepareHeader },
    { "waveOutUnprepareHeader", m_waveOutUnprepareHeader },
    { "waveOutReset",           m_waveOutReset },
    { "waveOutClose",           m_waveOutClose },
    { "waveOutGetNumDevs",      m_waveOutGetNumDevs },
    { "waveOutGetDevCapsA",     m_waveOutGetDevCapsA },
    { "mciSendStringA",         m_mciSendStringA },
    { "mciGetErrorStringA",     m_mciGetErrorStringA },
    { 0, 0 }
};

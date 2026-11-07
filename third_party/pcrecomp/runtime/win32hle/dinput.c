/*
 * dinput.c - DirectInput 7 for a lifted Win32 game, on SDL2.
 *
 * Porsche.exe imports dinput.dll's DirectInputCreateA and drives the
 * IDirectInput7A / IDirectInputDevice7A facades (the public DX7 ABI). This
 * implements them over SDL2's keyboard state, mouse and joystick APIs:
 *
 *   IDirectInput7A:          CreateDevice, EnumDevices, GetDeviceStatus,
 *                            RunControlPanel, Initialize, FindDevice,
 *                            CreateDeviceEx            (IUnknown + 7 methods)
 *   IDirectInputDevice7A:    all 29 methods (IUnknown, Device, Device2,
 *                            Device7), on one vtable; the lower-version
 *                            interfaces are the same object
 *   devices:                 system keyboard, system mouse, SDL joysticks /
 *                            gamepads (one IDirectInputDevice per pad)
 *
 * Covered with real semantics: CreateDevice/Ex, SetDataFormat (any
 * DIDATAFORMAT, mapped by GUID/type), SetCooperativeLevel, Acquire/Unacquire,
 * GetDeviceState (256-byte keyboard, DIMOUSESTATE, DIJOYSTATE), GetDeviceData
 * (a buffered ring fed by state transitions), EnumDevices/EnumObjects,
 * GetCapabilities, GetObjectInfo, GetDeviceInfo, Get/SetProperty
 * (BUFFERSIZE, AXISMODE, RANGE, DEADZONE, SATURATION, GRANULARITY), Poll.
 *
 * Not emulated, by design: force feedback. Every CreateEffect/EnumEffects/
 * GetEffectInfo/GetForceFeedbackState/SendForceFeedbackCommand returns
 * DIERR_UNSUPPORTED and reports no DIDC_FORCEFEEDBACK capability, which is the
 * same thing DirectInput does on a device without FF. NFS5 uses force feedback
 * only when the device advertises it (dx7z screens it), so this is the
 * "no wheel with motors" path and does not block input. Documented in
 * docs/progress-hle.md.
 *
 * SDL integration: this file never consumes the SDL event queue (screen.c owns
 * it); it calls SDL_PumpEvents (non-destructive) and reads SDL's keyboard /
 * mouse / joystick *state*, which SDL keeps current for any pump. A pad is
 * opened lazily on Acquire and reopened if it is unplugged and replugged.
 * Headless (SDL_VIDEODRIVER=dummy) the keyboard state is simply all-released;
 * a virtual SDL joystick can be attached by a test (see dinput_selftest).
 *
 * Traces: NFS5_TRACE_DI=1 (one line per call, plus device creation) or
 *         win32hle_trace; NFS5_TRACE_INPUT=1 (the device state the game reads,
 *         only when it changes: axes/POV/buttons/keys).
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "win32hle.h"

/* ---- HRESULTs and DI constants (public DX7 ABI) ---- */
#define DI_OK                           0u
#define DI_NOEFFECT                     0x00000001u
#define DI_BUFFEROVERFLOW               0x00000001u
#define DI_NOTATTACHED                  0x00000001u
#define DIERR_OLDDIRECTINPUTVERSION     0x80070490u
#define DIERR_BETADIRECTINPUTVERSION    0x80070491u
#define DIERR_DEVICENOTREG              0x80040154u
#define DIERR_NOTFOUND                  0x80070002u
#define DIERR_OBJECTNOTFOUND            0x80070002u
#define DIERR_INVALIDPARAM              0x80070057u
#define DIERR_NOINTERFACE               0x80004002u
#define DIERR_NOTINITIALIZED            0x80070015u
#define DIERR_ALREADYINITIALIZED        0x800704DFu
#define DIERR_UNSUPPORTED               0x80004001u
#define DIERR_INPUTLOST                 0x8007001Eu
#define DIERR_NOTACQUIRED               0x8007000Cu
#define DIERR_UNPLUGGED                 0x80040209u
#define DIERR_NOTBUFFERED               0x80040207u
#define DIERR_NOTEXCLUSIVEACQUIRED      0x80040205u

#define DIDEVTYPE_DEVICE                1
#define DIDEVTYPE_MOUSE                 2
#define DIDEVTYPE_KEYBOARD              3
#define DIDEVTYPE_JOYSTICK              4

#define DIDC_ATTACHED                   0x00000001u
#define DIDC_POLLEDDEVICE               0x00000002u
#define DIDC_EMULATED                   0x00000004u

#define DIDF_ABSAXIS                    0x00000001u
#define DIDF_RELAXIS                    0x00000002u

#define DIDFT_RELAXIS                   0x01u
#define DIDFT_ABSAXIS                   0x02u
#define DIDFT_AXIS                      0x03u
#define DIDFT_PSHBUTTON                 0x04u
#define DIDFT_TGLBUTTON                 0x08u
#define DIDFT_BUTTON                    0x0Cu
#define DIDFT_POV                       0x10u
#define DIDFT_COLLECTION                0x40u
#define DIDFT_NODATA                    0x80u
#define DIDFT_ANYINSTANCE               0x00FFFF00u
#define DIDFT_MAKEINSTANCE(n)           ((uint32_t)(uint16_t)(n) << 8)

#define DIPH_DEVICE                     0u
#define DIPH_BYOFFSET                   1u
#define DIPH_BYID                       2u
#define DIPH_BYUSAGE                    3u

#define DIPROP_BUFFERSIZE               1u
#define DIPROP_AXISMODE                 2u
#define DIPROP_GRANULARITY              3u
#define DIPROP_RANGE                    4u
#define DIPROP_DEADZONE                 5u
#define DIPROP_SATURATION               6u
#define DIPROP_FFGAIN                   7u
#define DIPROP_FFLOAD                   8u
#define DIPROP_CALIBRATIONMODE          10u

#define DIGDD_PEEK                      1u

#define DISCL_EXCLUSIVE                 0x00000001u
#define DISCL_NONEXCLUSIVE              0x00000002u
#define DISCL_FOREGROUND                0x00000004u
#define DISCL_BACKGROUND                0x00000008u

#define DIENUM_STOP                     0u
#define DIENUM_CONTINUE                 1u

/* ---- the public GUIDs this file has to recognise (little-endian bytes) ---- */
#define G1(g) ((uint32_t)(g)[0] | ((uint32_t)(g)[1] << 8) | ((uint32_t)(g)[2] << 16) | ((uint32_t)(g)[3] << 24))
static const uint8_t guid_sys_mouse[16]    = { 0x60,0x2B,0x1D,0x6F,0xA0,0xD5,0xCF,0x11,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00 };
static const uint8_t guid_sys_keyboard[16] = { 0x61,0x2B,0x1D,0x6F,0xA0,0xD5,0xCF,0x11,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00 };
static const uint8_t guid_joystick[16]     = { 0x70,0x2B,0x1D,0x6F,0xA0,0xD5,0xCF,0x11,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00 };
/* axis/object type GUIDs, by first dword (all share the C9F3 11CF BFC7... tail) */
#define D1_XAXIS    0xA36D02E0u
#define D1_YAXIS    0xA36D02E1u
#define D1_ZAXIS    0xA36D02E2u
#define D1_RXAXIS   0xA36D02F4u
#define D1_RYAXIS   0xA36D02F5u
#define D1_RZAXIS   0xA36D02E3u
#define D1_SLIDER   0xA36D02E4u
#define D1_BUTTON   0xA36D02F0u
#define D1_KEY      0x55728220u
#define D1_POV      0xA36D02F2u
static const uint8_t guid_tail[12] = { 0xF3,0xC9,0xCF,0x11,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00 };

/* interface IIDs, by first dword */
#define IID_DI_A        0x89521360u
#define IID_DI2_A       0x5944E662u
#define IID_DI7_A       0x9A4CB684u
#define IID_DEV_A       0x5944E680u
#define IID_DEV2_A      0x5944E682u
#define IID_DEV7_A      0x57D7C6BCu

/* ---- devices ---- */
#define DI_MAX_DEV   16
#define DI_MAX_OBJ   256
#define DI_RING      256
#define DI_CANON     512

typedef struct diobj { uint32_t pguid, ofs, type, flags; } diobj_t;

typedef struct didev {
    int used, kind;                    /* 1 keyboard, 2 mouse, 3 joystick */
    int ordinal;                       /* SDL joystick index for kind 3 */
    uint32_t self;
    uint8_t guid[16];
    uint32_t datasize;                 /* DIDATAFORMAT.dwDataSize, 0 until SetDataFormat */
    uint32_t format_flags;
    uint32_t nobj;
    diobj_t obj[DI_MAX_OBJ];
    uint32_t hwnd, coop;
    uint32_t notif;                    /* SetEventNotification's event handle (0 = none) */
    int changed;                       /* last poll_device saw a state change */
    int acquired, relative;
    uint32_t buffer_size;
    uint32_t ring_head, ring_count, overflow;
    struct { uint32_t ofs, data, stamp, seq; } ring[DI_RING];
    int32_t range_min[8], range_max[8];
    int32_t deadzone[8], saturation[8];
    uint32_t seq;
    int prev_valid;
    uint8_t prev[DI_CANON];
    SDL_Joystick *pad;
    int traced;
} didev_t;

/* Host-injected system-mouse state (the handheld pad bridge): the front end is
 * mouse-driven and has no mouse, so its cursor/click come from here instead of
 * SDL's mouse. Absolute position plus relative delta, as games use both. */
static int g_minject;
static int32_t g_mouse_x = 320, g_mouse_y = 240;
static int32_t g_mouse_dx, g_mouse_dy;
static uint32_t g_mouse_buttons;
void hle_dinput_mouse_set(int x, int y, int buttons) {
    if (!g_minject) { g_mouse_dx = x - g_mouse_x; g_mouse_dy = y - g_mouse_y; g_minject = 1; }
    else { g_mouse_dx += x - g_mouse_x; g_mouse_dy += y - g_mouse_y; }
    g_mouse_x = x; g_mouse_y = y;
    g_mouse_buttons = (uint32_t)(buttons & 7);
}
void hle_dinput_mouse_buttons(int buttons) { g_mouse_buttons = (uint32_t)(buttons & 7); }

static didev_t g_dev[DI_MAX_DEV];
static uint32_t g_vt_di, g_vt_dev;
static uint32_t g_di;                          /* the one DirectInput object (all versions) */
static int g_trace_calls = -1, g_trace_state = -1;
static int g_sdl_ready;
static int8_t g_dik_of_scancode[SDL_NUM_SCANCODES];
static uint32_t g_guid_va[16];                 /* guest copies of the object GUIDs */
static int g_guid_ready;

static int trace_calls(void) {
    if (g_trace_calls < 0) g_trace_calls = (getenv("NFS5_TRACE_DI") || win32hle_trace) ? 1 : 0;
    return g_trace_calls;
}
static int trace_state(void) {
    if (g_trace_state < 0) g_trace_state = getenv("NFS5_TRACE_INPUT") ? 1 : 0;
    return g_trace_state;
}
#define DITRACE(...) do { if (trace_calls()) fprintf(stderr, "[dinput] " __VA_ARGS__); } while (0)

/* ---- SDL ---- */
static int sdl_ready(void) {
    if (g_sdl_ready) return 1;
    if (SDL_InitSubSystem(SDL_INIT_EVENTS) != 0 && SDL_WasInit(SDL_INIT_EVENTS) == 0) {
        fprintf(stderr, "[dinput] SDL events: %s\n", SDL_GetError());
        return 0;
    }
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) != 0 && SDL_WasInit(SDL_INIT_JOYSTICK) == 0) {
        fprintf(stderr, "[dinput] SDL joystick: %s\n", SDL_GetError());
        return 0;
    }
    g_sdl_ready = 1;
    return 1;
}
/* NFS5_PAD_ONLY=vvvv:pppp (hex USB vendor:product): use only that joystick.
 * Under PortMaster, gptokeyb's xbox360 mode turns whatever controller the
 * device has into one virtual Xbox 360 pad (045e:028e); the game reads that
 * and ignores the physical pad (no doubled input, same layout everywhere). */
int hle_pad_allowed(int index) {
    static int only = -1;
    static unsigned vid, pid;
    if (only < 0) {
        const char *v = getenv("NFS5_PAD_ONLY");
        only = v && sscanf(v, "%x:%x", &vid, &pid) == 2;
        if (only) fprintf(stderr, "[dinput] only joystick %04x:%04x is used\n", vid, pid);
    }
    if (!only) return 1;
    return SDL_JoystickGetDeviceVendor(index) == (Uint16)vid && SDL_JoystickGetDeviceProduct(index) == (Uint16)pid;
}
static int pad_index(int ordinal) {            /* the ordinal-th allowed SDL joystick */
    int n = SDL_NumJoysticks(), k = 0;
    for (int i = 0; i < n; i++)
        if (hle_pad_allowed(i) && k++ == ordinal) return i;
    return -1;
}
static int pad_count(void) {
    sdl_ready();
    int n = SDL_NumJoysticks(), k = 0;
    for (int i = 0; i < n; i++) k += hle_pad_allowed(i);
    return k;
}
static SDL_Joystick *pad_get(didev_t *d) {
    if (d->pad && SDL_JoystickGetAttached(d->pad)) return d->pad;
    if (d->pad) { SDL_JoystickClose(d->pad); d->pad = NULL; }
    if (d->ordinal < 0 || d->ordinal >= pad_count()) return NULL;
    d->pad = SDL_JoystickOpen(pad_index(d->ordinal));
    if (!d->pad) DITRACE("SDL_JoystickOpen(%d): %s\n", d->ordinal, SDL_GetError());
    return d->pad;
}
static void pad_close(didev_t *d) {
    if (d->pad) SDL_JoystickClose(d->pad);
    d->pad = NULL;
}

/* ---- the keyboard's DIK codes, from SDL scancodes ---- */
static unsigned dik_of(SDL_Scancode s) {
    static const uint8_t letters[26] = { 0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
                                         0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C };
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return letters[s - SDL_SCANCODE_A];
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return 2u + (unsigned)(s - SDL_SCANCODE_1);
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F10) return 0x3Bu + (unsigned)(s - SDL_SCANCODE_F1);
    switch (s) {
    case SDL_SCANCODE_0: return 0x0B;   case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_RETURN: return 0x1C; case SDL_SCANCODE_TAB: return 0x0F;
    case SDL_SCANCODE_BACKSPACE: return 0x0E; case SDL_SCANCODE_SPACE: return 0x39;
    case SDL_SCANCODE_LCTRL: return 0x1D; case SDL_SCANCODE_RCTRL: return 0x9D;
    case SDL_SCANCODE_LSHIFT: return 0x2A; case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_LALT: return 0x38;  case SDL_SCANCODE_RALT: return 0xB8;
    case SDL_SCANCODE_MINUS: return 0x0C; case SDL_SCANCODE_EQUALS: return 0x0D;
    case SDL_SCANCODE_LEFTBRACKET: return 0x1A; case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
    case SDL_SCANCODE_SEMICOLON: return 0x27; case SDL_SCANCODE_APOSTROPHE: return 0x28;
    case SDL_SCANCODE_GRAVE: return 0x29; case SDL_SCANCODE_BACKSLASH: return 0x2B;
    case SDL_SCANCODE_COMMA: return 0x33; case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35;
    case SDL_SCANCODE_CAPSLOCK: return 0x3A; case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45;
    case SDL_SCANCODE_SCROLLLOCK: return 0x46;
    case SDL_SCANCODE_KP_7: return 0x47; case SDL_SCANCODE_KP_8: return 0x48; case SDL_SCANCODE_KP_9: return 0x49;
    case SDL_SCANCODE_KP_MINUS: return 0x4A; case SDL_SCANCODE_KP_4: return 0x4B;
    case SDL_SCANCODE_KP_5: return 0x4C; case SDL_SCANCODE_KP_6: return 0x4D;
    case SDL_SCANCODE_KP_PLUS: return 0x4E; case SDL_SCANCODE_KP_1: return 0x4F;
    case SDL_SCANCODE_KP_2: return 0x50; case SDL_SCANCODE_KP_3: return 0x51;
    case SDL_SCANCODE_KP_0: return 0x52; case SDL_SCANCODE_KP_PERIOD: return 0x53;
    case SDL_SCANCODE_F11: return 0x57; case SDL_SCANCODE_F12: return 0x58;
    case SDL_SCANCODE_KP_ENTER: return 0x9C; case SDL_SCANCODE_KP_DIVIDE: return 0xB5;
    case SDL_SCANCODE_HOME: return 0xC7; case SDL_SCANCODE_UP: return 0xC8;
    case SDL_SCANCODE_PAGEUP: return 0xC9; case SDL_SCANCODE_LEFT: return 0xCB;
    case SDL_SCANCODE_RIGHT: return 0xCD; case SDL_SCANCODE_END: return 0xCF;
    case SDL_SCANCODE_DOWN: return 0xD0; case SDL_SCANCODE_PAGEDOWN: return 0xD1;
    case SDL_SCANCODE_INSERT: return 0xD2; case SDL_SCANCODE_DELETE: return 0xD3;
    case SDL_SCANCODE_LGUI: return 0xDB; case SDL_SCANCODE_RGUI: return 0xDC;
    case SDL_SCANCODE_APPLICATION: return 0xDD;
    case SDL_SCANCODE_PAUSE: return 0xC5; case SDL_SCANCODE_PRINTSCREEN: return 0xB7;
    case SDL_SCANCODE_MUTE: return 0xA0; case SDL_SCANCODE_VOLUMEUP: return 0xB0;
    case SDL_SCANCODE_VOLUMEDOWN: return 0xAE;
    default: return 0;
    }
}
/* ---- host input injection (headless validation) ----
 * A script has no keyboard events, so it drives the game by name: the injected
 * keys are OR-ed into the DIK state GetDeviceState reports (and into user32's
 * key state, so GetAsyncKeyState agrees). Zero cost when unused. */
static uint8_t g_inject[256];
static unsigned dik_from_vk(uint32_t vk) {
    static const uint8_t letters[26] = { 0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
                                         0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C };
    if (vk >= 'A' && vk <= 'Z') return letters[vk - 'A'];
    if (vk >= 0x70 && vk <= 0x7B) return 0x3Bu + (vk - 0x70);                 /* F1..F12 */
    if (vk >= 0x60 && vk <= 0x69) { static const uint8_t kp[10] = { 0x52,0x4F,0x50,0x51,0x4B,0x4C,0x4D,0x47,0x48,0x49 }; return kp[vk - 0x60]; }
    switch (vk) {
    case 0x1B: return 0x01; case 0x0D: return 0x1C; case 0x09: return 0x0F; case 0x08: return 0x0E;
    case 0x20: return 0x39; case 0x10: return 0x2A; case 0x11: return 0x1D; case 0x12: return 0x38;
    case 0x26: return 0xC8; case 0x28: return 0xD0; case 0x25: return 0xCB; case 0x27: return 0xCD;
    case 0x21: return 0xC9; case 0x22: return 0xD1; case 0x23: return 0xCF; case 0x24: return 0xC7;
    case 0x2D: return 0xD2; case 0x2E: return 0xD3; case 0x14: return 0x3A; case 0x90: return 0x45;
    case 0x5B: return 0xDB; case 0x5C: return 0xDC; case 0x5D: return 0xDD;
    case 0xBA: return 0x27; case 0xBB: return 0x0D; case 0xBC: return 0x33; case 0xBD: return 0x0C;
    case 0xBE: return 0x34; case 0xBF: return 0x35; case 0xC0: return 0x29; case 0xDB: return 0x1A;
    case 0xDC: return 0x2B; case 0xDD: return 0x1B; default: return 0;
    }
}
void nfs5_dinput_inject_key(uint32_t vk, int down) {
    unsigned k = 0;
    if (vk >= '0' && vk <= '9') { static const uint8_t num[10] = { 0x0B,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A }; k = num[vk - '0']; }
    else k = dik_from_vk(vk);
    if (k && k < 256) g_inject[k] = down ? 0x80 : 0;
    hle_input_key(down ? 1 : 0, vk, k);
}
void nfs5_dinput_release_all(void) {
    memset(g_inject, 0, sizeof g_inject);
}
/* A headless validation script: NFS5_INPUT_SCRIPT="1000:0x0D:1,1200:0x0D:0,..."
 * or a path whose lines are "ms vk down" (# comments). Times are relative to
 * the first keyboard poll; it drives menus/races with no keyboard events. */
typedef struct { uint32_t at, vk; int down; } inject_ev;
static inject_ev g_script[256];
static int g_script_n = -1, g_script_i;
static uint32_t g_script_t0;
static uint32_t parse_vk(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return (uint32_t)strtoul(s, NULL, 0);
}
static void script_load(void) {
    if (g_script_n >= 0) return;
    g_script_n = 0;
    g_script_t0 = hle_ticks_ms();
    const char *v = getenv("NFS5_INPUT_SCRIPT");
    if (!v || !*v) return;
    FILE *f = NULL;
    if (strchr(v, ':') || strchr(v, ',')) {
        const char *p = v;
        while (*p && g_script_n < (int)(sizeof g_script / sizeof *g_script)) {
            char buf[128];
            size_t n = strcspn(p, ",");
            if (n >= sizeof buf) n = sizeof buf - 1;
            memcpy(buf, p, n), buf[n] = 0;
            unsigned long at = strtoul(buf, NULL, 10);
            char *c1 = strchr(buf, ':');
            if (c1) {
                uint32_t vk = parse_vk(c1 + 1);
                char *c2 = strchr(c1 + 1, ':');
                int down = c2 ? (int)strtol(c2 + 1, NULL, 10) : 1;
                g_script[g_script_n++] = (inject_ev){ (uint32_t)at, vk, down };
            }
            p += n;
            while (*p == ',') p++;
        }
    } else if ((f = fopen(v, "r"))) {
        char line[256];
        while (fgets(line, sizeof line, f) && g_script_n < (int)(sizeof g_script / sizeof *g_script)) {
            char *hash = strchr(line, '#');
            if (hash) *hash = 0;
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (!*p || *p == '\n' || *p == '\r') continue;
            unsigned long at = strtoul(p, &p, 10);
            uint32_t vk = parse_vk(p);
            int down = 1;
            char *q = p;
            while (*q == ' ' || *q == '\t') q++;
            while (*q && *q != ' ' && *q != '\t') q++;
            if (*q) down = (int)strtol(q, NULL, 10);
            g_script[g_script_n++] = (inject_ev){ (uint32_t)at, vk, down };
        }
        fclose(f);
    } else {
        fprintf(stderr, "[dinput] NFS5_INPUT_SCRIPT %s: cannot read\n", v);
    }
    if (g_script_n && trace_calls())
        fprintf(stderr, "[dinput] input script loaded: %d events\n", g_script_n);
}
static void script_run(void) {
    script_load();
    uint32_t now = hle_ticks_ms() - g_script_t0;
    while (g_script_i < g_script_n && now >= g_script[g_script_i].at) {
        inject_ev *e = &g_script[g_script_i++];
        nfs5_dinput_inject_key(e->vk, e->down);
        if (trace_calls() || trace_state())
            fprintf(stderr, "[dinput-script] t=%u vk=%#x %s\n", e->at, e->vk, e->down ? "down" : "up");
    }
}
static void build_kbd_map(void) {
    if (g_dik_of_scancode[SDL_SCANCODE_A]) return;
    for (int s = 0; s < SDL_NUM_SCANCODES; s++) g_dik_of_scancode[s] = (int8_t)dik_of((SDL_Scancode)s);
}

/* ---- device slots ---- */
static didev_t *device_of(uint32_t self) {
    if (!self || !g_vt_dev || MEM32(self) != g_vt_dev) return NULL;
    uint32_t i = MEM32(self + 8);
    if (i >= DI_MAX_DEV || !g_dev[i].used || g_dev[i].self != self) return NULL;
    return &g_dev[i];
}
static uint32_t device_new(const uint8_t *guid, int kind, int ordinal) {
    for (int i = 0; i < DI_MAX_DEV; i++) {
        if (g_dev[i].used) continue;
        didev_t *d = &g_dev[i];
        memset(d, 0, sizeof *d);
        d->used = 1, d->kind = kind, d->ordinal = ordinal;
        memcpy(d->guid, guid, 16);
        d->datasize = kind == 1 ? 256u : kind == 2 ? 20u : 80u;
        d->saturation[0] = -1;
        /* DirectInput's default joystick axis range is 0..65535 (centre 32767);
         * NFS5 never sets one and reads a centred stick as 128 of 255. A
         * -1000..1000 default made it read 0: full left steer and full gas. */
        for (int a = 0; a < 8; a++) { d->range_min[a] = 0; d->range_max[a] = 65535; d->saturation[a] = 10000; }
        d->self = hle_com_new(g_vt_dev, 12);
        MEM32(d->self + 8) = (uint32_t)i;
        return d->self;
    }
    return 0;
}
static const char *kind_name(int kind) { return kind == 1 ? "keyboard" : kind == 2 ? "mouse" : "joystick"; }
static void guid_of_kind(uint8_t *out, int kind, int ordinal) {
    if (kind == 1) memcpy(out, guid_sys_keyboard, 16);
    else if (kind == 2) memcpy(out, guid_sys_mouse, 16);
    else { memcpy(out, guid_joystick, 16); out[15] = (uint8_t)ordinal; }
}
static int guid_to_kind(const uint8_t *g, int *ordinal) {
    uint32_t d1 = G1(g);
    *ordinal = 0;
    if (d1 == 0x6F1D2B61u) return 1;
    if (d1 == 0x6F1D2B60u) return 2;
    if (d1 == 0x6F1D2B70u) { *ordinal = g[15]; return 3; }
    return 0;
}

/* ---- canonical device state (what the device would report) ---- */
static int32_t scale_axis(didev_t *d, int axis, int32_t raw) {
    int32_t lo = d->range_min[axis], hi = d->range_max[axis];
    if (lo > hi) { int32_t t = lo; lo = hi; hi = t; }
    int32_t dz = d->deadzone[axis], sat = d->saturation[axis];
    if (dz < 0) dz = 0;
    if (dz > 10000) dz = 10000;
    if (sat <= 0 || sat > 10000) sat = 10000;
    if (sat <= dz) sat = dz + 1;
    int64_t v = raw;
    if (dz == 0 && sat >= 10000) {                       /* no reshaping: the raw axis 1:1 */
        int64_t out = (int64_t)lo + ((v + 32768LL) * ((int64_t)hi - lo)) / 65535LL;
        if (out < lo) out = lo;
        if (out > hi) out = hi;
        return (int32_t)out;
    }
    int64_t dead = (32768LL * dz) / 10000, limit = (32768LL * sat) / 10000;
    if (v < 0) {
        if (-v <= dead) v = 0;
        else { v = -(((-v - dead) * 32767LL) / (limit - dead)); if (v < -32768) v = -32768; }
    } else {
        if (v <= dead) v = 0;
        else { v = ((v - dead) * 32767LL) / (limit - dead); if (v > 32767) v = 32767; }
    }
    int64_t out = (int64_t)lo + ((v + 32768LL) * ((int64_t)hi - lo)) / 65535LL;
    if (out < lo) out = lo;
    if (out > hi) out = hi;
    return (int32_t)out;
}
static uint32_t hat_to_pov(int hat) {
    static const uint32_t deg[16] = { 0xFFFFFFFFu, 0u, 9000u, 4500u, 18000u, 0xFFFFFFFFu, 13500u, 0xFFFFFFFFu,
                                      27000u, 31500u, 0xFFFFFFFFu, 0xFFFFFFFFu, 22500u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    return deg[hat & 15];
}
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* Fill the canonical state for a device's kind. Returns 0 if the hardware is
 * not there (a joystick that got unplugged). Sizes: keyboard 256, mouse 20,
 * joystick DIJOYSTATE 80 (6 axes, 2 sliders, 4 POVs, 32 buttons). */
static int build_canon(didev_t *d, uint8_t *c) {
    memset(c, 0, DI_CANON);
    if (d->kind == 1) {
        sdl_ready();
        SDL_PumpEvents();
        build_kbd_map();
        const Uint8 *ks = SDL_GetKeyboardState(NULL);
        for (int s = 0; s < SDL_NUM_SCANCODES; s++) {
            unsigned k = (unsigned)(uint8_t)g_dik_of_scancode[s];
            if (k && ks[s]) c[k] = 0x80;
        }
        script_run();
        for (int k = 0; k < 256; k++) if (g_inject[k]) c[k] = g_inject[k];   /* a script's keys */
        return 1;
    }
    if (d->kind == 2) {
        if (g_minject) {
            if (d->relative) { put32(c + 0, (uint32_t)g_mouse_dx); put32(c + 4, (uint32_t)g_mouse_dy); }
            else { put32(c + 0, (uint32_t)g_mouse_x); put32(c + 4, (uint32_t)g_mouse_y); }
            put32(c + 8, 0);
            for (int b = 0; b < 8; b++) c[12 + b] = (g_mouse_buttons & (1u << b)) ? 0x80 : 0;
            /* the relative delta is consumed once, like SDL_GetRelativeMouseState */
            if (d->relative) g_mouse_dx = g_mouse_dy = 0;
            return 1;
        }
        sdl_ready();
        SDL_PumpEvents();
        int x = 0, y = 0;
        uint32_t buttons;
        if (d->relative) { buttons = SDL_GetRelativeMouseState(&x, &y); }
        else { buttons = SDL_GetMouseState(&x, &y); }
        put32(c + 0, (uint32_t)x);
        put32(c + 4, (uint32_t)y);
        put32(c + 8, 0);                                   /* wheel: not tracked (see file comment) */
        for (int b = 0; b < 8; b++) c[12 + b] = (buttons & (1u << b)) ? 0x80 : 0;
        return 1;
    }
    /* NFS5 handheld: a virtual racing wheel/pad laid out for the game's own
     * joystick bindings, driven by the launcher's controller mapping */
    {
        extern int hle_pad_virtual_joy(int32_t ax[8], uint8_t bt[32], uint32_t pov[4]) __attribute__((weak));
        int32_t vax[8]; uint8_t vbt[32]; uint32_t vpov[4];
        if (hle_pad_virtual_joy && d->ordinal == 0 && hle_pad_virtual_joy(vax, vbt, vpov)) {
            for (int a = 0; a < 8; a++) put32(c + 4 * a, (uint32_t)scale_axis(d, a, vax[a]));
            for (int p = 0; p < 4; p++) put32(c + 32 + 4 * p, vpov[p]);
            for (int b = 0; b < 32; b++) c[48 + b] = vbt[b] ? 0x80 : 0;
            return 1;
        }
    }
    sdl_ready();
    SDL_Joystick *j = pad_get(d);
    if (!j) return 0;
    SDL_JoystickUpdate();
    int naxes = SDL_JoystickNumAxes(j), nbuttons = SDL_JoystickNumButtons(j), nhats = SDL_JoystickNumHats(j);
    for (int a = 0; a < 8; a++) {
        int32_t raw = a < naxes ? (int32_t)SDL_JoystickGetAxis(j, a) : 0;
        put32(c + 4 * a, (uint32_t)scale_axis(d, a, raw));
    }
    for (int p = 0; p < 4; p++) {
        int hat = p < nhats ? SDL_JoystickGetHat(j, p) : SDL_HAT_CENTERED;
        put32(c + 32 + 4 * p, hat_to_pov(hat));
    }
    for (int b = 0; b < 32; b++)
        c[48 + b] = (b < nbuttons && SDL_JoystickGetButton(j, b)) ? 0x80 : 0;
    return 1;
}

/* Canonical offset and width for a data-format object, or -1. */
static int obj_canon(const didev_t *d, const diobj_t *o, int *width) {
    uint32_t t = o->type & 0xFFu;
    uint32_t inst = (o->type >> 8) & 0xFFFFu;
    uint32_t g1 = o->pguid ? MEM32(o->pguid) : 0;
    if (d->kind == 1) {                                    /* keyboard */
        if (t & DIDFT_BUTTON) { *width = 1; return (int)(inst & 0xFFu); }
        return -1;
    }
    if (d->kind == 2) {                                    /* mouse */
        if (t & DIDFT_BUTTON) { *width = 1; return 12 + (int)(inst & 7u); }
        if (g1 == D1_XAXIS) { *width = 4; return 0; }
        if (g1 == D1_YAXIS) { *width = 4; return 4; }
        if (g1 == D1_ZAXIS) { *width = 4; return 8; }
        return -1;
    }
    /* joystick. Data formats like c_dfDIJoystick declare buttons and POVs
     * with DIDFT_ANYINSTANCE (instance 0xFFFF): DirectInput then hands out
     * the device's buttons/POVs in order, so the n-th such entry is button n.
     * Taking the instance bits literally sent all 32 to button 31. */
    if (t & (DIDFT_POV | DIDFT_BUTTON)) {
        int n = (int)inst;
        if (inst == 0xFFFFu) {
            n = 0;
            for (const diobj_t *q = d->obj; q < o; q++) {
                uint32_t qt = q->type & 0xFFu;
                if (((q->type >> 8) & 0xFFFFu) != 0xFFFFu) continue;
                if ((t & DIDFT_POV) ? (qt & DIDFT_POV) != 0 : ((qt & DIDFT_BUTTON) && !(qt & DIDFT_POV))) n++;
            }
        }
        if (t & DIDFT_POV) { *width = 4; return 32 + 4 * (n & 3); }
        *width = 1;
        return 48 + (n & 31);
    }
    switch (g1) {
    case D1_XAXIS:  *width = 4; return 0;
    case D1_YAXIS:  *width = 4; return 4;
    case D1_ZAXIS:  *width = 4; return 8;
    case D1_RXAXIS: *width = 4; return 12;
    case D1_RYAXIS: *width = 4; return 16;
    case D1_RZAXIS: *width = 4; return 20;
    case D1_SLIDER: *width = 4; return 24 + 4 * (int)(inst & 1u);
    default: break;
    }
    if (t & DIDFT_AXIS) { *width = 4; return (int)((inst & 7u) * 4u); }
    return -1;
}

/* Copy the canonical state into the layout the game's DIDATAFORMAT asks for. */
static void encode_state(didev_t *d, const uint8_t *c, uint8_t *out) {
    memset(out, 0, d->datasize);
    if (!d->nobj) { memcpy(out, c, d->datasize < DI_CANON ? d->datasize : DI_CANON); return; }
    for (uint32_t i = 0; i < d->nobj; i++) {
        int w = 0, src = obj_canon(d, &d->obj[i], &w);
        if (src < 0) continue;
        if (d->obj[i].ofs + (uint32_t)w > d->datasize) continue;
        memcpy(out + d->obj[i].ofs, c + src, (size_t)w);
    }
}

/* ---- buffered input: state changes become DIDEVICEOBJECTDATA ---- */
static void ring_push(didev_t *d, uint32_t ofs, uint32_t data) {
    uint32_t cap = d->buffer_size;
    if (!cap) return;
    if (cap > DI_RING) cap = DI_RING;
    if (d->ring_count >= cap) {
        d->ring_head = (d->ring_head + 1) % DI_RING;
        d->ring_count--;
        d->overflow = 1;
    }
    uint32_t at = (d->ring_head + d->ring_count) % DI_RING;
    d->ring[at].ofs = ofs;
    d->ring[at].data = data;
    d->ring[at].stamp = hle_ticks_ms();
    d->ring[at].seq = d->seq++;
    d->ring_count++;
}
static uint32_t value_at(const uint8_t *p, int w) {
    if (w == 1) return p[0];
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
/* Watch the device: encode its state now and turn differences since the last
 * look into buffered events. Called from GetDeviceState, GetDeviceData and
 * Poll, which is when a DirectInput game expects input to have been polled. */
static int poll_device(didev_t *d) {
    uint8_t c[DI_CANON], cur[DI_CANON];
    if (!d->datasize || d->datasize > DI_CANON) return 0;
    if (!build_canon(d, c)) return 0;
    encode_state(d, c, cur);
    int changed = 0;
    if (d->nobj) {
        for (uint32_t i = 0; i < d->nobj; i++) {
            int w = 0, src = obj_canon(d, &d->obj[i], &w);
            if (src < 0 || d->obj[i].ofs + (uint32_t)w > d->datasize) continue;
            uint32_t now = value_at(cur + d->obj[i].ofs, w);
            uint32_t was = d->prev_valid ? value_at(d->prev + d->obj[i].ofs, w) : now;
            if (d->prev_valid && now != was) { ring_push(d, d->obj[i].ofs, now); changed = 1; }
        }
    } else if (d->prev_valid) {
        for (uint32_t i = 0; i < d->datasize; i++)
            if (cur[i] != d->prev[i]) { ring_push(d, i, cur[i]); changed = 1; }
    }
    if (trace_state() && changed) {
        /* the values the game sees, when they change: headless input evidence */
        fprintf(stderr, "[dinput-state] %s%d", kind_name(d->kind), d->ordinal);
        if (d->kind == 3)
            fprintf(stderr, " x=%d y=%d z=%d rx=%d ry=%d rz=%d pov0=%d buttons=%02x%02x%02x%02x",
                    (int32_t)value_at(cur, 4), (int32_t)value_at(cur + 4, 4), (int32_t)value_at(cur + 8, 4),
                    (int32_t)value_at(cur + 12, 4), (int32_t)value_at(cur + 16, 4), (int32_t)value_at(cur + 20, 4),
                    (int32_t)value_at(cur + 32, 4), cur[48], cur[49], cur[50], cur[51]);
        else
            fprintf(stderr, " buttons=%02x%02x%02x%02x", cur[12], cur[13], cur[14], cur[15]);
        fprintf(stderr, "\n");
    }
    memcpy(d->prev, cur, d->datasize);
    d->prev_valid = 1;
    d->changed = changed;
    return 1;
}

/* ---- the object GUIDs, in guest memory, for the enumerators ---- */
static uint32_t obj_guid_va(int which) {
    if (!g_guid_ready) {
        static const struct { uint32_t d1; uint32_t d2; uint16_t d3; } tab[12] = {
            { D1_XAXIS, 0xC9F3, 0x11CF }, { D1_YAXIS, 0xC9F3, 0x11CF }, { D1_ZAXIS, 0xC9F3, 0x11CF },
            { D1_RXAXIS, 0xC9F3, 0x11CF }, { D1_RYAXIS, 0xC9F3, 0x11CF }, { D1_RZAXIS, 0xC9F3, 0x11CF },
            { D1_SLIDER, 0xC9F3, 0x11CF }, { D1_BUTTON, 0xC9F3, 0x11CF }, { D1_POV, 0xC9F3, 0x11CF },
            { D1_KEY, 0xD33C, 0x11CF }, { 0x6F1D2B61u, 0xD5A0, 0x11CF }, { 0x6F1D2B70u, 0xD5A0, 0x11CF },
        };
        for (int i = 0; i < 12; i++) {
            uint8_t *p = (uint8_t *)hle_alloc(16);
            uint32_t d1 = tab[i].d1;
            p[0] = (uint8_t)d1; p[1] = (uint8_t)(d1 >> 8); p[2] = (uint8_t)(d1 >> 16); p[3] = (uint8_t)(d1 >> 24);
            p[4] = (uint8_t)tab[i].d2; p[5] = (uint8_t)(tab[i].d2 >> 8);
            p[6] = (uint8_t)tab[i].d3; p[7] = (uint8_t)(tab[i].d3 >> 8);
            memcpy(p + 8, guid_tail + 2, 8);              /* BFC7 4445 5354 0000 */
            g_guid_va[i] = hle_va(p);
        }
        g_guid_ready = 1;
    }
    return g_guid_va[which];
}

/* The default objects a device has before SetDataFormat (or if a game enumerates
 * without setting one): keyboard 256 keys; mouse 3 axes + 8 buttons; joystick
 * 8 axes + 4 POVs + 32 buttons. */
static uint32_t default_objects(didev_t *d, diobj_t *out) {
    uint32_t n = 0;
    if (d->kind == 1) {
        for (int k = 0; k < 256; k++)
            out[n++] = (diobj_t){ 0, (uint32_t)k, DIDFT_BUTTON | DIDFT_MAKEINSTANCE((uint32_t)k), 0 };
        return n;
    }
    if (d->kind == 2) {
        out[n++] = (diobj_t){ obj_guid_va(0), 0, DIDFT_AXIS | DIDFT_MAKEINSTANCE(0), 0 };
        out[n++] = (diobj_t){ obj_guid_va(1), 4, DIDFT_AXIS | DIDFT_MAKEINSTANCE(1), 0 };
        out[n++] = (diobj_t){ obj_guid_va(2), 8, DIDFT_AXIS | DIDFT_MAKEINSTANCE(2), 0 };
        for (int b = 0; b < 8; b++)
            out[n++] = (diobj_t){ 0, (uint32_t)(12 + b), DIDFT_BUTTON | DIDFT_MAKEINSTANCE((uint32_t)b), 0 };
        return n;
    }
    for (int a = 0; a < 8; a++)
        out[n++] = (diobj_t){ obj_guid_va(a < 7 ? a : 6), (uint32_t)(a * 4),
                              DIDFT_AXIS | DIDFT_MAKEINSTANCE((uint32_t)a), 0 };
    for (int p = 0; p < 4; p++)
        out[n++] = (diobj_t){ obj_guid_va(8), (uint32_t)(32 + p * 4), DIDFT_POV | DIDFT_MAKEINSTANCE((uint32_t)p), 0 };
    for (int b = 0; b < 32; b++)
        out[n++] = (diobj_t){ 0, (uint32_t)(48 + b), DIDFT_BUTTON | DIDFT_MAKEINSTANCE((uint32_t)b), 0 };
    return n;
}

/* ---- IDirectInputDevice7 ---- */
#define SELF()  A32(0)
static void dev_QueryInterface(void) {
    uint32_t d1 = A32(1) ? MEM32(A32(1)) : 0;
    if (d1 == 0 || d1 == IID_DEV_A || d1 == IID_DEV2_A || d1 == IID_DEV7_A) {
        MEM32(A32(2)) = SELF();
        MEM32(SELF() + 4)++;
        RET(DI_OK, 3);
    }
    if (A32(2)) MEM32(A32(2)) = 0;
    DITRACE("device::QueryInterface(%#x) -> E_NOINTERFACE\n", d1);
    RET(DIERR_NOINTERFACE, 3);
}
static void dev_AddRef(void) { MEM32(SELF() + 4)++; RET(MEM32(SELF() + 4), 1); }
static void dev_Release(void) {
    uint32_t self = SELF(), n = --MEM32(self + 4);
    if (!n) {
        didev_t *d = device_of(self);
        if (d) { pad_close(d); memset(d, 0, sizeof *d); }
        hle_com_free(self);
    }
    RET(n, 1);
}
static void dev_GetCapabilities(void) {
    didev_t *d = device_of(SELF());
    uint32_t p = A32(1);
    if (!d) RET(DIERR_NOTINITIALIZED, 2);
    if (!p || MEM32(p) < 24) RET(DIERR_INVALIDPARAM, 2);
    uint32_t size = MEM32(p);
    if (size > 64) size = 64;
    memset(hle_ptr(p + 4), 0, size - 4);
    MEM32(p + 4) = DIDC_ATTACHED | DIDC_POLLEDDEVICE;
    MEM32(p + 8) = d->kind == 1 ? DIDEVTYPE_KEYBOARD : d->kind == 2 ? DIDEVTYPE_MOUSE : DIDEVTYPE_JOYSTICK;
    if (d->kind == 1) { MEM32(p + 12) = 0; MEM32(p + 16) = 256; MEM32(p + 20) = 0; }
    else if (d->kind == 2) { MEM32(p + 12) = 3; MEM32(p + 16) = 8; MEM32(p + 20) = 0; }
    else {
        SDL_Joystick *j = pad_get(d);
        MEM32(p + 12) = (uint32_t)(j ? SDL_JoystickNumAxes(j) : 8);
        MEM32(p + 16) = (uint32_t)(j ? SDL_JoystickNumButtons(j) : 32);
        MEM32(p + 20) = (uint32_t)(j ? SDL_JoystickNumHats(j) : 4);
    }
    RET(DI_OK, 2);
}
static void dev_EnumObjects(void) {
    didev_t *d = device_of(SELF());
    uint32_t cb = A32(1), ref = A32(2), flags = A32(3);
    if (!d) RET(DIERR_NOTINITIALIZED, 4);
    if (!cb) RET(DIERR_INVALIDPARAM, 4);
    diobj_t def[DI_MAX_OBJ];
    uint32_t n = d->nobj ? d->nobj : default_objects(d, def);
    const diobj_t *objs = d->nobj ? d->obj : def;
    uint32_t mark = hle_scratch_mark();
    for (uint32_t i = 0; i < n; i++) {
        if (flags && !(objs[i].type & (flags & ~0x80000000u))) continue;   /* DIDFT_ALL is 0 */
        uint32_t doi = hle_scratch(316);
        memset(hle_ptr(doi), 0, 316);
        MEM32(doi) = 316;
        int w = 0, src = obj_canon(d, &objs[i], &w);
        uint32_t g1 = objs[i].pguid ? MEM32(objs[i].pguid) : 0;
        int gi = -1;
        if (src >= 0 && d->kind == 1) gi = 9;             /* GUID_Key */
        else if (src >= 0 && d->kind == 2 && (objs[i].type & DIDFT_BUTTON)) gi = 7;
        else if (src >= 0 && d->kind == 3) {
            if (objs[i].type & DIDFT_POV) gi = 8;
            else if (objs[i].type & DIDFT_BUTTON) gi = 7;
            else gi = (g1 == D1_SLIDER) ? 6 : (src / 4) < 6 ? src / 4 : 6;
        }
        if (gi >= 0) MEM32(doi + 4) = obj_guid_va(gi);
        (void)g1;
        MEM32(doi + 20) = objs[i].ofs;
        MEM32(doi + 24) = objs[i].type;
        MEM32(doi + 28) = objs[i].flags;
        char *name = (char *)hle_ptr(doi + 32);
        if (d->kind == 1) snprintf(name, 80, "Key %u", (objs[i].type >> 8) & 0xFFu);
        else if (objs[i].type & DIDFT_POV) snprintf(name, 80, "POV %u", (objs[i].type >> 8) & 0xFFu);
        else if (objs[i].type & DIDFT_BUTTON) snprintf(name, 80, "Button %u", (objs[i].type >> 8) & 0xFFu);
        else snprintf(name, 80, "Axis %u", (objs[i].type >> 8) & 0xFFu);
        MEM16(doi + 300) = 1;                             /* wCollectionNumber */
        MEM16(doi + 306) = 1;                             /* wUsagePage (generic desktop) */
        MEM16(doi + 308) = (uint16_t)((objs[i].type & DIDFT_POV) ? 0x39 : (objs[i].type & DIDFT_BUTTON) ? 0x09 : 0x30);
        uint32_t a[2] = { doi, ref };
        uint32_t more = hle_call_guest(cb, 2, a);
        if (!more) break;
    }
    hle_scratch_release(mark);
    RET(DI_OK, 4);
}
static int prop_axis(const didev_t *d, uint32_t obj, uint32_t how) {
    if (how == DIPH_DEVICE) return 0;
    if (how == DIPH_BYID) {
        uint32_t t = obj & 0xFFu, inst = (obj >> 8) & 0xFFFFu;
        if (t & DIDFT_AXIS) return (int)(inst & 7u);
        return -1;
    }
    if (how == DIPH_BYOFFSET) {
        for (uint32_t i = 0; i < d->nobj; i++)
            if (d->obj[i].ofs == obj) {
                int w = 0, src = obj_canon(d, &d->obj[i], &w);
                if (src >= 0 && w == 4 && !(d->obj[i].type & DIDFT_POV)) return src / 4;
            }
        return (int)(obj / 4);
    }
    return -1;
}
static void dev_GetProperty(void) {
    didev_t *d = device_of(SELF());
    uint32_t prop = A32(1), p = A32(2);
    if (!d) RET(DIERR_NOTINITIALIZED, 3);
    if (!p || MEM32(p) < 16) RET(DIERR_INVALIDPARAM, 3);
    uint32_t dwObj = MEM32(p + 8), how = MEM32(p + 12);
    switch (prop) {
    case DIPROP_BUFFERSIZE:
        if (MEM32(p) < 20) RET(DIERR_INVALIDPARAM, 3);
        MEM32(p + 16) = d->buffer_size;
        RET(DI_OK, 3);
    case DIPROP_AXISMODE:
        if (MEM32(p) < 20) RET(DIERR_INVALIDPARAM, 3);
        MEM32(p + 16) = d->relative ? 1u : 0u;
        RET(DI_OK, 3);
    case DIPROP_GRANULARITY:
        if (MEM32(p) < 20) RET(DIERR_INVALIDPARAM, 3);
        MEM32(p + 16) = 1;
        RET(DI_OK, 3);
    case DIPROP_RANGE: {
        if (MEM32(p) < 24) RET(DIERR_INVALIDPARAM, 3);
        int a = prop_axis(d, dwObj, how);
        if (a < 0 || a > 7) RET(DIERR_INVALIDPARAM, 3);
        MEM32(p + 16) = (uint32_t)d->range_min[a];
        MEM32(p + 20) = (uint32_t)d->range_max[a];
        RET(DI_OK, 3);
    }
    case DIPROP_DEADZONE: case DIPROP_SATURATION: {
        if (MEM32(p) < 20) RET(DIERR_INVALIDPARAM, 3);
        int a = prop_axis(d, dwObj, how);
        if (a < 0 || a > 7) a = 0;
        MEM32(p + 16) = (uint32_t)(prop == DIPROP_DEADZONE ? d->deadzone[a] : d->saturation[a]);
        RET(DI_OK, 3);
    }
    default:
        RET(DIERR_UNSUPPORTED, 3);
    }
}
static void dev_SetProperty(void) {
    didev_t *d = device_of(SELF());
    uint32_t prop = A32(1), p = A32(2);
    if (!d) RET(DIERR_NOTINITIALIZED, 3);
    if (!p || MEM32(p) < 16) RET(DIERR_INVALIDPARAM, 3);
    uint32_t dwObj = MEM32(p + 8), how = MEM32(p + 12);
    switch (prop) {
    case DIPROP_BUFFERSIZE:
        if (MEM32(p) < 20) RET(DIERR_INVALIDPARAM, 3);
        d->buffer_size = MEM32(p + 16);
        DITRACE("device::SetProperty(BUFFERSIZE=%u)\n", d->buffer_size);
        RET(DI_OK, 3);
    case DIPROP_AXISMODE:
        if (MEM32(p) < 20) RET(DIERR_INVALIDPARAM, 3);
        d->relative = MEM32(p + 16) != 0;
        RET(DI_OK, 3);
    case DIPROP_GRANULARITY:
        RET(DI_OK, 3);
    case DIPROP_RANGE: {
        if (MEM32(p) < 24) RET(DIERR_INVALIDPARAM, 3);
        int a = prop_axis(d, dwObj, how);
        DITRACE("device::SetProperty(RANGE [%d..%d] how=%u obj=%#x axis=%d)\n",
                (int32_t)MEM32(p + 16), (int32_t)MEM32(p + 20), how, dwObj, a);
        if (a < 0) RET(DIERR_INVALIDPARAM, 3);
        if (how == DIPH_DEVICE) for (int i = 0; i < 8; i++) d->range_min[i] = (int32_t)MEM32(p + 16), d->range_max[i] = (int32_t)MEM32(p + 20);
        else d->range_min[a] = (int32_t)MEM32(p + 16), d->range_max[a] = (int32_t)MEM32(p + 20);
        RET(DI_OK, 3);
    }
    case DIPROP_DEADZONE: case DIPROP_SATURATION: {
        if (MEM32(p) < 20) RET(DIERR_INVALIDPARAM, 3);
        uint32_t v = MEM32(p + 16);
        if (v > 10000) RET(DIERR_INVALIDPARAM, 3);
        int a = prop_axis(d, dwObj, how);
        if (a < 0) a = 0;
        DITRACE("device::SetProperty(%s=%u axis=%d)\n", prop == DIPROP_DEADZONE ? "DEADZONE" : "SATURATION", v, a);
        if (how == DIPH_DEVICE) for (int i = 0; i < 8; i++) (prop == DIPROP_DEADZONE ? d->deadzone : d->saturation)[i] = (int32_t)v;
        else (prop == DIPROP_DEADZONE ? d->deadzone : d->saturation)[a] = (int32_t)v;
        RET(DI_OK, 3);
    }
    default:
        DITRACE("device::SetProperty(%u): unsupported\n", prop);
        RET(DIERR_UNSUPPORTED, 3);
    }
}
static void dev_Acquire(void) {
    didev_t *d = device_of(SELF());
    if (!d) RET(DIERR_NOTINITIALIZED, 1);
    if (!sdl_ready()) RET(DIERR_UNSUPPORTED, 1);
    if (d->kind == 3 && !pad_get(d)) RET(DIERR_UNPLUGGED, 1);
    d->acquired = 1;
    d->prev_valid = 0;                                     /* no stale transitions across an acquire */
    DITRACE("device::Acquire(%s%d) -> OK\n", kind_name(d->kind), d->ordinal);
    RET(DI_OK, 1);
}
static void dev_Unacquire(void) {
    didev_t *d = device_of(SELF());
    if (!d) RET(DIERR_NOTINITIALIZED, 1);
    d->acquired = 0;
    d->ring_head = d->ring_count = 0;
    DITRACE("device::Unacquire(%s%d)\n", kind_name(d->kind), d->ordinal);
    RET(DI_OK, 1);
}
static void dev_GetDeviceState(void) {
    didev_t *d = device_of(SELF());
    uint32_t cb = A32(1), out = A32(2);
    if (!d) RET(DIERR_NOTINITIALIZED, 3);
    if (!out || !d->datasize || cb != d->datasize) {
        DITRACE("device::GetDeviceState(cb=%u, want %u) -> INVALIDPARAM\n", cb, d->datasize);
        RET(DIERR_INVALIDPARAM, 3);
    }
    if (!d->acquired) RET(DIERR_NOTACQUIRED, 3);
    poll_device(d);
    uint8_t c[DI_CANON];
    if (!build_canon(d, c)) RET(DIERR_UNPLUGGED, 3);
    encode_state(d, c, (uint8_t *)hle_ptr(out));
    if (g_trace_calls) {
        static int n;
        if ((n++ % 200) == 0 || c[0x26] || c[0x28] || c[0x2E] || c[0xC8]) {   /* up/down/c/pressed */
            int pressed = 0;
            for (int k = 0; k < 256; k++) if (c[k]) { pressed = 1; break; }
            fprintf(stderr, "[dikey] GetDeviceState kind=%d cb=%u pressed=%d up=%02x down=%02x c=%02x p=%02x\n",
                    d->kind, cb, pressed, c[0xC8], c[0xD0], c[0x2E], c[0x19]);
        }
    }
    if (trace_state() && d->kind == 3) {
        uint8_t *o = (uint8_t *)hle_ptr(out);
        static uint32_t last_sig[DI_MAX_DEV];
        uint32_t sig = 0;
        for (int i = 48; i < 80; i++) sig = sig * 31 + o[i];
        sig = sig * 31 + (uint32_t)(int32_t)(o[0] | o[1] << 8 | o[2] << 16 | o[3] << 24);
        int slot = d->ordinal;
        if (slot >= 0 && slot < DI_MAX_DEV && sig != last_sig[slot]) {
            last_sig[slot] = sig;
            fprintf(stderr, "[dinput-state] GetDeviceState %s%d raw=%02x%02x%02x%02x.. buttons=%02x%02x%02x%02x%02x%02x%02x%02x\n",
                    kind_name(d->kind), d->ordinal, o[0], o[1], o[2], o[3],
                    o[48], o[49], o[50], o[51], o[52], o[53], o[54], o[55]);
        }
    }
    RET(DI_OK, 3);
}
static void dev_GetDeviceData(void) {
    didev_t *d = device_of(SELF());
    uint32_t cb = A32(1), arr = A32(2), pinout = A32(3), flags = A32(4);
    if (!d) RET(DIERR_NOTINITIALIZED, 5);
    if (!pinout || !arr || cb != 16) RET(DIERR_INVALIDPARAM, 5);
    if (!d->buffer_size) RET(DIERR_NOTBUFFERED, 5);
    if (!d->acquired) RET(DIERR_NOTACQUIRED, 5);
    poll_device(d);
    uint32_t want = MEM32(pinout), got = 0;
    while (got < want && d->ring_count) {
        uint32_t at = d->ring_head;
        uint32_t p = arr + got * 16;
        MEM32(p + 0) = d->ring[at].ofs;
        MEM32(p + 4) = d->ring[at].data;
        MEM32(p + 8) = d->ring[at].stamp;
        MEM32(p + 12) = d->ring[at].seq;
        got++;
        if (!(flags & DIGDD_PEEK)) { d->ring_head = (d->ring_head + 1) % DI_RING; d->ring_count--; }
    }
    MEM32(pinout) = got;
    int overflow = d->overflow;
    d->overflow = 0;
    DITRACE("device::GetDeviceData(%u) -> %u events%s\n", want, got, overflow ? " (overflow)" : "");
    RET(overflow ? DI_BUFFEROVERFLOW : DI_OK, 5);
}
static void dev_SetDataFormat(void) {
    didev_t *d = device_of(SELF());
    uint32_t df = A32(1);
    if (!d) RET(DIERR_NOTINITIALIZED, 2);
    if (!df) RET(DIERR_INVALIDPARAM, 2);
    uint32_t dwSize = MEM32(df), objSize = MEM32(df + 4), flags = MEM32(df + 8);
    uint32_t dataSize = MEM32(df + 12), n = MEM32(df + 16), rg = MEM32(df + 20);
    if (dwSize < 24 || objSize < 16 || dataSize == 0 || dataSize > DI_CANON || !rg || n > DI_MAX_OBJ)
        RET(DIERR_INVALIDPARAM, 2);
    d->datasize = dataSize;
    d->format_flags = flags;
    d->nobj = n;
    d->prev_valid = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t o = rg + i * objSize;
        d->obj[i].pguid = MEM32(o + 0);
        d->obj[i].ofs = MEM32(o + 4);
        d->obj[i].type = MEM32(o + 8);
        d->obj[i].flags = MEM32(o + 12);
    }
    if (flags & DIDF_RELAXIS) d->relative = 1;
    else if (flags & DIDF_ABSAXIS) d->relative = 0;
    DITRACE("device::SetDataFormat(%s%d) size=%u objs=%u flags=%#x\n",
            kind_name(d->kind), d->ordinal, dataSize, n, flags);
    RET(DI_OK, 2);
}
static void dev_SetEventNotification(void) {
    /* NFS5's input thread does not poll GetDeviceState: it waits on this event
     * and only then reads the device. Windows DirectInput signals it when the
     * device state changes. We keep the handle and signal it from
     * hle_dinput_pump(), which the host wait paths call while the game thread
     * is blocked (this was why keyboard/controller input never reached the
     * game: with the event never signalled the thread never polled). */
    didev_t *d = device_of(SELF());
    if (!d) RET(DIERR_NOTINITIALIZED, 2);
    d->notif = A32(1);
    DITRACE("device::SetEventNotification(%s%d, event=%#x)\n", kind_name(d->kind), d->ordinal, d->notif);
    RET(DI_OK, 2);
}
/* Called from the host wait paths (kernel32 hle_wait / user32 message wait) on
 * the thread that is about to block: pump each acquired device and signal its
 * notification event when its state changed, so an event-driven game polls. */
int hle_dinput_mouse_active(void) { return g_minject; }

void hle_dinput_pump(void) {
    nfs5_pad_poll();
    for (int i = 0; i < DI_MAX_DEV; i++) {
        didev_t *d = &g_dev[i];
        if (!d->used || !d->acquired || !d->datasize) continue;
        if (!poll_device(d)) continue;
        if (d->changed && d->notif) hle_set_event(d->notif);
    }
}
static void dev_SetCooperativeLevel(void) {
    didev_t *d = device_of(SELF());
    if (!d) RET(DIERR_NOTINITIALIZED, 3);
    d->hwnd = A32(1);
    d->coop = A32(2);
    DITRACE("device::SetCooperativeLevel(%s%d, hwnd=%#x, flags=%#x)\n",
            kind_name(d->kind), d->ordinal, d->hwnd, d->coop);
    RET(DI_OK, 3);
}
static void dev_GetObjectInfo(void) {
    didev_t *d = device_of(SELF());
    uint32_t p = A32(1), obj = A32(2), how = A32(3);
    if (!d) RET(DIERR_NOTINITIALIZED, 4);
    if (!p || MEM32(p) < 316) RET(DIERR_INVALIDPARAM, 4);
    diobj_t def[DI_MAX_OBJ];
    uint32_t n = d->nobj ? d->nobj : default_objects(d, def);
    const diobj_t *objs = d->nobj ? d->obj : def;
    const diobj_t *found = NULL;
    for (uint32_t i = 0; i < n; i++) {
        if (how == DIPH_BYOFFSET && objs[i].ofs == obj) { found = &objs[i]; break; }
        if (how == DIPH_BYID && (objs[i].type & 0xFFFFFF00u) == (obj & 0xFFFFFF00u)) { found = &objs[i]; break; }
        if (how == DIPH_DEVICE) { found = &objs[0]; break; }
    }
    if (!found) RET(DIERR_OBJECTNOTFOUND, 4);
    memset(hle_ptr(p), 0, MEM32(p));
    MEM32(p) = 316;
    MEM32(p + 20) = found->ofs;
    MEM32(p + 24) = found->type;
    MEM32(p + 28) = found->flags;
    char *name = (char *)hle_ptr(p + 32);
    snprintf(name, 80, "%s %u", (found->type & DIDFT_POV) ? "POV" : (found->type & DIDFT_BUTTON) ? "Button" : "Axis",
             (found->type >> 8) & 0xFFu);
    RET(DI_OK, 4);
}
static void dev_GetDeviceInfo(void) {
    didev_t *d = device_of(SELF());
    uint32_t p = A32(1);
    if (!d) RET(DIERR_NOTINITIALIZED, 2);
    if (!p || MEM32(p) < 560) RET(DIERR_INVALIDPARAM, 2);
    uint32_t size = MEM32(p);
    memset(hle_ptr(p + 4), 0, size - 4);
    memcpy(hle_ptr(p + 4), d->guid, 16);
    memcpy(hle_ptr(p + 20), d->guid, 16);                 /* product GUID: ours */
    MEM32(p + 36) = d->kind == 1 ? DIDEVTYPE_KEYBOARD : d->kind == 2 ? DIDEVTYPE_MOUSE : DIDEVTYPE_JOYSTICK;
    const char *name = d->kind == 1 ? "SDL2 keyboard" : d->kind == 2 ? "SDL2 mouse" : NULL;
    char padname[260];
    if (d->kind == 3) {
        const char *jn = d->ordinal >= 0 ? SDL_JoystickNameForIndex(d->ordinal) : NULL;
        snprintf(padname, sizeof padname, "%s", jn ? jn : "SDL2 joystick");
        name = padname;
    }
    snprintf((char *)hle_ptr(p + 40), 260, "%s", name);
    snprintf((char *)hle_ptr(p + 300), 260, "%s", name);
    if (size >= 580) { MEM16(p + 576) = 1; MEM16(p + 578) = 1; }
    RET(DI_OK, 2);
}
static void dev_RunControlPanel(void) { RET(DI_OK, 3); }
static void dev_Initialize(void) { RET(DI_OK, 4); }
/* force feedback: a device with no motors */
static void dev_CreateEffect(void) { if (A32(2)) MEM32(A32(2)) = 0; RET(DIERR_UNSUPPORTED, 5); }
static void dev_EnumEffects(void) { RET(DI_OK, 4); }
static void dev_GetEffectInfo(void) { RET(DIERR_UNSUPPORTED, 3); }
static void dev_GetForceFeedbackState(void) { if (A32(1)) MEM32(A32(1)) = 0; RET(DIERR_UNSUPPORTED, 2); }
static void dev_SendForceFeedbackCommand(void) { RET(DIERR_UNSUPPORTED, 2); }
static void dev_EnumCreatedEffectObjects(void) { RET(DI_OK, 4); }
static void dev_Escape(void) { RET(DIERR_UNSUPPORTED, 2); }
static void dev_Poll(void) {
    didev_t *d = device_of(SELF());
    if (!d) RET(DIERR_NOTINITIALIZED, 1);
    if (!d->acquired) RET(DIERR_NOTACQUIRED, 1);
    if (d->kind == 3 && !pad_get(d)) RET(DIERR_UNPLUGGED, 1);
    poll_device(d);
    RET(DI_OK, 1);
}
static void dev_SendDeviceData(void) { RET(DIERR_UNSUPPORTED, 5); }
static void dev_EnumEffectsInFile(void) { RET(DI_OK, 5); }
static void dev_WriteEffectToFile(void) { RET(DIERR_UNSUPPORTED, 5); }

/* ---- IDirectInput7 ---- */
static void di_QueryInterface(void) {
    uint32_t d1 = A32(1) ? MEM32(A32(1)) : 0;
    if (d1 == 0 || d1 == IID_DI_A || d1 == IID_DI2_A || d1 == IID_DI7_A) {
        MEM32(A32(2)) = g_di;
        MEM32(g_di + 4)++;
        RET(DI_OK, 3);
    }
    if (A32(2)) MEM32(A32(2)) = 0;
    DITRACE("DirectInput::QueryInterface(%#x) -> E_NOINTERFACE\n", d1);
    RET(DIERR_NOINTERFACE, 3);
}
static void di_AddRef(void) { MEM32(g_di + 4)++; RET(MEM32(g_di + 4), 1); }
static void di_Release(void) { uint32_t n = MEM32(g_di + 4) ? --MEM32(g_di + 4) : 0; RET(n, 1); }
static uint32_t create_device_from_guid(uint32_t gp, uint32_t out) {
    if (!gp || !out) return DIERR_INVALIDPARAM;
    uint8_t g[16];
    memcpy(g, hle_ptr(gp), 16);
    int ordinal = 0;
    int kind = guid_to_kind(g, &ordinal);
    if (!kind) {
        /* not one of the system GUIDs: if a pad with that ordinal exists, take
         * it; otherwise the device is not there */
        DITRACE("CreateDevice(guid %08x %08x ...): not ours\n", G1(g), G1(g + 4));
        return DIERR_DEVICENOTREG;
    }
    if (kind == 3) {
        if (ordinal < 0 || ordinal >= pad_count()) return DIERR_DEVICENOTREG;
        uint8_t canonical[16];
        guid_of_kind(canonical, 3, ordinal);
        memcpy(g, canonical, 16);
    }
    if (getenv("NFS5_NO_DINPUT_KEYBOARD") && kind == 1) {
        /* Experiment: without a DirectInput keyboard the game may fall back to
         * window messages for input (the path the port already feeds). */
        DITRACE("CreateDevice(keyboard0): suppressed by NFS5_NO_DINPUT_KEYBOARD\n");
        return DIERR_DEVICENOTREG;
    }
    uint32_t self = device_new(g, kind, ordinal);
    if (!self) return 0x8007000Eu;                        /* E_OUTOFMEMORY */
    MEM32(out) = self;
    DITRACE("CreateDevice(%s%d) -> object %#x\n", kind_name(kind), ordinal, self);
    return DI_OK;
}
static void di_CreateDevice(void) {                      /* (this, guid*, &dev, outer) */
    if (A32(2)) MEM32(A32(2)) = 0;
    if (A32(3)) RET(DIERR_NOINTERFACE, 4);               /* aggregation not supported */
    uint32_t hr = create_device_from_guid(A32(1), A32(2));
    RET(hr, 4);
}
static void di_CreateDeviceEx(void) {                    /* (this, guid*, riid, &dev, outer) */
    uint32_t out = A32(3);
    if (out) MEM32(out) = 0;
    if (A32(4)) RET(DIERR_NOINTERFACE, 5);
    uint32_t d1 = A32(2) ? MEM32(A32(2)) : 0;
    if (!(d1 == 0 || d1 == IID_DEV_A || d1 == IID_DEV2_A || d1 == IID_DEV7_A)) RET(DIERR_NOINTERFACE, 5);
    uint32_t hr = create_device_from_guid(A32(1), out);
    RET(hr, 5);
}
static uint32_t enumerate_kind_cb(uint32_t kind, int ordinal, uint32_t cb, uint32_t ref, uint8_t *g) {
    uint32_t mark = hle_scratch_mark();
    uint32_t di = hle_scratch(580);
    memset(hle_ptr(di), 0, 580);
    MEM32(di) = 580;
    memcpy(hle_ptr(di + 4), g, 16);
    memcpy(hle_ptr(di + 20), g, 16);
    MEM32(di + 36) = kind == 1 ? DIDEVTYPE_KEYBOARD : kind == 2 ? DIDEVTYPE_MOUSE : DIDEVTYPE_JOYSTICK;
    const char *name = kind == 1 ? "SDL2 keyboard" : kind == 2 ? "SDL2 mouse" : NULL;
    char padname[260];
    if (kind == 3) {
        const char *jn = SDL_JoystickNameForIndex(ordinal);
        snprintf(padname, sizeof padname, "%s", jn ? jn : "SDL2 joystick");
        name = padname;
    }
    snprintf((char *)hle_ptr(di + 40), 260, "%s", name);
    snprintf((char *)hle_ptr(di + 300), 260, "%s", name);
    MEM16(di + 576) = 1;
    MEM16(di + 578) = 1;
    uint32_t a[2] = { di, ref };
    uint32_t r = hle_call_guest(cb, 2, a);
    hle_scratch_release(mark);
    return r;
}
static void di_EnumDevices(void) {                       /* (this, devtype, cb, ref, flags) */
    uint32_t type = A32(1), cb = A32(2), ref = A32(3);
    if (!cb) RET(DIERR_INVALIDPARAM, 5);
    sdl_ready();
    int want = (int)(type & 0xFFu);
    int all = want == 0 || want == DIDEVTYPE_DEVICE;
    uint8_t g[16];
    if (all || want == DIDEVTYPE_KEYBOARD) {
        guid_of_kind(g, 1, 0);
        if (!enumerate_kind_cb(1, 0, cb, ref, g)) { RET(DI_OK, 5); }
    }
    if (all || want == DIDEVTYPE_MOUSE) {
        guid_of_kind(g, 2, 0);
        if (!enumerate_kind_cb(2, 0, cb, ref, g)) { RET(DI_OK, 5); }
    }
    if (all || want == DIDEVTYPE_JOYSTICK) {
        int n = pad_count();
        for (int i = 0; i < n; i++) {
            guid_of_kind(g, 3, i);
            if (!enumerate_kind_cb(3, i, cb, ref, g)) { RET(DI_OK, 5); }
        }
    }
    RET(DI_OK, 5);
}
static void di_GetDeviceStatus(void) {                   /* (this, guid*) */
    if (!A32(1)) RET(DIERR_INVALIDPARAM, 2);
    uint8_t g[16];
    memcpy(g, hle_ptr(A32(1)), 16);
    int ordinal = 0, kind = guid_to_kind(g, &ordinal);
    if (!kind) RET(DIERR_NOTFOUND, 2);
    if (kind == 3 && (ordinal < 0 || ordinal >= pad_count())) RET(DI_NOTATTACHED, 2);
    RET(DI_OK, 2);
}
static void di_RunControlPanel(void) { RET(DI_OK, 3); }
static void di_Initialize(void) { RET(DI_OK, 3); }
static void di_FindDevice(void) {                        /* (this, guid*, name, &guid) */
    if (!A32(1) || !A32(2)) RET(DIERR_INVALIDPARAM, 4);
    const char *name = ASTR(2);
    uint8_t g[16];
    memcpy(g, hle_ptr(A32(1)), 16);
    int ordinal = 0, kind = guid_to_kind(g, &ordinal);
    if (!kind) RET(DIERR_DEVICENOTREG, 4);
    const char *have = kind == 1 ? "SDL2 keyboard" : kind == 2 ? "SDL2 mouse" : "SDL2 joystick";
    if (name && name[0] && strcasecmp(name, have)) RET(DIERR_NOTFOUND, 4);
    if (kind == 3 && (ordinal < 0 || ordinal >= pad_count())) RET(DIERR_NOTFOUND, 4);
    if (A32(3)) { guid_of_kind(g, kind, ordinal); memcpy(hle_ptr(A32(3)), g, 16); }
    RET(DI_OK, 4);
}

/* ---- the vtable + registration ---- */
static void vtables(void) {
    if (g_vt_di) return;
    static const win32hle_shim di_methods[] = {
        { "DI::QueryInterface", di_QueryInterface }, { "DI::AddRef", di_AddRef }, { "DI::Release", di_Release },
        { "DI::CreateDevice", di_CreateDevice }, { "DI::EnumDevices", di_EnumDevices },
        { "DI::GetDeviceStatus", di_GetDeviceStatus }, { "DI::RunControlPanel", di_RunControlPanel },
        { "DI::Initialize", di_Initialize }, { "DI::FindDevice", di_FindDevice },
        { "DI::CreateDeviceEx", di_CreateDeviceEx },
        { 0, 0 }
    };
    static const win32hle_shim dev_methods[] = {
        { "DID::QueryInterface", dev_QueryInterface }, { "DID::AddRef", dev_AddRef }, { "DID::Release", dev_Release },
        { "DID::GetCapabilities", dev_GetCapabilities }, { "DID::EnumObjects", dev_EnumObjects },
        { "DID::GetProperty", dev_GetProperty }, { "DID::SetProperty", dev_SetProperty },
        { "DID::Acquire", dev_Acquire }, { "DID::Unacquire", dev_Unacquire },
        { "DID::GetDeviceState", dev_GetDeviceState }, { "DID::GetDeviceData", dev_GetDeviceData },
        { "DID::SetDataFormat", dev_SetDataFormat }, { "DID::SetEventNotification", dev_SetEventNotification },
        { "DID::SetCooperativeLevel", dev_SetCooperativeLevel }, { "DID::GetObjectInfo", dev_GetObjectInfo },
        { "DID::GetDeviceInfo", dev_GetDeviceInfo }, { "DID::RunControlPanel", dev_RunControlPanel },
        { "DID::Initialize", dev_Initialize },
        { "DID::CreateEffect", dev_CreateEffect }, { "DID::EnumEffects", dev_EnumEffects },
        { "DID::GetEffectInfo", dev_GetEffectInfo }, { "DID::GetForceFeedbackState", dev_GetForceFeedbackState },
        { "DID::SendForceFeedbackCommand", dev_SendForceFeedbackCommand },
        { "DID::EnumCreatedEffectObjects", dev_EnumCreatedEffectObjects }, { "DID::Escape", dev_Escape },
        { "DID::Poll", dev_Poll }, { "DID::SendDeviceData", dev_SendDeviceData },
        { "DID::EnumEffectsInFile", dev_EnumEffectsInFile }, { "DID::WriteEffectToFile", dev_WriteEffectToFile },
        { 0, 0 }
    };
    g_vt_di = hle_com_vtable(di_methods);
    g_vt_dev = hle_com_vtable(dev_methods);
    g_di = hle_com_new(g_vt_di, 8);
    MEM32(g_di + 4) = 0;                                  /* no owner until DirectInputCreate */
}
static void di_create(void) {                            /* DirectInputCreateA(hinst, version, &out, outer) */
    uint32_t version = A32(1), out = A32(2);
    if (out) MEM32(out) = 0;
    if (!out) RET(DIERR_INVALIDPARAM, 4);
    if (A32(3)) RET(DIERR_NOINTERFACE, 4);               /* no aggregation, as DirectInput */
    if (version < 0x0300) RET(DIERR_OLDDIRECTINPUTVERSION, 4);
    if (!sdl_ready()) RET(DIERR_UNSUPPORTED, 4);
    vtables();
    MEM32(g_di + 4)++;
    MEM32(out) = g_di;
    DITRACE("DirectInputCreateA(version=%#x) -> object %#x\n", version, g_di);
    RET(DI_OK, 4);
}
static void di_create_ex(void) {                         /* DirectInputCreateEx(hinst, version, riid, &out, outer) */
    uint32_t version = A32(1), out = A32(3);
    if (out) MEM32(out) = 0;
    if (!out) RET(DIERR_INVALIDPARAM, 5);
    if (A32(4)) RET(DIERR_NOINTERFACE, 5);
    uint32_t d1 = A32(2) ? MEM32(A32(2)) : 0;
    if (!(d1 == 0 || d1 == IID_DI_A || d1 == IID_DI2_A || d1 == IID_DI7_A)) RET(DIERR_NOINTERFACE, 5);
    if (version < 0x0300) RET(DIERR_OLDDIRECTINPUTVERSION, 5);
    if (!sdl_ready()) RET(DIERR_UNSUPPORTED, 5);
    vtables();
    MEM32(g_di + 4)++;
    MEM32(out) = g_di;
    DITRACE("DirectInputCreateEx(version=%#x) -> object %#x\n", version, g_di);
    RET(DI_OK, 5);
}

const win32hle_shim win32hle_dinput[] = {
    { "DirectInputCreateA", di_create },
    { "DirectInputCreate",  di_create },
    { "DirectInputCreateEx", di_create_ex },
    { 0, 0 }
};

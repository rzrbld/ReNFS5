/* padkeys.c - gamepad -> keyboard bridge for NFS5.
 *
 * Why this exists: the game's in-game "controller" path is fed by a device
 * layer that this port does not fully reach yet (see docs/KNOWN_ISSUES.md);
 * the keyboard/window-message path, however, works for all front-end screens
 * and the pause menu. On a handheld the user has no keyboard, so this bridge
 * turns SDL gamepad state into the same keyboard events the port already
 * handles: menus/pause/options become fully pad-operable today, and the mapping
 * is ready for in-race controls as soon as the race input consumer is wired.
 *
 * Enabled with NFS5_PAD_KEYS=1 (the launcher's env sets it on handheld).
 * Headless testing without hardware:
 *   NFS5_PAD_KEYS=1 NFS5_VIRTUAL_PAD=1 \
 *   NFS5_PAD_TIMELINE="1000:hat0=2,1400:hat0=0,2000:b0=1,2400:b0=0"
 * attaches one SDL virtual controller and drives its buttons from the timeline
 * (button indices follow SDL_GameController: 0=A, 1=B, 2=X, 3=Y, 4=LB, 5=RB,
 * 6=Back, 7=Start; hat values are SDL_HAT_* bits).
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "win32hle.h"

/* VK codes (user32.c keeps its own; the public header has no Windows VK macros) */
#define VK_UP_    0x26
#define VK_DOWN_  0x28
#define VK_LEFT_  0x25
#define VK_RIGHT_ 0x27
#define VK_RETURN_ 0x0D
#define VK_ESCAPE_ 0x1B
#define VK_SPACE_  0x20
#define VK_F1_    0x70
#define VK_F5_    0x74
#define VK_F7_    0x76

/* The front end is a mouse-driven UI (buttons activate on a click, not on
 * arrow keys), and a handheld has no mouse. Drive win32hle's cursor from the
 * left stick and turn (A) into a left click, so every menu is reachable with
 * the pad alone. NFS5_PAD_MOUSE=0 disables it; speed in NFS5_PAD_MOUSE_SPEED. */
#define WM_MOUSEMOVE_   0x0200
#define WM_LBUTTONDOWN_ 0x0201
#define WM_LBUTTONUP_   0x0202
extern void hle_dinput_mouse_set(int x, int y, int buttons) __attribute__((weak));
static float g_cur_x = 320.0f, g_cur_y = 240.0f;
static uint32_t g_cur_t;
static int g_prev_mouse_a;
static int g_mouse_ok = -1, g_mouse_speed = 420;
/* Automated test hook: NFS5_PAD_FAKE_SCRIPT="ms:x,y,btn;..." drives the same
 * cursor/click injection without needing a physical controller (the device
 * tests use it to verify the DirectInput mouse path end to end). */
typedef struct { uint32_t at; int x, y, btn; } fake_ev;
static fake_ev g_fake[32];
static int g_nfake, g_fake_i, g_fake_parsed;
static uint32_t g_fake_base;
static void fake_parse(void) {
    g_fake_parsed = 1;
    const char *s = getenv("NFS5_PAD_FAKE_SCRIPT");
    if (!s) return;
    char *copy = strdup(s), *p = copy;
    while (p && *p && g_nfake < 32) {
        char *semi = strchr(p, ';');
        if (semi) *semi = 0;
        unsigned at; int x, y, b;
        if (sscanf(p, "%u:%d,%d,%d", &at, &x, &y, &b) == 4) {
            if (!g_fake_base) g_fake_base = hle_ticks_ms();
            g_fake[g_nfake++] = (fake_ev){ at, x, y, b };
        }
        if (!semi) break;
        p = semi + 1;
    }
    free(copy);
}

static void pad_mouse(SDL_GameController *c) {
    if (!g_fake_parsed) fake_parse();
    if (getenv("NFS5_PAD_FAKE_SCRIPT")) {
        uint32_t now = hle_ticks_ms();
        while (g_fake_i < g_nfake && now - g_fake_base >= g_fake[g_fake_i].at) {
            g_cur_x = (float)g_fake[g_fake_i].x;
            g_cur_y = (float)g_fake[g_fake_i].y;
            if (hle_dinput_mouse_set) hle_dinput_mouse_set((int)g_cur_x, (int)g_cur_y,
                                                           g_fake[g_fake_i].btn);
            hle_input_mouse(WM_MOUSEMOVE_, (int)g_cur_x, (int)g_cur_y);
            if (g_fake[g_fake_i].btn && !g_prev_mouse_a)
                hle_input_mouse(WM_LBUTTONDOWN_, (int)g_cur_x, (int)g_cur_y);
            if (!g_fake[g_fake_i].btn && g_prev_mouse_a)
                hle_input_mouse(WM_LBUTTONUP_, (int)g_cur_x, (int)g_cur_y);
            g_prev_mouse_a = g_fake[g_fake_i].btn;
            if (getenv("NFS5_PAD_DEBUG"))
                fprintf(stderr, "[padmouse] fake t=%u x=%.0f y=%.0f btn=%d\n",
                        now, g_cur_x, g_cur_y, g_fake[g_fake_i].btn);
            g_fake_i++;
        }
        return;
    }
    if (g_mouse_ok < 0) {
        const char *v = getenv("NFS5_PAD_MOUSE");
        g_mouse_ok = v && *v && strcmp(v, "0") == 0 ? 0 : 1;
        const char *sp = getenv("NFS5_PAD_MOUSE_SPEED");
        if (sp && *sp) { int n = atoi(sp); if (n > 30) g_mouse_speed = n; }
    }
    if (!g_mouse_ok) return;
    uint32_t now = hle_ticks_ms();
    if (!g_cur_t) g_cur_t = now;
    float dt = (float)(now - g_cur_t) / 1000.0f;
    if (dt <= 0.0f) dt = 0.001f;
    if (dt > 0.1f) dt = 0.1f;
    g_cur_t = now;
    int ax = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    int ay = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
    float fx = ax > 9000 ? (float)(ax - 9000) / 23767.0f : ax < -9000 ? (float)(ax + 9000) / 23767.0f : 0.0f;
    float fy = ay > 9000 ? (float)(ay - 9000) / 23767.0f : ay < -9000 ? (float)(ay + 9000) / 23767.0f : 0.0f;
    if (fx > 1.0f) fx = 1.0f;
    if (fx < -1.0f) fx = -1.0f;
    if (fy > 1.0f) fy = 1.0f;
    if (fy < -1.0f) fy = -1.0f;
    int moved = fx != 0.0f || fy != 0.0f;
    if (moved) {
        g_cur_x += fx * (float)g_mouse_speed * dt;
        g_cur_y += fy * (float)g_mouse_speed * dt;
        if (g_cur_x < 0) g_cur_x = 0;
        if (g_cur_y < 0) g_cur_y = 0;
        if (g_cur_x > 639) g_cur_x = 639;
        if (g_cur_y > 479) g_cur_y = 479;
    }
    int a = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) != 0;
    int buttons = a ? 1 : 0;
    /* Both routes: window messages (front end widgets) and the DirectInput
     * system-mouse state (what NFS5 actually polls). */
    if (hle_dinput_mouse_set) hle_dinput_mouse_set((int)g_cur_x, (int)g_cur_y, buttons);
    if (moved || a || g_prev_mouse_a)
        hle_input_mouse(WM_MOUSEMOVE_, (int)g_cur_x, (int)g_cur_y);
    if (a && !g_prev_mouse_a)
        hle_input_mouse(WM_LBUTTONDOWN_, (int)g_cur_x, (int)g_cur_y);
    if (!a && g_prev_mouse_a)
        hle_input_mouse(WM_LBUTTONUP_, (int)g_cur_x, (int)g_cur_y);
    g_prev_mouse_a = a;
    {
        static int calls;
        if (getenv("NFS5_PAD_DEBUG") && calls < 12) {
            fprintf(stderr, "[padmouse] poll %d cur=%.0f,%.0f ax=%d ay=%d a=%d\n",
                    calls, g_cur_x, g_cur_y, ax, ay, a);
            calls++;
        }
        static int dbg = -1, last_ax, last_ay, last_btn;
        if (dbg < 0) dbg = getenv("NFS5_PAD_DEBUG") != NULL;
        if (dbg && (abs(ax - last_ax) > 6000 || abs(ay - last_ay) > 6000 || buttons != last_btn)) {
            fprintf(stderr, "[padmouse] cur=%.0f,%.0f ax=%d ay=%d btn=%d\n",
                    g_cur_x, g_cur_y, ax, ay, buttons);
            last_ax = ax; last_ay = ay; last_btn = buttons;
        }
    }
}

#define MAX_PADS 4
static SDL_GameController *g_pad[MAX_PADS];
static int g_npads, g_ready, g_enabled = -1;
static uint8_t g_prev_btn[SDL_CONTROLLER_BUTTON_MAX];
static uint8_t g_prev_hat[4];
static int g_virtual_index = -1;

/* ------------------------------------------------------------ test timeline */
typedef struct { uint32_t at; int kind; int idx; int val; } tl_ev;
static tl_ev g_tl[64];
static int g_ntl, g_tl_idx;
static int g_tl_parsed;

static void tl_parse(void) {
    g_tl_parsed = 1;
    const char *s = getenv("NFS5_PAD_TIMELINE");
    if (!s) return;
    char *copy = strdup(s), *p = copy;
    while (p && *p && g_ntl < 64) {
        while (*p == ' ') p++;
        if (!*p) break;
        char *colon = strchr(p, ':');
        if (!colon) break;
        *colon = 0;
        unsigned long at = strtoul(p, NULL, 10);
        char *item = colon + 1;
        char *comma = strchr(item, ',');
        if (comma) *comma = 0;
        int kind = -1, idx = 0, val = 0;
        if (item[0] == 'b') { kind = 0; idx = atoi(item + 1); char *eq = strchr(item, '='); if (eq) val = atoi(eq + 1); }
        else if (!strncmp(item, "hat", 3)) { kind = 1; idx = atoi(item + 3); char *eq = strchr(item, '='); if (eq) val = atoi(eq + 1); }
        else if (!strncmp(item, "ax", 2)) { kind = 2; idx = atoi(item + 2); char *eq = strchr(item, '='); if (eq) val = atoi(eq + 1); }
        if (kind >= 0 && idx >= 0) { g_tl[g_ntl].at = (uint32_t)at; g_tl[g_ntl].kind = kind; g_tl[g_ntl].idx = idx; g_tl[g_ntl].val = val; g_ntl++; }
        if (!comma) break;
        p = comma + 1;
    }
    free(copy);
}

/* -------------------------------------------------------------- key mapping */
typedef struct { int vk; uint32_t scan; } keyinfo;
static keyinfo vk_arrow(int d) {
    switch (d) {
    case 0: return (keyinfo){ VK_UP_, 0x48 };
    case 1: return (keyinfo){ VK_DOWN_, 0x50 };
    case 2: return (keyinfo){ VK_LEFT_, 0x4B };
    default: return (keyinfo){ VK_RIGHT_, 0x4D };
    }
}
static void key_edge(int prev, int now, keyinfo k) {
    if (prev && now) hle_input_key(1, (uint32_t)k.vk, k.scan);          /* repeat held keys */
    if (!prev && now) hle_input_key(1, (uint32_t)k.vk, k.scan);
    if (prev && !now) hle_input_key(0, (uint32_t)k.vk, k.scan);
}

static void apply_axis_dpad(SDL_GameController *c, int axis, int neg_d, int pos_d, int *prev_neg, int *prev_pos) {
    int v = SDL_GameControllerGetAxis(c, (SDL_GameControllerAxis)axis);
    keyinfo n = vk_arrow(neg_d), p = vk_arrow(pos_d);
    int n_on = v < -12000, p_on = v > 12000;
    key_edge(*prev_neg, n_on, n); *prev_neg = n_on;
    key_edge(*prev_pos, p_on, p); *prev_pos = p_on;
}

/* ------------------------------------------------- launcher controller map
 * configs/controller.cfg (written by the launcher's CONTROLLER MAPPING page):
 * action -> SDL GameController button. Order matches runtime/configuration.c. */
enum { ACT_THROTTLE, ACT_BRAKE, ACT_LEFT, ACT_RIGHT, ACT_HANDBRAKE, ACT_SHIFTUP, ACT_SHIFTDOWN,
       ACT_CAMERA, ACT_HORN, ACT_PAUSE, ACT_COUNT };
typedef struct { char button[ACT_COUNT][24]; } padmap_t;
extern int nfs5_padmap_load(const char *config_dir, padmap_t *m) __attribute__((weak));
extern int nfs5_button_index(const char *name) __attribute__((weak));
static int g_act[ACT_COUNT];
static int g_act_ready;
static void padmap_init(void) {
    static const int defaults[ACT_COUNT] = {
        SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
        SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
        SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X,
        SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_CONTROLLER_BUTTON_START};
    memcpy(g_act, defaults, sizeof g_act);
    g_act_ready = 1;
    const char *dir = getenv("NFS5_CONFIG");
    padmap_t m;
    if (dir && *dir && nfs5_padmap_load && nfs5_button_index && nfs5_padmap_load(dir, &m) == 1)
        for (int i = 0; i < ACT_COUNT; i++) {
            int b = nfs5_button_index(m.button[i]);
            if (b >= 0) g_act[i] = b;
        }
    fprintf(stderr, "[padkeys] controls: throttle=%d brake=%d left=%d right=%d handbrake=%d "
            "shift_up=%d shift_down=%d camera=%d horn=%d pause=%d (SDL buttons)\n",
            g_act[0], g_act[1], g_act[2], g_act[3], g_act[4], g_act[5], g_act[6], g_act[7],
            g_act[8], g_act[9]);
}
/* Test hook (no hardware buttons needed): NFS5_PAD_ACTIONS="ms:action=1|0,..."
 * with action in throttle,brake,left,right,handbrake,shift_up,shift_down
 * forces those actions on/off from that time (hle_ticks_ms). */
static int g_force[ACT_COUNT], g_force_on;
static void force_actions(void) {
    const char *s = getenv("NFS5_PAD_ACTIONS");
    if (!s) return;
    static const char *names[] = {"throttle", "brake", "left", "right", "handbrake", "shift_up", "shift_down", "camera"};
    uint32_t now = hle_ticks_ms();
    char buf[512];
    snprintf(buf, sizeof buf, "%s", s);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        unsigned at; char name[24]; int v;
        if (sscanf(tok, "%u:%23[a-z_]=%d", &at, name, &v) != 3 || now < at) continue;
        for (int i = 0; i < 8; i++)
            if (!strcmp(name, names[i])) g_force[i] = v ? 1 : -1;
    }
    g_force_on = 1;
}
static int act_down(SDL_GameController *c, int a) {
    if (g_force_on && g_force[a]) return g_force[a] > 0;
    return c && SDL_GameControllerGetButton(c, (SDL_GameControllerButton)g_act[a]) != 0;
}
/* The game's own joystick bindings on this handheld (fe_out.txt): steering on
 * axis 0, gas/brake on axis 2 (gas = low, brake = high, centre = none),
 * handbrake button 1, shift up 4, shift down 5, reset 6, camera cycle 7
 * (DIJOYSTATE indices).
 * Present exactly that layout: left stick X (or the steer buttons) steers, the
 * throttle/brake buttons or triggers work the pedal axis. */
int hle_pad_virtual_joy(int32_t ax[8], uint8_t bt[32], uint32_t pov[4]) {
    if (g_enabled <= 0 || !g_npads || !g_pad[0]) return 0;
    if (!g_act_ready) padmap_init();
    force_actions();
    SDL_GameController *c = g_pad[0];
    int sx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    if (act_down(c, ACT_LEFT)) sx = -32768;
    else if (act_down(c, ACT_RIGHT)) sx = 32767;
    int gas = act_down(c, ACT_THROTTLE) ||
              SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000;
    int brk = act_down(c, ACT_BRAKE) ||
              SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000;
    memset(ax, 0, 8 * sizeof *ax);
    ax[0] = sx;
    ax[1] = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
    ax[2] = brk ? 32767 : gas ? -32768 : 0;
    ax[3] = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    ax[4] = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY);
    memset(bt, 0, 32);
    bt[1] = (uint8_t)act_down(c, ACT_HANDBRAKE);
    bt[4] = (uint8_t)act_down(c, ACT_SHIFTUP);
    bt[5] = (uint8_t)act_down(c, ACT_SHIFTDOWN);
    bt[7] = (uint8_t)act_down(c, ACT_CAMERA);   /* i_cyclecamera = Button, 2, 7 */
    for (int p = 0; p < 4; p++) pov[p] = 0xFFFFFFFFu;
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("NFS5_PAD_DEBUG") != NULL;
    if (dbg) {
        static int last;
        int sig = (sx >> 12) * 31 + ax[2] * 7 + bt[1] + bt[4] * 2 + bt[5] * 4;
        if (sig != last) {
            last = sig;
            fprintf(stderr, "[padjoy] steer=%d pedal=%d hb=%d up=%d down=%d\n", sx, ax[2], bt[1], bt[4], bt[5]);
        }
    }
    return 1;
}

void nfs5_pad_poll(void) {
    if (g_enabled < 0) {
        const char *ek = getenv("NFS5_PAD_KEYS");
        g_enabled = ek && *ek && strcmp(ek, "0") != 0;
        if (g_enabled && getenv("NFS5_VIRTUAL_PAD") &&
            (SDL_NumJoysticks() == 0 || getenv("NFS5_PAD_FORCE_VIRTUAL"))) {
            if (getenv("NFS5_PAD_FORCE_VIRTUAL")) SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
            /* SDL2 has no virtual-gamecontroller attach API: attach a virtual
             * joystick and register a gamecontroller mapping for its GUID, so
             * the rest of this file can use the portable GameController API. */
            if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) == 0 || SDL_WasInit(SDL_INIT_JOYSTICK) != 0) {
                g_virtual_index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 15, 2);
                if (g_virtual_index >= 0) {
                    SDL_Joystick *vj = SDL_JoystickOpen(0);
                    if (vj) {
                        SDL_JoystickGUID g = SDL_JoystickGetGUID(vj);
                        char guid[64]; SDL_JoystickGetGUIDString(g, guid, sizeof guid);
                        char mapping[512];
                        snprintf(mapping, sizeof mapping,
                                 "%s,Port Test Pad,a:b0,b:b1,x:b2,y:b3,back:b6,start:b7,"
                                 "leftshoulder:b4,rightshoulder:b5,dpup:h0.1,dpdown:h0.4,"
                                 "dpleft:h0.8,dpright:h0.2,leftx:a0,lefty:a1,", guid);
                        SDL_GameControllerAddMapping(mapping);
                        SDL_JoystickClose(vj);
                    }
                }
            }
        }
        tl_parse();
    }
    if (!g_enabled) return;
    if (!g_ready) {
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0 && SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
            static int warned;
            if (!warned) { fprintf(stderr, "[padkeys] SDL gamecontroller init failed: %s\n", SDL_GetError()); warned = 1; }
            return;
        }
        g_ready = 1;
        int force_virtual = getenv("NFS5_PAD_FORCE_VIRTUAL") != NULL;
        int n = SDL_NumJoysticks();
        for (int i = 0; i < n && g_npads < MAX_PADS; i++) {
            if (force_virtual && i != g_virtual_index) continue;
            if (!SDL_IsGameController(i)) continue;
            SDL_GameController *c = SDL_GameControllerOpen(i);
            if (!c) continue;
            g_pad[g_npads++] = c;
            fprintf(stderr, "[padkeys] gamepad %d opened: %s (%s)\n", i, SDL_GameControllerName(c),
                    SDL_GameControllerGetAttached(c) ? "attached" : "detached");
        }
        if (!g_npads) { static int warned; if (!warned) { fprintf(stderr, "[padkeys] no game controllers found\n"); warned = 1; } }
    }
    if (getenv("NFS5_PAD_FAKE_SCRIPT")) { pad_mouse(NULL); return; }
    if (!g_npads) return;
    /* timeline drives the virtual controller for headless verification */
    if (g_ntl) {
        uint32_t now = hle_ticks_ms();
        while (g_tl_idx < g_ntl && now >= g_tl[g_tl_idx].at) {
            tl_ev *e = &g_tl[g_tl_idx++];
            SDL_GameController *c = g_pad[0];
            if (!c) continue;
            SDL_Joystick *vj = SDL_GameControllerGetJoystick(c);
            if (!vj) continue;
            if (e->kind == 0) SDL_JoystickSetVirtualButton(vj, (int)e->idx, (Uint8)e->val);
            else if (e->kind == 1) SDL_JoystickSetVirtualHat(vj, (int)e->idx, (Uint8)e->val);
            else SDL_JoystickSetVirtualAxis(vj, (int)e->idx, (Sint16)e->val);
        }
    }
    SDL_GameController *c = g_pad[0];
    SDL_GameControllerUpdate();
    /* hats / d-pad */
    for (int dir = 0; dir < 4; dir++) {
        SDL_GameControllerButton b = (SDL_GameControllerButton)(SDL_CONTROLLER_BUTTON_DPAD_UP + dir);
        int on = SDL_GameControllerGetButton(c, b) != 0;
        key_edge(g_prev_hat[dir], on, vk_arrow(dir));
        g_prev_hat[dir] = (uint8_t)on;
    }
    /* left stick as d-pad */
    static int stick_prev[4];
    apply_axis_dpad(c, SDL_CONTROLLER_AXIS_LEFTX, 2, 3, &stick_prev[2], &stick_prev[3]);
    apply_axis_dpad(c, SDL_CONTROLLER_AXIS_LEFTY, 0, 1, &stick_prev[0], &stick_prev[1]);
    /* analog triggers: right = throttle (Up), left = brake (Down) */
    {
        static int trig_prev[2];
        struct { SDL_GameControllerAxis a; uint32_t vk; uint32_t scan; int idx; } tg[] = {
            { SDL_CONTROLLER_AXIS_TRIGGERRIGHT, VK_UP_, 0x48, 0 },
            { SDL_CONTROLLER_AXIS_TRIGGERLEFT, VK_DOWN_, 0x50, 1 },
        };
        for (size_t i = 0; i < sizeof tg / sizeof *tg; i++) {
            int raw = SDL_GameControllerGetAxis(c, tg[i].a);
            int on = raw > 12000;
            key_edge(trig_prev[tg[i].idx], on, (keyinfo){ (int)tg[i].vk, tg[i].scan });
            trig_prev[tg[i].idx] = (uint8_t)on;
        }
    }
    /* buttons -> keyboard. Fixed menu keys (A = Enter, B = back/Escape,
     * Start = pause, Back = F1) plus the launcher's controller map for the
     * keys the race uses (camera C, handbrake Space, horn H, throttle Up,
     * brake Down when the game is configured for the keyboard). */
    if (!g_act_ready) padmap_init();
    struct { int b; uint32_t vk; uint32_t scan; } map[12];
    int nmap = 0;
    map[nmap++] = (typeof(map[0])){ SDL_CONTROLLER_BUTTON_A, VK_RETURN_, 0x1C };
    map[nmap++] = (typeof(map[0])){ SDL_CONTROLLER_BUTTON_B, VK_ESCAPE_, 0x01 };
    map[nmap++] = (typeof(map[0])){ g_act[ACT_PAUSE], VK_ESCAPE_, 0x01 };
    map[nmap++] = (typeof(map[0])){ SDL_CONTROLLER_BUTTON_BACK, VK_F1_, 0x3B };
    map[nmap++] = (typeof(map[0])){ g_act[ACT_CAMERA], 'C', 0x2E };
    map[nmap++] = (typeof(map[0])){ g_act[ACT_HANDBRAKE], VK_SPACE_, 0x39 };
    map[nmap++] = (typeof(map[0])){ g_act[ACT_HORN], 'H', 0x23 };
    if (g_act[ACT_THROTTLE] < SDL_CONTROLLER_BUTTON_DPAD_UP)
        map[nmap++] = (typeof(map[0])){ g_act[ACT_THROTTLE], VK_UP_, 0x48 };
    if (g_act[ACT_BRAKE] < SDL_CONTROLLER_BUTTON_DPAD_UP)
        map[nmap++] = (typeof(map[0])){ g_act[ACT_BRAKE], VK_DOWN_, 0x50 };
    static uint8_t prev[12];
    for (int i = 0; i < nmap; i++) {
        int on = SDL_GameControllerGetButton(c, (SDL_GameControllerButton)map[i].b) != 0;
        key_edge(prev[i], on, (keyinfo){ (int)map[i].vk, map[i].scan });
        prev[i] = (uint8_t)on;
    }
    (void)g_prev_btn;
    pad_mouse(c);
}

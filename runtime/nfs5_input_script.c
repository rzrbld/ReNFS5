/* Deterministic input injection for headless validation.
 *
 * This host has no display or controller, so menus cannot be driven by hand.
 * NFS5_INPUT_SCRIPT enables a small scripted driver that presses DirectInput
 * keys through the HLE override hook (nfs5_dinput_inject_key), so a run can
 * reach menus and a race and produce reproducible screenshots, audio captures
 * and performance numbers.
 *
 * Format: comma-separated "<ms>:<action>[:<action>...]" where an action is
 *   down | up | tap            (Enter)
 *   <name>_down | <name>_tap   (named key, see key_names)
 * Time is milliseconds after the game's entry point starts. Actions run in
 * order; taps hold for 80 ms.
 *
 * Example: NFS5_INPUT_SCRIPT="3000:tap,6000:down_down,6200:down_up,8000:tap"
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>

extern void nfs5_dinput_inject_key(uint32_t vk, int down) __attribute__((weak));
/* user32's message queue, so games that read WM_KEYDOWN (not only DirectInput)
 * see the scripted presses too. */
void hle_post_message(uint32_t hwnd, uint32_t message, uint32_t wParam, uint32_t lParam);
void hle_input_mouse(uint32_t message, int screen_x, int screen_y);   /* user32 host API */
uint32_t hle_first_hwnd(void);
uint32_t hle_focus_window(void);

/* Windows VK codes for the keys the menus use (Win32 ABI, not SDL). */
static const struct { const char *name; uint32_t vk; } key_names[] = {
    { "enter", 0x0D }, { "return", 0x0D }, { "space", 0x20 }, { "esc", 0x1B },
    { "escape", 0x1B }, { "up", 0x26 }, { "down", 0x28 }, { "left", 0x25 },
    { "right", 0x27 }, { "tab", 0x09 }, { "backspace", 0x08 },
    { "a", 'A' }, { "b", 'B' }, { "c", 'C' }, { "x", 'X' }, { "y", 'Y' },
    { "z", 'Z' }, { "s", 'S' }, { "f1", 0x70 }, { "f2", 0x71 }, { "f12", 0x7B },
    { "ctrl", 0x11 }, { "shift", 0x10 }, { "alt", 0x12 },
};

static uint32_t lookup(const char *name) {
    for (size_t i = 0; i < sizeof key_names / sizeof *key_names; i++)
        if (!strcasecmp(name, key_names[i].name)) return key_names[i].vk;
    if (name[0] && !name[1] && ((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z')))
        return (uint32_t)(unsigned char)toupper((unsigned char)name[0]);   /* any letter, e.g. p/c/h */
    return 0;
}

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Mouse actions let the front-end (which is also mouse-driven) be navigated
 * headlessly: "move=XxY", "click=XxY" in 640x480 screen coordinates. */
static int mouse_action(const char *action) {
    int is_click = !strncmp(action, "click=", 6);
    int is_move = !strncmp(action, "move=", 5);
    if (!is_click && !is_move) return 0;
    const char *p = action + (is_click ? 6 : 5);
    char *end = NULL;
    long x = strtol(p, &end, 10);
    if (!end || (*end != 'x' && *end != ',')) return 0;
    long y = strtol(end + 1, NULL, 10);
    hle_input_mouse(0x0200u /* WM_MOUSEMOVE */, (int)x, (int)y);
    if (is_click) {
        /* The FE updates its hover/active button from the move before it
         * processes a button-down; give it a frame or two. */
        sleep_ms(150);
        hle_input_mouse(0x0201u /* WM_LBUTTONDOWN */, (int)x, (int)y);
        sleep_ms(90);
        hle_input_mouse(0x0202u /* WM_LBUTTONUP */, (int)x, (int)y);
    }
    fprintf(stderr, "[input] %s %ldx%ld\n", is_click ? "click" : "move", x, y);
    return 1;
}

static void apply(const char *action, int *held) {
    if (mouse_action(action)) return;
    if (!strncmp(action, "msg=", 4)) {                 /* raw window message, bring-up */
        unsigned long m = strtoul(action + 4, NULL, 0);
        uint32_t hwnd = hle_first_hwnd();
        hle_post_message(hwnd, (uint32_t)m, 1, 0);
        fprintf(stderr, "[input] post msg=%#lx to %#x\n", m, hwnd);
        return;
    }
    char name[32];
    int down = -1;
    size_t n = strlen(action);
    if(!strcasecmp(action, "down")) { name[0]=0; down=1; }
    else if(!strcasecmp(action, "up")) { name[0]=0; down=0; }
    else if(!strcasecmp(action, "tap")) { name[0]=0; down=2; }
    else if (n > 5 && !strcmp(action + n - 5, "_down")) {
        snprintf(name, sizeof name, "%.*s", (int)(n - 5), action); down = 1;
    } else if (n > 4 && !strcmp(action + n - 4, "_tap")) {
        snprintf(name, sizeof name, "%.*s", (int)(n - 4), action); down = 2;
    } else if (n > 3 && !strcmp(action + n - 3, "_up")) {
        snprintf(name, sizeof name, "%.*s", (int)(n - 3), action); down = 0;
    } else {
        fprintf(stderr, "[input] unknown action '%s'\n", action);
        return;
    }
    uint32_t vk = name[0] ? lookup(name) : 0x0D;      /* bare actions are Enter */
    if (!vk) { fprintf(stderr, "[input] unknown key '%s'\n", name); return; }
    if (!nfs5_dinput_inject_key) {
        fprintf(stderr, "[input] dinput injection hook not present; script inert\n");
        return;
    }
    uint32_t hwnd = hle_first_hwnd();
    uint32_t scan = vk << 16;   /* lParam scan code field is enough for a game */
    /* nfs5_dinput_inject_key() already posts the window message with the real
     * scan code (hle_input_key in user32.c) and updates the DirectInput state;
     * posting a second, scan-code-less message here used to confuse the game's
     * key table (two downs/ups per tap). */
    (void)hwnd; (void)scan;
    if (down == 2) {
        nfs5_dinput_inject_key(vk, 1);
        sleep_ms(80);
        nfs5_dinput_inject_key(vk, 0);
    } else {
        nfs5_dinput_inject_key(vk, down);
        *held = down;
    }
    fprintf(stderr, "[input] %s %s (vk %02x)\n", down == 0 ? "up" : "down", name[0] ? name : "enter", vk);
}

static void *runner(void *arg) {
    (void)arg;
    char *script = strdup(getenv("NFS5_INPUT_SCRIPT"));
    long elapsed = 0;
    int held = 0;
    /* strtok_r: the inner loop must not clobber the outer tokenizer's state. */
    char *outer_state = NULL;
    for (char *entry = strtok_r(script, ",", &outer_state); entry;
         entry = strtok_r(NULL, ",", &outer_state)) {
        char *colon = strchr(entry, ':');
        if (!colon) continue;
        *colon = 0;
        long at = strtol(entry, NULL, 10);
        if (at > elapsed) { sleep_ms(at - elapsed); elapsed = at; }
        char *inner_state = NULL;
        for (char *act = strtok_r(colon + 1, ":", &inner_state); act;
             act = strtok_r(NULL, ":", &inner_state)) apply(act, &held);
    }
    (void)held;
    free(script);
    fprintf(stderr, "[input] script finished\n");
    return NULL;
}

void nfs5_input_script_start(void) {
    if (!getenv("NFS5_INPUT_SCRIPT")) return;
    if (!nfs5_dinput_inject_key) {
        fprintf(stderr, "[input] NFS5_INPUT_SCRIPT set but dinput injection is not available\n");
        return;
    }
    pthread_t t;
    if (!pthread_create(&t, NULL, runner, NULL)) pthread_detach(t);
}

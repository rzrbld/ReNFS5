/* DirectDraw present-level frame capture.
 *
 * The front-end presents through DirectDraw (Flip/Blt to the primary), not only
 * through the D3D7 device, so D3D7-level capture misses those screens. When
 * NFS5_DD_SHOTS=1 this hooks win32hle's primary-surface change callback and
 * writes the current primary surface to build/shots_dd/dd_%05u.png, giving
 * visual evidence for every presented FE frame regardless of path.
 *
 * NFS5_DD_SHOTS_EVERY=N  capture every Nth presented frame (default 10)
 * NFS5_DD_SHOTS_DIR=dir  output directory (default build/shots_dd)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "win32hle.h"

/* stb_image_write is implemented once in runtime/render/shot.c */
int stbi_write_png(char const *filename, int w, int h, int comp, const void *data, int stride_in_bytes);

static int g_every = 10, g_count;
static unsigned g_last_seq = (unsigned)-1;
static char g_dir[512] = "build/shots_dd";

static void mkdirs(const char *p) {
    char t[512];
    snprintf(t, sizeof t, "%s", p);
    for (char *s = t + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0, mkdir(t, 0755), *s = '/';
    }
    mkdir(t, 0755);
}

static void (*g_prev_primary_hook)(void);   /* the screen layer's presenter, if any */

static void dump_primary(void) {
    if (g_prev_primary_hook) g_prev_primary_hook();   /* show the frame as well as capture it */
    const uint8_t *px = NULL;
    int w = 0, h = 0, pitch = 0, bpp = 0;
    const uint32_t *pal = NULL;
    unsigned seq = 0;
    if (!hle_dd_frame(&px, &w, &h, &pitch, &bpp, &pal, &seq)) return;
    if (seq == g_last_seq) return;
    g_last_seq = seq;
    if (g_count++ % g_every) return;

    uint8_t *rgb = (uint8_t *)malloc((size_t)w * (size_t)h * 3);
    if (!rgb) return;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = px + (size_t)y * (size_t)pitch;
        for (int x = 0; x < w; x++) {
            uint8_t r, g, b;
            if (bpp == 16) {
                /* the D3D7/DDraw mode is 16bpp; dx7z's backbuffer is RGB565 */
                uint16_t v = *(const uint16_t *)(row + 2 * x);
                r = (uint8_t)(((v >> 11) & 0x1f) * 255 / 31);
                g = (uint8_t)(((v >> 5) & 0x3f) * 255 / 63);
                b = (uint8_t)((v & 0x1f) * 255 / 31);
            } else if (bpp == 8) {
                uint32_t c = pal ? pal[row[x]] : 0;
                r = (uint8_t)((c >> 16) & 0xff), g = (uint8_t)((c >> 8) & 0xff), b = (uint8_t)(c & 0xff);
            } else {
                const uint8_t *p = row + 4 * x;
                r = p[2], g = p[1], b = p[0];
            }
            uint8_t *o = rgb + ((size_t)y * (size_t)w + (size_t)x) * 3;
            o[0] = r, o[1] = g, o[2] = b;
        }
    }
    mkdirs(g_dir);
    char path[600];
    snprintf(path, sizeof path, "%s/dd_%05d.png", g_dir, g_count / g_every);
    stbi_write_png(path, w, h, 3, rgb, w * 3);
    free(rgb);
}

void nfs5_present_dump_start(void) {
    if (!getenv("NFS5_DD_SHOTS")) return;
    const char *e = getenv("NFS5_DD_SHOTS_EVERY");
    if (e && atoi(e) > 0) g_every = atoi(e);
    if (getenv("NFS5_DD_SHOTS_DIR")) snprintf(g_dir, sizeof g_dir, "%s", getenv("NFS5_DD_SHOTS_DIR"));
    g_prev_primary_hook = hle_dd_primary_hook;
    hle_dd_primary_hook = dump_primary;
    fprintf(stderr, "[ddshots] primary-surface capture every %d presented frames -> %s\n", g_every, g_dir);
}

/* shot.c - PNG screenshot writer used by the D3D7 renderer and the probe.
 * stb_image_write is public domain (v1.16, https://github.com/nothings/stb);
 * the vendored copy lives next to this file. */
#define _GNU_SOURCE
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "renderer.h"

/* rgba: RGBA8888, rows top-to-bottom (our FBO readback order). */
int r_write_png(const char *path, const uint8_t *rgba, int w, int h) {
    return stbi_write_png(path, w, h, 4, rgba, w * 4);
}

int r_write_png_compressed(const char *path, const uint8_t *rgba, int w, int h, int stride) {
    return stbi_write_png(path, w, h, 4, rgba, stride);
}

// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// headless backend, no host at all: the canvas geometry comes from the
// command line and every present is written out as a ppm dump

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend.h"
#include "state.h"

static uint32_t canvas_w = 640, canvas_h = 480;
static const char *ppm_path = "build/framebuffer.ppm";

static const char *flag_value(int argc, char **argv, const char *flag) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], flag) == 0 && i + 1 < argc) {
            return argv[i + 1];
        }
    }
    return 0;
}

static int init(int argc, char **argv) {
    const char *w = flag_value(argc, argv, "--width");
    const char *h = flag_value(argc, argv, "--height");
    const char *p = flag_value(argc, argv, "--ppm");
    if (w) {
        canvas_w = (uint32_t)strtoul(w, 0, 10);
    }
    if (h) {
        canvas_h = (uint32_t)strtoul(h, 0, 10);
    }
    if (p) {
        ppm_path = p;
    }
    if (canvas_w == 0 || canvas_h == 0 || canvas_w > 16384 || canvas_h > 16384) {
        return -1;
    }
    return 0;
}

static void get_size(uint32_t *w, uint32_t *h) {
    *w = canvas_w;
    *h = canvas_h;
}

static void present(const uint32_t *fb, const struct x12_rect *rects,
                    int n_rects) {
    // the ppm dump is a test artifact, not a scanout, it always carries
    // the whole frame so asserts stay independent of damage regions
    (void)rects;
    (void)n_rects;
    FILE *f = fopen(ppm_path, "wb");
    if (!f) {
        return;
    }
    fprintf(f, "P6\n%u %u\n255\n", canvas_w, canvas_h);
    for (uint32_t y = 0; y < canvas_h; y++) {
        for (uint32_t x = 0; x < canvas_w; x++) {
            uint32_t v = fb[(size_t)y * canvas_w + x];
            uint8_t rgb[3] = {
                (uint8_t)((v >> 16) & 255),
                (uint8_t)((v >> 8) & 255),
                (uint8_t)(v & 255),
            };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

static void pump(void) {
}

static int halt_requested(void) {
    return 0;
}

static void shutdown(void) {
}

const struct x12_backend_ops x12_backend_headless = {
    .init = init,
    .get_size = get_size,
    .present = present,
    .pump = pump,
    .halt_requested = halt_requested,
    .shutdown = shutdown,
    .host_cursor = 0,
};

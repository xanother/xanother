// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// the gpu canvas contract: random scenes through the cpu blend path and
// the gpu engine must land byte for byte on the same framebuffer. the
// scene driver mirrors the compositor pass order - fill the damage,
// then fold the views bottom to top over it - so a pass here is a pass
// in the compositor. run without a gl stack it prints SKIP and exits
// zero, the cpu path stays the reference there

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "gpu.h"
#include "state.h"

static uint64_t rng_state;

static uint32_t rnd(uint32_t mod) {
    rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)((rng_state >> 33) % mod);
}

struct scene_view {
    struct x12_view view;
    struct x12_bufref ref;
    uint32_t scale;
};

// the argb buffers stay valid premultiplied - channel never exceeds
// alpha - so both paths walk the same contract range
static void fill_argb(uint8_t *px, uint32_t w, uint32_t h) {
    for (uint32_t i = 0; i < w * h; i++) {
        uint8_t a = (uint8_t)rnd(256);
        px[i * 4 + 0] = (uint8_t)rnd((uint32_t)a + 1);
        px[i * 4 + 1] = (uint8_t)rnd((uint32_t)a + 1);
        px[i * 4 + 2] = (uint8_t)rnd((uint32_t)a + 1);
        px[i * 4 + 3] = a;
    }
}

static void fill_xrgb(uint8_t *px, uint32_t w, uint32_t h) {
    for (uint32_t i = 0; i < w * h; i++) {
        px[i * 4 + 0] = (uint8_t)rnd(256);
        px[i * 4 + 1] = (uint8_t)rnd(256);
        px[i * 4 + 2] = (uint8_t)rnd(256);
        px[i * 4 + 3] = 0xFF;
    }
}

static int run_scene(uint32_t seed) {
    rng_state = seed * 2654435761u + 12345;
    uint32_t fw = 32 + rnd(224);
    uint32_t fh = 32 + rnd(224);
    size_t fb_words = (size_t)fw * fh;

    uint32_t *cpu_fb = calloc(fb_words, 4);
    uint32_t *gpu_fb = calloc(fb_words, 4);
    uint32_t *band_fb = calloc(fb_words, 4);
    if (!cpu_fb || !gpu_fb || !band_fb) {
        return 0;
    }

    struct scene_view views[4];
    int n_views = 1 + (int)rnd(4);
    for (int v = 0; v < n_views; v++) {
        // the logical view and its buffer are independent sizes, the
        // buffer may be smaller (keep old content past its edge) or
        // larger (only the view rect shows)
        views[v].view.w = 1 + rnd(160);
        views[v].view.h = 1 + rnd(160);
        views[v].view.x = (int32_t)rnd(fw + 40) - 20;
        views[v].view.y = (int32_t)rnd(fh + 40) - 20;
        views[v].scale = rnd(2) ? 256 + rnd(256) : 256;
        views[v].ref.width = 1 + rnd(200);
        views[v].ref.height = 1 + rnd(200);
        views[v].ref.stride = views[v].ref.width * 4;
        views[v].ref.format =
            rnd(2) ? X12_FORMAT_ARGB8888 : X12_FORMAT_XRGB8888;
        views[v].ref.object = 0;
        uint8_t *px = malloc((size_t)views[v].ref.height *
                             views[v].ref.stride);
        if (!px) {
            return 0;
        }
        views[v].ref.pixels = px;
        if (views[v].ref.format == X12_FORMAT_ARGB8888) {
            fill_argb(px, views[v].ref.width, views[v].ref.height);
        } else {
            fill_xrgb(px, views[v].ref.width, views[v].ref.height);
        }
    }

    // one clip rect per pass half the time, two disjoint rects otherwise
    struct x12_rect rects[2];
    int n_rects = 1;
    rects[0].x = 0;
    rects[0].y = 0;
    rects[0].w = fw;
    rects[0].h = fh;
    if (rnd(2)) {
        n_rects = 2;
        rects[0].h = 1 + rnd(fh - 1);
        rects[1].x = 0;
        rects[1].y = (int32_t)rects[0].h;
        rects[1].w = fw;
        rects[1].h = fh - rects[0].h;
    }

    if (!x12_gpu_canvas(fw, fh)) {
        return 0;
    }

    // the compositor order per rect: opaque background then the views
    // bottom to top, both paths on the very same rects
    for (int k = 0; k < n_rects; k++) {
        x12_cpu_fill(cpu_fb, fw, fh, &rects[k], 0xFF000000u);
        x12_gpu_fill(&rects[k], 1);
        for (int v = 0; v < n_views; v++) {
            x12_cpu_blend_view(cpu_fb, fw, fh, &views[v].view, &views[v].ref,
                               views[v].scale, &rects[k]);
            x12_gpu_blend(&views[v].view, &views[v].ref, views[v].scale,
                          &rects[k], 1);
        }
    }

    x12_gpu_readback((uint8_t *)gpu_fb, fw * 4);
    int ok = memcmp(cpu_fb, gpu_fb, fb_words * 4) == 0;

    // the band readback rides the rows the frame damage covers, one
    // band over both rects when there are two
    if (ok) {
        uint32_t y0 = (uint32_t)rects[0].y;
        uint32_t y1 = (uint32_t)rects[n_rects - 1].y + rects[n_rects - 1].h;
        memcpy(band_fb, cpu_fb, fb_words * 4);
        // wipe the band rows, the readback must restore exactly them
        memset((uint8_t *)band_fb + (size_t)y0 * fw * 4, 0xA5,
               (size_t)(y1 - y0) * fw * 4);
        x12_gpu_readback_band((uint8_t *)band_fb, fw * 4, y0, y1);
        ok = memcmp(cpu_fb, band_fb, fb_words * 4) == 0;
    }

    // the unorm round trip: the scanout shader normalized and read back
    // unorm must land on the very same bytes
    if (ok) {
        memset(band_fb, 0, fb_words * 4);
        x12_gpu_unorm_probe((uint8_t *)band_fb, fw * 4);
        ok = memcmp(cpu_fb, band_fb, fb_words * 4) == 0;
    }

    if (!ok) {
        fprintf(stderr, "gpu contract: FAIL seed=%u canvas=%ux%u\n", seed, fw,
                fh);
        for (int v = 0; v < n_views; v++) {
            fprintf(stderr,
                    "  view%d (%d,%d %ux%u) buf %ux%u scale=%u fmt=%c\n", v,
                    views[v].view.x, views[v].view.y, views[v].view.w,
                    views[v].view.h, views[v].ref.width, views[v].ref.height,
                    views[v].scale,
                    views[v].ref.format == X12_FORMAT_ARGB8888 ? 'a' : 'x');
        }
    }

    for (int v = 0; v < n_views; v++) {
        free((uint8_t *)views[v].ref.pixels);
    }
    free(cpu_fb);
    free(gpu_fb);
    free(band_fb);
    return ok;
}

int main(int argc, char **argv) {
    int scenes = argc > 1 ? atoi(argv[1]) : 200;
    if (!x12_gpu_init(NULL)) {
        printf("gpu contract: SKIP (no gl stack, cpu path is the reference)\n");
        return 0;
    }
    int bad = 0;
    for (int s = 0; s < scenes; s++) {
        if (!run_scene((uint32_t)(s + 1))) {
            bad++;
            if (bad > 3) {
                break;
            }
        }
    }
    x12_gpu_shutdown();
    if (bad) {
        printf("gpu contract: FAIL (%d scenes broken of %d)\n", bad, scenes);
        return 1;
    }
    printf("gpu contract: PASS (%d scenes bit exact)\n", scenes);
    return 0;
}

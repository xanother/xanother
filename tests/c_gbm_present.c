// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// gbm present smoke on a real render node: allocate the scanout pair,
// fold the canvas into both bos through the unorm pass and read them
// back through their imported textures. the bytes must land exactly on
// the canvas the integer pass painted, which is the same promise the
// page flip gives the scanout. no drm node prints SKIP and exits zero

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cpu.h"
#include "gpu.h"
#include "state.h"

static void pattern_argb(uint8_t *px, uint32_t w, uint32_t h) {
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t a = (uint8_t)(64 + (x + y) % 128);
            px[(y * w + x) * 4 + 0] = (uint8_t)((x * 3) % ((uint32_t)a + 1));
            px[(y * w + x) * 4 + 1] = (uint8_t)((y * 5) % ((uint32_t)a + 1));
            px[(y * w + x) * 4 + 2] = (uint8_t)((x * y) % ((uint32_t)a + 1));
            px[(y * w + x) * 4 + 3] = a;
        }
    }
}

int main(int argc, char **argv) {
    const char *node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
    if (access(node, R_OK | W_OK) != 0) {
        printf("gbm present: SKIP (no drm node %s)\n", node);
        return 0;
    }
    if (!x12_gpu_init(node)) {
        printf("gbm present: SKIP (no gl stack on %s)\n", node);
        return 0;
    }
    enum { W = 64, H = 48 };
    if (!x12_gpu_canvas(W, H)) {
        printf("gbm present: FAIL (no canvas on %s)\n", node);
        return 1;
    }

    // one scene both paths agree on: black background, an opaque view
    // and a premultiplied one over it
    static uint8_t xr[W * 16 * 4], ar[24 * 24 * 4];
    uint32_t *cpu_fb = calloc((size_t)W * H, 4);
    uint32_t *gpu_fb = calloc((size_t)W * H, 4);
    uint8_t *bo_fb = calloc((size_t)W * H, 4);
    if (!cpu_fb || !gpu_fb || !bo_fb) {
        return 1;
    }
    for (size_t i = 0; i < sizeof xr / 4; i++) {
        xr[i * 4 + 0] = (uint8_t)(i * 7);
        xr[i * 4 + 1] = (uint8_t)(i * 13);
        xr[i * 4 + 2] = (uint8_t)(i * 29);
        xr[i * 4 + 3] = 0xFF;
    }
    pattern_argb(ar, 24, 24);

    struct x12_rect full = { 0, 0, W, H };
    struct x12_view v1 = { 8, 4, W / 2, 16 };
    struct x12_bufref r1 = { 0, W / 2, 16, W / 2 * 4, X12_FORMAT_XRGB8888,
                             xr };
    struct x12_view v2 = { 20, 16, 32, 32 };
    struct x12_bufref r2 = { 0, 24, 24, 24 * 4, X12_FORMAT_ARGB8888, ar };

    x12_cpu_fill(cpu_fb, W, H, &full, 0xFF000000u);
    x12_cpu_blend_view(cpu_fb, W, H, &v1, &r1, 256, &full);
    x12_cpu_blend_view(cpu_fb, W, H, &v2, &r2, 384, &full);
    x12_gpu_fill(&full, 1);
    x12_gpu_blend(&v1, &r1, 256, &full, 1);
    x12_gpu_blend(&v2, &r2, 384, &full, 1);
    x12_gpu_readback((uint8_t *)gpu_fb, W * 4);

    if (memcmp(cpu_fb, gpu_fb, (size_t)W * H * 4) != 0) {
        printf("gbm present: FAIL (canvas blend drifted on %s)\n", node);
        return 1;
    }

    int fds[2];
    uint32_t strides[2];
    if (!x12_gpu_present_setup(W, H, fds, strides)) {
        printf("gbm present: FAIL (no scanout bos on %s)\n", node);
        return 1;
    }
    int bad = 0;
    for (int b = 0; b < 2 && !bad; b++) {
        if (fds[b] < 0 || strides[b] < W * 4) {
            printf("gbm present: FAIL (bad bo %d on %s)\n", b, node);
            bad = 1;
            break;
        }
        if (!x12_gpu_present_copy(b)) {
            printf("gbm present: FAIL (copy %d on %s)\n", b, node);
            bad = 1;
            break;
        }
        memset(bo_fb, 0, (size_t)W * H * 4);
        x12_gpu_present_readback(b, bo_fb, W * 4);
        // the dma path may pad rows past the visible width, the compare
        // walks the visible square the flip shows
        for (uint32_t y = 0; y < H && !bad; y++) {
            if (memcmp(bo_fb + (size_t)y * W * 4,
                       (uint8_t *)cpu_fb + (size_t)y * W * 4, W * 4) != 0) {
                printf("gbm present: FAIL (bo %d row %u on %s)\n", b, y, node);
                bad = 1;
            }
        }
    }
    for (int b = 0; b < 2; b++) {
        if (fds[b] >= 0) {
            close(fds[b]);
        }
    }
    x12_gpu_present_shutdown();
    x12_gpu_shutdown();
    free(cpu_fb);
    free(gpu_fb);
    free(bo_fb);
    if (bad) {
        return 1;
    }
    printf("gbm present: PASS (both bos byte exact on %s, strides %u/%u)\n",
           node, strides[0], strides[1]);
    return 0;
}

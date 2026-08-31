// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#include "cpu.h"

void x12_cpu_fill(uint32_t *fb, uint32_t fb_w, uint32_t fb_h,
                  const struct x12_rect *r, uint32_t color) {
    for (uint32_t y = (uint32_t)r->y; y < (uint32_t)r->y + r->h && y < fb_h; y++) {
        for (uint32_t x = (uint32_t)r->x; x < (uint32_t)r->x + r->w && x < fb_w; x++) {
            fb[(size_t)y * fb_w + x] = color;
        }
    }
}

void x12_cpu_blend_view(uint32_t *fb, uint32_t fb_w, uint32_t fb_h,
                        const struct x12_view *view, const struct x12_bufref *ref,
                        uint32_t scale, const struct x12_rect *clip) {
    if (!ref->pixels) {
        return;
    }
    if (scale == 0) {
        scale = 256;
    }
    // intersection of the clip region with the view rectangle
    int32_t x0 = clip->x > view->x ? clip->x : view->x;
    int32_t y0 = clip->y > view->y ? clip->y : view->y;
    int32_t x1 = clip->x + (int32_t)clip->w < view->x + (int32_t)view->w
                     ? clip->x + (int32_t)clip->w
                     : view->x + (int32_t)view->w;
    int32_t y1 = clip->y + (int32_t)clip->h < view->y + (int32_t)view->h
                     ? clip->y + (int32_t)clip->h
                     : view->y + (int32_t)view->h;
    for (int32_t y = y0; y < y1; y++) {
        if (y < 0 || y >= (int32_t)fb_h) {
            continue;
        }
        // the buffer carries scale times the logical view, nearest
        // neighbour maps logical coords back to pixels
        uint32_t vy = (uint32_t)(y - view->y) * scale / 256;
        if (vy >= ref->height) {
            continue;
        }
        const uint8_t *row = ref->pixels + (size_t)ref->stride * vy;
        for (int32_t x = x0; x < x1; x++) {
            if (x < 0 || x >= (int32_t)fb_w) {
                continue;
            }
            uint32_t vx = (uint32_t)(x - view->x) * scale / 256;
            if (vx >= ref->width) {
                continue;
            }
            uint32_t *dst = &fb[(size_t)y * fb_w + x];
            const uint8_t *sp = row + 4u * vx;
            if (ref->format != X12_FORMAT_ARGB8888) {
                // opaque buffer, straight copy of the color bytes
                *dst = (uint32_t)sp[0] | ((uint32_t)sp[1] << 8) |
                       ((uint32_t)sp[2] << 16) | 0xFF000000u;
                continue;
            }
            // premultiplied over: out = src + dst * (1 - src_alpha),
            // everything stays premultiplied and the framebuffer ends
            // opaque because the background underneath is opaque
            uint32_t sa = sp[3];
            uint32_t d = *dst;
            uint32_t inv = 255 - sa;
            uint32_t b = (uint32_t)sp[0] + (d & 0xFFu) * inv / 255;
            uint32_t g = (uint32_t)sp[1] + ((d >> 8) & 0xFFu) * inv / 255;
            uint32_t r = (uint32_t)sp[2] + ((d >> 16) & 0xFFu) * inv / 255;
            uint32_t a = sa + (d >> 24) * inv / 255;
            *dst = b | (g << 8) | (r << 16) | (a << 24);
        }
    }
}

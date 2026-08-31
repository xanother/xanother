// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#ifndef X12_CPU_H
#define X12_CPU_H

// the cpu reference engine. every pixel rule lives here so the gpu twin
// has exactly one definition to agree with, byte for byte. the blend is
// premultiplied over, xrgb copies opaque and the scale is nearest
// neighbour from buffer pixels back to logical coordinates

#include <stdint.h>

#include "state.h"

// paints the region with an opaque color
void x12_cpu_fill(uint32_t *fb, uint32_t fb_w, uint32_t fb_h,
                  const struct x12_rect *r, uint32_t color);

// blends one surface view into the framebuffer, clipped to the region.
// scale is fixed24p8, 256 means one buffer pixel per logical pixel, the
// buffer carries scale times the logical size
void x12_cpu_blend_view(uint32_t *fb, uint32_t fb_w, uint32_t fb_h,
                        const struct x12_view *view, const struct x12_bufref *ref,
                        uint32_t scale, const struct x12_rect *clip);

#endif

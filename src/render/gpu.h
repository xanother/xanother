// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#ifndef X12_GPU_H
#define X12_GPU_H

#include <stdint.h>

#include "state.h"

// the gl canvas engine: the composite pass blends client buffers into a
// gpu canvas with the same integer premultiplied math the cpu path
// uses, so pixels stay bit exact across both paths. every entry point
// resolves through dlopen at init, the binary carries no hard gl
// dependency - a box without libegl runs the cpu blend path and nothing
// else changes. init degrades quietly: zero means the cpu path carries
// on

// node is reserved for the device platform of a render node, NULL
// selects the default display software contexts run on
int x12_gpu_init(const char *node);
void x12_gpu_shutdown(void);

int x12_gpu_ready(void);

// allocates the blend surfaces for one canvas size, zero on failure
int x12_gpu_canvas(uint32_t w, uint32_t h);

// blends one surface view over the canvas, clipped to each rect, the
// exact twin of x12_cpu_blend_view: the same view rect bounds, the same
// fixed24p8 nearest sampling from the buffer origin, the same keep old
// content rule past the buffer edge. scale zero means 256 like the cpu
int x12_gpu_blend(const struct x12_view *view, const struct x12_bufref *ref,
                  uint32_t scale, const struct x12_rect *rects, int n_rects);

// paints the rects opaque black, the background the cpu pass fills
// before its views blend over it
void x12_gpu_fill(const struct x12_rect *rects, int n_rects);

// the blended frame becomes the destination of the next blend
void x12_gpu_dst_commit(void);

// reads the canvas out top down as exact argb bytes
void x12_gpu_readback(uint8_t *out, uint32_t stride_bytes);

// reads the rows [y0, y1) of the canvas into the framebuffer layout at
// out (which starts at row 0), one transfer per band; rows outside the
// frame damage never cross the gpu bus, which keeps small frames cheap
void x12_gpu_readback_band(uint8_t *out, uint32_t stride_bytes,
                           uint32_t y0, uint32_t y1);

// the unorm conversion probe: the canvas copied through the scanout
// shader into a plain rgba8 target, the diagnostic that proves the
// integer to unorm round trip is exact
int x12_gpu_unorm_probe(uint8_t *out, uint32_t stride_bytes);

// allocates two linear scanout bos of the canvas size on the gbm device
// of the render node the context rides, one pair per canvas size, and
// hands their dma bufs over; the fd belongs to the caller after the
// call, the engine never touches it again. zero means the present
// stays on the cpu path
int x12_gpu_present_setup(uint32_t w, uint32_t h, int fds[2],
                          uint32_t strides[2]);

// folds the canvas into the bo of one slot, flushed before return so
// the page flip the backend arms picks up a finished frame
int x12_gpu_present_copy(int idx);

// reads one present bo back through its texture, the diagnostic path
int x12_gpu_present_readback(int idx, uint8_t *out, uint32_t stride_bytes);

void x12_gpu_present_shutdown(void);

#endif

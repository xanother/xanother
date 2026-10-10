// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#ifndef X12_BACKEND_H
#define X12_BACKEND_H

// host interface: the compositor renders into its canvas and hands the
// damaged regions to the host, backends may ignore the regions and push
// everything, correctness stays the same. the core is host agnostic,
// every host plugs in one of these

#include <stdint.h>

struct x12_rect;

// direct scanout of a client dma buffer: the backend flips the buffer
// straight to the screen when the hardware takes it, the composite pass
// stays the fallback for everything the screen cannot show directly
struct x12_scanout_req {
    int dmabuf_fd;
    uint32_t width, height, format, stride;
    uint64_t modifier;
    uint32_t cookie;  // comes back with the events, names the request
};

// scanout progress: landed = the buffer hit the screen at the reported
// moment, replaced = another frame took the screen and the buffer is
// free to reuse again
#define X12_SCANOUT_LANDED 1
#define X12_SCANOUT_REPLACED 2

struct x12_scanout_event {
    int kind;
    uint32_t cookie;
    uint64_t ts;
};

struct x12_backend_ops {
    // reads its own flags from argv, returns 0 on success
    int (*init)(int argc, char **argv);
    // the canvas geometry the compositor renders into
    void (*get_size)(uint32_t *w, uint32_t *h);
    // pushes the presented frame to the host
    void (*present)(const uint32_t *fb, const struct x12_rect *rects,
                    int n_rects);
    // direct scanout: 1 when the buffer went to the screen through a
    // flip, 0 when the caller must composite instead. both may be NULL
    // on backends that never scan out directly
    int (*scanout)(const struct x12_scanout_req *req);
    // pops one scanout progress event, 1 = filled, 0 = nothing new
    int (*scanout_poll)(struct x12_scanout_event *ev);
    // pumps host events, may present again on expose
    void (*pump)(void);
    // nonzero when the host asks the server to quit
    int (*halt_requested)(void);
    void (*shutdown)(void);
    // nonzero when the host draws its own cursor (nested hosts), the
    // compositor then renders no pointer sprite at all
    int host_cursor;
};

// the host the server runs on, picked once at startup from the command
// line before any client connects
extern const struct x12_backend_ops *x12_backend;

extern const struct x12_backend_ops x12_backend_headless;
extern const struct x12_backend_ops x12_backend_kms;

#endif

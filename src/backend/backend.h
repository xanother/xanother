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

// one physical output of the host. the slot index is the output
// identity the protocol instance numbers name, it never renumbers: a
// down head leaves a hole instead of shifting its neighbours
struct x12_out_info {
    int32_t x, y;  // position on the global canvas
    uint32_t width, height;
    uint32_t mm_width, mm_height;
    uint32_t refresh_millihz;
    const char *name;  // backend owned, static storage
    int live;
};

// hotplug reports through the output poll
#define X12_OUTPUT_ADDED 1
#define X12_OUTPUT_REMOVED 2

struct x12_output_event {
    int kind;
    int out;
};

// direct scanout of a client dma buffer: the backend flips the buffer
// straight to the screen when the hardware takes it, the composite pass
// stays the fallback for everything the screen cannot show directly
struct x12_scanout_req {
    int dmabuf_fd;
    uint32_t width, height, format, stride;
    uint64_t modifier;
    int out;       // output slot the buffer scans out on
    uint32_t cookie;  // comes back with the events, names the request
};

// scanout progress: landed = the buffer hit the screen at the reported
// moment, replaced = another frame took the screen and the buffer is
// free to reuse again
#define X12_SCANOUT_LANDED 1
#define X12_SCANOUT_REPLACED 2

struct x12_scanout_event {
    int kind;
    int out;
    uint32_t cookie;
    uint64_t ts;
};

struct x12_backend_ops {
    // reads its own flags from argv, returns 0 on success
    int (*init)(int argc, char **argv);
    // the canvas geometry the compositor renders into, the bounding
    // box of every live output
    void (*get_size)(uint32_t *w, uint32_t *h);
    // pushes the presented frame to the host, the regions decide which
    // outputs flip and which keep showing what they already have
    void (*present)(const uint32_t *fb, const struct x12_rect *rects,
                    int n_rects);
    // output slots and their geometry, out of range returns 0
    int (*output_count)(void);
    int (*output_info)(int out, struct x12_out_info *info);
    // variable refresh rate: capability probe and the toggle. both
    // refuse with -1 on hosts that cannot do it
    int (*output_vrr)(int out, uint32_t *min_millihz, uint32_t *max_millihz);
    int (*set_vrr)(int out, int enable);
    // hotplug stream, 1 = filled, 0 = nothing new
    int (*output_poll)(struct x12_output_event *ev);
    // direct scanout: 1 when the buffer went to the screen through a
    // flip, 0 when the caller must composite instead. both may be NULL
    // on backends that never scan out directly
    int (*scanout)(const struct x12_scanout_req *req);
    // pops one scanout progress event, 1 = filled, 0 = nothing new
    int (*scanout_poll)(struct x12_scanout_event *ev);
    // pumps host events, may present again on expose
    void (*pump)(void);
    // test bench hook: a host event line the input stand could not
    // parse as input, backends with runtime affordances (the headless
    // canvas and heads) consume them here; 1 = consumed
    int (*host_command)(const char *line);
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

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

struct x12_backend_ops {
    // reads its own flags from argv, returns 0 on success
    int (*init)(int argc, char **argv);
    // the canvas geometry the compositor renders into
    void (*get_size)(uint32_t *w, uint32_t *h);
    // pushes the presented frame to the host
    void (*present)(const uint32_t *fb, const struct x12_rect *rects,
                    int n_rects);
    // pumps host events, may present again on expose
    void (*pump)(void);
    // nonzero when the host asks the server to quit
    int (*halt_requested)(void);
    void (*shutdown)(void);
    // nonzero when the host draws its own cursor (nested hosts), the
    // compositor then renders no pointer sprite at all
    int host_cursor;
};

extern const struct x12_backend_ops x12_backend;

#endif

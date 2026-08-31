// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#ifndef X12_STATE_H
#define X12_STATE_H

// object payload types, plain data only, the logic lives in the modules.
// every field is zero on alloc unless the module says otherwise

#include <stddef.h>
#include <stdint.h>

// damage rectangle in surface logical coordinates, w or h may be zero
struct x12_rect {
    int32_t x, y;
    uint32_t w, h;
};

// view rect of a surface on the canvas in logical coordinates, roles
// drive the origin later, the reference pass anchors at the origin
struct x12_view {
    int32_t x, y;
    uint32_t w, h;
};

// rect list capacity per surface, overflow degrades to full surface
#define X12_MAX_DAMAGE 8

// frames waiting for their fence before their credits leave, a client
// committing faster than its fences signal grows this and dies loudly
#define X12_MAX_RETIRE 8

// frame callbacks riding one committed frame
#define X12_MAX_CBS 8

// 'XR24' and 'AR24', drm fourcc for xrgb8888 and premultiplied argb8888
#define X12_FORMAT_XRGB8888 0x34325258u
#define X12_FORMAT_ARGB8888 0x34325241u

// pixels a buffer reference points at, the snapshot a commit carries
struct x12_bufref {
    uint32_t object;
    uint32_t width, height, stride, format;
    const uint8_t *pixels;
};

struct x12_pool_state {
    uint8_t *map;
    size_t size;
    uint32_t owner;  // unused today, kept next to the mapping it describes
};

struct x12_buffer_state {
    const uint8_t *pixels;
    uint32_t width, height, stride, format;
    uint32_t pool_id;
};

// one committed frame that left the current slot and waits for its fence
// before the release credit leaves. the credit belongs to the buffer that
// earned it, never to whatever the surface shows later
struct x12_retire {
    uint32_t buffer_id;  // 0 = the frame carried no buffer
    int fence;           // sync_file fd, -1 = always ready
    uint32_t cbs[X12_MAX_CBS];
    uint8_t n_cbs;
};

struct x12_surface_state {
    // pending state gathers between commits and lands atomically
    struct x12_bufref pending;
    int has_pending;
    struct x12_rect pending_damage[X12_MAX_DAMAGE];
    int n_pending_damage;  // -1 = the whole surface
    int pending_fence;
    uint32_t pending_scale;      // fixed24p8, 256 = 1.0
    uint32_t pending_transform;
    uint32_t pending_sync;
    uint32_t pending_cbs[X12_MAX_CBS];
    uint8_t n_pending_cbs;

    // current state is what the compositor pass reads
    struct x12_bufref current;
    int has_current;
    int has_frame;  // a commit happened, the frame on screen retires once
    struct x12_rect current_damage[X12_MAX_DAMAGE];
    int n_current_damage;
    uint32_t current_scale, current_transform, current_sync;
    int fence;  // guard of the current frame, -1 = none
    uint32_t current_cbs[X12_MAX_CBS];
    uint8_t n_current_cbs;

    // retired frames in commit order, the front one leaves first
    struct x12_retire retire[X12_MAX_RETIRE];
    uint8_t n_retire;
};

#endif

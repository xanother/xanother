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

// view rect of a surface on the canvas in logical coordinates
struct x12_view {
    int32_t x, y;
    uint32_t w, h;
};

// role_kind values, 0 means the surface is never composited
enum x12_role {
    ROLE_NONE = 0,
    ROLE_TOPLEVEL = 1,
    ROLE_POPUP = 2,
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

    // window role, ROLE_NONE never reaches the canvas. role_id names the
    // object holding the role state, it lives in this connection
    uint32_t role_kind;
    uint32_t role_id;
};

// placement description for popups, the popup copies the values at
// create time and the positioner slot stays behind untouched
struct x12_positioner_state {
    int32_t rect_x, rect_y;
    uint32_t rect_w, rect_h;
    uint32_t anchor, gravity;  // x12_positioner.anchor_kind values
    int32_t off_x, off_y;
};

struct x12_toplevel_state {
    uint32_t surface_id;  // 0 = the surface died under the role
    // configure serial bookkeeping: a commit maps the window only when
    // the latest offer was acknowledged, anything else reoffered
    uint32_t last_config, acked_config;
    int fullscreen;
    uint32_t min_w, min_h, max_w, max_h;
    char title[128], app_id[128];
};

struct x12_popup_state {
    uint32_t surface_id;   // 0 = the surface died under the role
    uint32_t parent_id;    // parent surface, always a toplevel
    // copy of the positioner values taken at create time
    int32_t rect_x, rect_y;
    uint32_t rect_w, rect_h;
    uint32_t anchor, gravity;
    int32_t off_x, off_y;
    uint32_t last_config, acked_config;
    int grabbed;  // armed by a valid grab, dismissal rides with input later
};

// clipboard content offered by one client: the mime list set through
// offer requests before the source is ever claimed
struct x12_data_source_state {
    char mimes[8][128];
    uint32_t n_mimes;
};

// a pull handle onto one selection generation, dead the moment the
// generation moves
struct x12_data_offer_state {
    int primary;       // which slot it pins to
    uint32_t generation;
};

#endif

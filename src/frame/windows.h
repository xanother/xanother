// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#ifndef X12_WINDOWS_H
#define X12_WINDOWS_H

#include <stdint.h>

struct x12_conn;

// the window stack: positions, z order and the placement policies of
// the skeleton window manager. deterministic and parameter free on
// purpose, a real wm swaps this module without touching protocol code.
// the compositor paints bottom to top, input walks it top to bottom

// stack capacity, a client past it loses the map race loudly
#define X12_MAX_VIEWS 32

struct x12_winview {
    int used;
    struct x12_conn *client;
    uint32_t surface_id;
    int32_t x, y;
    uint32_t width, height;  // logical, tracks the surface size
    uint32_t nat_w, nat_h;   // the size offered when leaving fullscreen
    int is_popup;
};

void x12_windows_init(void);
void x12_windows_set_output(uint32_t w, uint32_t h);
void x12_windows_output(uint32_t *w, uint32_t *h);

// stack changes repaint whole frames, the pass takes the flag once
int x12_windows_take_dirty(void);
void x12_windows_force_dirty(void);

int x12_windows_count(void);
const struct x12_winview *x12_windows_at(int i);

// a new view lands on top of the stack, -1 when the stack is full
int x12_windows_add(struct x12_conn *c, uint32_t surface_id,
                    int32_t x, int32_t y, uint32_t w, uint32_t h,
                    int is_popup);
int x12_windows_find(struct x12_conn *c, uint32_t surface_id);
void x12_windows_remove(int idx);
void x12_windows_remove_client(struct x12_conn *c);

// geometry updates only dirty the stack when something moved
void x12_windows_update(int idx, int32_t x, int32_t y, uint32_t w,
                        uint32_t h);

// placement policies: diagonal cascade for toplevels, origin pin for
// fullscreen, anchor rect math for popups. all clamp into the output
void x12_windows_place_toplevel(int32_t *x, int32_t *y);
void x12_windows_place_fullscreen(int idx, int on);
void x12_windows_place_popup(int32_t rect_x, int32_t rect_y,
                             uint32_t rect_w, uint32_t rect_h,
                             uint32_t anchor, uint32_t gravity,
                             int32_t off_x, int32_t off_y,
                             uint32_t pop_w, uint32_t pop_h,
                             const struct x12_winview *parent,
                             int32_t *x, int32_t *y);

#endif

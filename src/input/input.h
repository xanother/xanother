// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#ifndef X12_INPUT_H
#define X12_INPUT_H

#include <stdint.h>

#include "conn.h"

// input router: raw host events arrive through x12_input_push, once per
// main loop iteration x12_input_dispatch turns them into seat protocol
// events. routing policies live here, all documented and none hardcoded
// to a particular window layout:
//   pointer  - hover, hit tested against the window stack top down
//   keyboard - click to focus, presses on a toplevel move keyboard focus
//   popups   - an armed popup grabs the pointer, a press outside its
//              bounds dismisses it and the press is rerouted normally
// a focus change raises the focused toplevel above other toplevels.
// hover, focus and grab are tracked by window identity, client plus
// surface id, because stack indexes shift whenever a window closes.
// every output lives in one global coordinate space, so the same hit
// test spans however many monitors the backend lays out in it

enum x12_input_kind {
    IN_MOTION,
    IN_BUTTON,
    IN_KEY,
    IN_AXIS,
};

struct x12_input_event {
    enum x12_input_kind kind;
    int32_t x, y;        // motion, global coordinates
    uint32_t code;       // button index or key code
    uint32_t pressed;    // 1 pressed, 0 released
    uint32_t axis;       // axis_kind enum value, see x12_pointer
    int32_t value;       // axis scroll in fixed24p8, positive is down/right
};

void x12_input_push(const struct x12_input_event *e);

// drains the queue and routes events to seat clients, call once per loop
void x12_input_dispatch(void);

// arms a popup grab, views are tracked by identity because stack indexes
// shift on removal; a press outside the armed popup dismisses it
void x12_input_set_grab(struct x12_conn *c, uint32_t surface_id);

// nonzero when the client actually received this serial as a button
// press, the only input a popup grab may reference
int x12_input_serial_valid(struct x12_conn *c, uint32_t serial);

// current pointer position and the connection owning the surface under
// it (grab target while a grab is armed), null means background
void x12_input_pointer(int32_t *x, int32_t *y, struct x12_conn **c);

#endif

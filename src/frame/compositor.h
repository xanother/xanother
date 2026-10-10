// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#ifndef X12_COMPOSITOR_H
#define X12_COMPOSITOR_H

#include <stddef.h>
#include <stdint.h>

#include "conn.h"

// the damage driven composite pass: it collects what changed since the
// last pass, repaints only that and hands the result to the backend. it
// owns the canvas and is the only writer of what is on screen

// sizes the canvas, called once the backend reports its geometry
void x12_comp_set_size(uint32_t w, uint32_t h);

// forces the next pass to repaint everything, unmap, destroy and later
// stack moves go through here since their old pixels must be covered
void x12_comp_full_damage(void);

// one pass over every surface of every client, surfaces contribute their
// current damage in logical coordinates and lose it to the pass. returns
// nonzero when a frame was presented
int x12_comp_pass(struct x12_conn *conns, size_t n);

// scanout progress: turns flip landings into present feedback and
// replaces into release credits, called once per loop round. drop
// forgets the sessions of a dying connection
void x12_comp_scanout_pump(void);
void x12_comp_scanout_drop(struct x12_conn *c);

#endif

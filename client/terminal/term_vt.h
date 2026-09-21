// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// vt parser: turns the pty byte stream into grid operations. implements
// exactly the subset the shipped x12-term terminfo advertises, so
// programs only send sequences the parser understands. everything else
// is consumed silently, never printed as garbage

#ifndef X12_TERM_VT_H
#define X12_TERM_VT_H

#include <stddef.h>
#include <stdint.h>

#include "term_grid.h"

struct x12term_vt {
    // ground, esc, csi
    uint8_t state;
    // csi accumulation
    uint8_t private;
    uint32_t params[8];
    uint32_t n_params;
    uint32_t cur_param;
    int seen_digit;
    // saved cursor for esc 7 / esc 8 and csi s / u
    uint32_t saved_x, saved_y;
    // utf8 continuation accumulator
    uint8_t utf8[3];
    uint32_t utf8_len, utf8_need;
    uint32_t utf8_cp;
};

void term_vt_reset(struct x12term_vt *vt);
// feeds pty bytes into the grid, the grid tracks its own damage
void term_vt_feed(struct x12term_grid *g, struct x12term_vt *vt,
                  const uint8_t *data, size_t n);

#endif

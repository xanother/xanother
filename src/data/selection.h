// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#ifndef X12_SELECTION_H
#define X12_SELECTION_H

#include <stddef.h>
#include <stdint.h>

#include "conn.h"

// the clipboard and primary selection store. the pull model puts all
// moving parts on the caller side: this file tracks ownership and
// generations and fans the change signals out through the server client
// table registered at init, the request handlers stay thin

// what a claim or a clear did to the slot, the caller decides the events
struct x12_selection_change {
    struct x12_conn *prev_owner;
    uint32_t prev_source;
    int changed;
};

// the store broadcasts through this table, the server is single
// threaded and the array outlives every connection
void x12_selection_init(struct x12_conn *conns, size_t n);

void x12_selection_claim(int primary, struct x12_conn *c, uint32_t source_id,
                         struct x12_selection_change *out);
void x12_selection_clear(int primary, struct x12_selection_change *out);
// the live owner of the slot, null when empty or the owner died
void x12_selection_owner(int primary, struct x12_conn **owner,
                         uint32_t *source_id);
uint32_t x12_selection_generation(int primary);

// the full change reaction: cancelled for a replaced owner, finished for
// every offer pinned to the old generation, the new generation to every
// bound data device
void x12_selection_after_change(const struct x12_selection_change *chg,
                                int primary, struct x12_conn *claimer,
                                uint32_t claimed_source);
// a dying owner drops both slots without a cancelled event, it cannot
// hear one anyway
void x12_selection_release_owner(struct x12_conn *c);

#endif

// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#ifndef X12_OUTPUT_H
#define X12_OUTPUT_H

#include <stdint.h>

#include "conn.h"

// the output global: one screen description and its presentation
// feedback. every client that binds the global hears every present, the
// commit token tells it which of its own frames reached the screen

// the screen slots the server drives, slot indexes are the output
// identities the protocol instance numbers name. they never renumber,
// a down head leaves a hole instead of shifting its neighbours
#define X12_OUTPUT_SLOTS 8

struct x12_output_desc {
    char name[32];
    int32_t x, y;
    uint32_t w, h;
    uint32_t mm_w, mm_h;
    uint32_t refresh_millihz;
    int live;
};

// startup and hotplug description of one screen slot
void x12_output_set(int slot, const struct x12_output_desc *d);
void x12_output_drop(int slot);
int x12_output_describe(int slot, struct x12_output_desc *d);
int x12_output_count(void);

// the registry bind and the teardown path
void x12_output_bind(struct x12_conn *c, uint32_t id);
void x12_output_unbind(struct x12_conn *c);

// presentation feedback: the token names the commit that reached the
// screen, kind is the x12_output present_kind wire value and ns is the
// scanout moment the display picked the frame up
void x12_output_present(uint64_t token, uint32_t kind, uint64_t ns);

#endif

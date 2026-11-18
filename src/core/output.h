// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#ifndef X12_OUTPUT_H
#define X12_OUTPUT_H

#include <stdint.h>

#include "conn.h"

// the output global: one screen description and its presentation
// feedback. every client that binds the global hears every present, the
// commit token tells it which of its own frames reached the screen.
// one instance per physical output, the instance number names the
// screen slot and never renumbers

// the screen slots the server drives, slot indexes are the output
// identities the protocol instance numbers name. a down head leaves a
// hole instead of shifting its neighbours
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

// the registry bind and the teardown paths: release frees one binding,
// unbind drops everything a dying connection held. bind refuses when
// the table is full so the client hears about it
int x12_output_bind(struct x12_conn *c, uint32_t id, int slot);
void x12_output_release(struct x12_conn *c, uint32_t id);
void x12_output_unbind(struct x12_conn *c);

// presentation feedback: the token names the commit that reached the
// screen, kind is the x12_output present_kind wire value and ns is the
// scanout moment the display picked the frame up. the slot picks the
// output whose bindings hear it
void x12_output_present(int slot, uint64_t token, uint32_t kind, uint64_t ns);

// which screen slot a bound output object names, -1 when the object
// holds no output binding
int x12_output_slot_of(struct x12_conn *c, uint32_t id);

// the description burst (mode, geometry, scale, name) for one binding
// or for every binding of one output, binds and hotplug returns ride
// it. returns -1 when the connection refused the events
int x12_output_send_desc(struct x12_conn *c, uint32_t id, int slot);
void x12_output_refresh(int slot);

// hotplug and vrr state changes for the bindings of one output
void x12_output_removed(int slot);
void x12_output_vrr(int slot, uint32_t enabled, uint32_t min_millihz,
                    uint32_t max_millihz);

#endif

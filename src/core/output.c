// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#include "output.h"

#include <string.h>

#include "x12_protocol.h"

static struct {
    struct x12_conn *c;
    uint32_t id;
} bindings[X12_MAX_CLIENTS];

static struct x12_output_desc slots[X12_OUTPUT_SLOTS];

void x12_output_set(int slot, const struct x12_output_desc *d) {
    if (slot < 0 || slot >= X12_OUTPUT_SLOTS || !d) {
        return;
    }
    slots[slot] = *d;
    slots[slot].live = 1;
}

void x12_output_drop(int slot) {
    if (slot < 0 || slot >= X12_OUTPUT_SLOTS) {
        return;
    }
    slots[slot].live = 0;
}

int x12_output_describe(int slot, struct x12_output_desc *d) {
    if (slot < 0 || slot >= X12_OUTPUT_SLOTS || !slots[slot].live || !d) {
        return 0;
    }
    *d = slots[slot];
    return 1;
}

int x12_output_count(void) {
    int n = 0;
    for (int i = 0; i < X12_OUTPUT_SLOTS; i++) {
        if (slots[i].live) {
            n++;
        }
    }
    return n;
}

void x12_output_bind(struct x12_conn *c, uint32_t id) {
    for (size_t i = 0; i < X12_MAX_CLIENTS; i++) {
        if (bindings[i].c == c && bindings[i].id == id) {
            return;
        }
    }
    for (size_t i = 0; i < X12_MAX_CLIENTS; i++) {
        if (bindings[i].c == 0) {
            bindings[i].c = c;
            bindings[i].id = id;
            return;
        }
    }
    // no slot left: the binding hears nothing, the client sees silence
    // instead of a kill
}

void x12_output_unbind(struct x12_conn *c) {
    for (size_t i = 0; i < X12_MAX_CLIENTS; i++) {
        if (bindings[i].c == c) {
            bindings[i].c = 0;
            bindings[i].id = 0;
        }
    }
}

void x12_output_present(uint64_t token, uint32_t kind, uint64_t ns) {
    const x12_iface_desc *out = x12_iface_find("x12_output");
    x12_arg a[2] = { { .u64 = token }, { .u64 = kind } };
    for (size_t i = 0; i < X12_MAX_CLIENTS; i++) {
        if (!bindings[i].c) {
            continue;
        }
        if (bindings[i].c->dead) {
            bindings[i].c = 0;
            bindings[i].id = 0;
            continue;
        }
        x12_conn_event_ts(bindings[i].c, out, "present", bindings[i].id, a, 2,
                          ns);
    }
}

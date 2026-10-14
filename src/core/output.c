// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#include "output.h"

#include <string.h>

#include "x12_protocol.h"

static struct {
    struct x12_conn *c;
    uint32_t id;
    int slot;
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
    if (slot < 0 || slot >= X12_OUTPUT_SLOTS || !slots[slot].live) {
        return 0;
    }
    // a null desc just probes whether the slot lives
    if (d) {
        *d = slots[slot];
    }
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

void x12_output_bind(struct x12_conn *c, uint32_t id, int slot) {
    for (size_t i = 0; i < X12_MAX_CLIENTS; i++) {
        if (bindings[i].c == c && bindings[i].id == id) {
            return;
        }
    }
    for (size_t i = 0; i < X12_MAX_CLIENTS; i++) {
        if (bindings[i].c == 0) {
            bindings[i].c = c;
            bindings[i].id = id;
            bindings[i].slot = slot;
            return;
        }
    }
    // no slot left: the binding hears nothing, the client sees silence
    // instead of a kill
}

int x12_output_slot_of(struct x12_conn *c, uint32_t id) {
    for (size_t i = 0; i < X12_MAX_CLIENTS; i++) {
        if (bindings[i].c == c && bindings[i].id == id) {
            return bindings[i].slot;
        }
    }
    return -1;
}

void x12_output_release(struct x12_conn *c, uint32_t id) {
    for (size_t i = 0; i < X12_MAX_CLIENTS; i++) {
        if (bindings[i].c == c && bindings[i].id == id) {
            bindings[i].c = 0;
            bindings[i].id = 0;
        }
    }
}

void x12_output_unbind(struct x12_conn *c) {
    for (size_t i = 0; i < X12_MAX_CLIENTS; i++) {
        if (bindings[i].c == c) {
            bindings[i].c = 0;
            bindings[i].id = 0;
        }
    }
}

// one event to every binding of one output
static void fan_out(int slot, const char *event, const x12_arg *a, int n_args,
                    uint64_t ns) {
    const x12_iface_desc *out = x12_iface_find("x12_output");
    for (size_t i = 0; i < X12_MAX_CLIENTS; i++) {
        if (!bindings[i].c || bindings[i].slot != slot) {
            continue;
        }
        if (bindings[i].c->dead) {
            bindings[i].c = 0;
            bindings[i].id = 0;
            continue;
        }
        x12_conn_event_ts(bindings[i].c, out, event, bindings[i].id, a, n_args,
                          ns);
    }
}

void x12_output_present(int slot, uint64_t token, uint32_t kind, uint64_t ns) {
    x12_arg a[2] = { { .u64 = token }, { .u64 = kind } };
    fan_out(slot, "present", a, 2, ns);
}

void x12_output_removed(int slot) {
    fan_out(slot, "removed", 0, 0, 0);
}

void x12_output_vrr(int slot, uint32_t enabled, uint32_t min_millihz,
                    uint32_t max_millihz) {
    x12_arg a[3] = {
        { .u64 = enabled },
        { .u64 = min_millihz },
        { .u64 = max_millihz },
    };
    fan_out(slot, "vrr", a, 3, 0);
}

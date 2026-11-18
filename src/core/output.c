// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#include "output.h"

#include <string.h>

#include "x12_protocol.h"

static struct {
    struct x12_conn *c;
    uint32_t id;
    int slot;
} bindings[X12_MAX_CLIENTS * X12_MAX_OBJECTS];

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

int x12_output_bind(struct x12_conn *c, uint32_t id, int slot) {
    for (size_t i = 0; i < sizeof bindings / sizeof bindings[0]; i++) {
        if (bindings[i].c == c && bindings[i].id == id) {
            return 0;
        }
    }
    for (size_t i = 0; i < sizeof bindings / sizeof bindings[0]; i++) {
        if (bindings[i].c == 0) {
            bindings[i].c = c;
            bindings[i].id = id;
            bindings[i].slot = slot;
            return 0;
        }
    }
    // the table covers the whole object budget, so this is a guard,
    // not a tripwire. refusing beats the old silence where a client
    // believed its output was bound while nobody would ever hear it
    return -1;
}

int x12_output_slot_of(struct x12_conn *c, uint32_t id) {
    for (size_t i = 0; i < sizeof bindings / sizeof bindings[0]; i++) {
        if (bindings[i].c == c && bindings[i].id == id) {
            return bindings[i].slot;
        }
    }
    return -1;
}

void x12_output_release(struct x12_conn *c, uint32_t id) {
    for (size_t i = 0; i < sizeof bindings / sizeof bindings[0]; i++) {
        if (bindings[i].c == c && bindings[i].id == id) {
            bindings[i].c = 0;
            bindings[i].id = 0;
        }
    }
}

void x12_output_unbind(struct x12_conn *c) {
    for (size_t i = 0; i < sizeof bindings / sizeof bindings[0]; i++) {
        if (bindings[i].c == c) {
            bindings[i].c = 0;
            bindings[i].id = 0;
        }
    }
}

int x12_output_send_desc(struct x12_conn *c, uint32_t id, int slot) {
    struct x12_output_desc d;
    if (!x12_output_describe(slot, &d)) {
        return 0;
    }
    const x12_iface_desc *oi = x12_iface_find("x12_output");
    // the one mode the screen runs is both current and preferred, the
    // enum can only say one thing so it names the truth a user picking
    // a mode cares about
    x12_arg m[4] = {
        { .u64 = d.w },
        { .u64 = d.h },
        { .u64 = d.refresh_millihz },
        { .u64 = X12_OUTPUT_MODE_FLAG_PREFERRED },
    };
    if (x12_conn_event(c, oi, "mode", id, m, 4) != 0) {
        return -1;
    }
    x12_arg g[4] = {
        { .i64 = d.x },
        { .i64 = d.y },
        { .u64 = d.mm_w },
        { .u64 = d.mm_h },
    };
    if (x12_conn_event(c, oi, "geometry", id, g, 4) != 0) {
        return -1;
    }
    x12_arg sc[1] = { { .i64 = 256 } };
    if (x12_conn_event(c, oi, "scale", id, sc, 1) != 0) {
        return -1;
    }
    x12_arg nm[1] = {
        { .str = d.name, .str_len = (uint32_t)strlen(d.name) },
    };
    return x12_conn_event(c, oi, "name", id, nm, 1);
}

void x12_output_refresh(int slot) {
    for (size_t i = 0; i < sizeof bindings / sizeof bindings[0]; i++) {
        if (bindings[i].c && bindings[i].slot == slot) {
            x12_output_send_desc(bindings[i].c, bindings[i].id, slot);
        }
    }
}

// one event to every binding of one output
static void fan_out(int slot, const char *event, const x12_arg *a, int n_args,
                    uint64_t ns) {
    const x12_iface_desc *out = x12_iface_find("x12_output");
    for (size_t i = 0; i < sizeof bindings / sizeof bindings[0]; i++) {
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

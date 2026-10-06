// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#include "output.h"

#include <string.h>

#include "x12_protocol.h"

static struct {
    struct x12_conn *c;
    uint32_t id;
} bindings[X12_MAX_CLIENTS];

static uint32_t info_w, info_h, info_refresh = 60000;
static char info_name[32] = "X12-1";

void x12_output_info(uint32_t w, uint32_t h, uint32_t refresh_millihz,
                     const char *name) {
    info_w = w;
    info_h = h;
    if (refresh_millihz) {
        info_refresh = refresh_millihz;
    }
    if (name && name[0]) {
        strncpy(info_name, name, sizeof info_name - 1);
        info_name[sizeof info_name - 1] = 0;
    }
}

void x12_output_describe(uint32_t *w, uint32_t *h, uint32_t *refresh_millihz,
                         const char **name) {
    *w = info_w;
    *h = info_h;
    *refresh_millihz = info_refresh;
    *name = info_name;
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

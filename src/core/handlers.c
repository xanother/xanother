// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#include "handlers.h"

#include <string.h>

#include "x12_protocol.h"

// bind rules: which global names the server advertises and which
// capability it takes to bind them. the gate refuses loudly, a refused
// bind never leaves a stub object behind
struct bind_rule {
    const char *name;
    enum x12_obj_kind kind;
    uint32_t required_cap;  // x12_capability wire value, none means open
};

static const struct bind_rule bind_rules[] = {
    { "x12_compositor", OBJ_COMPOSITOR, X12_CAPABILITY_NONE },
    { "x12_buffer_manager", OBJ_BUFFER_MANAGER, X12_CAPABILITY_NONE },
    { "x12_seat", OBJ_SEAT, X12_CAPABILITY_SEAT },
    { "x12_screencopy", OBJ_SCREENCOPY, X12_CAPABILITY_SCREENCOPY },
};

static const struct bind_rule *rule_by_name(const char *name) {
    for (size_t i = 0; i < sizeof bind_rules / sizeof bind_rules[0]; i++) {
        if (strcmp(bind_rules[i].name, name) == 0) {
            return &bind_rules[i];
        }
    }
    return 0;
}

static int handle_hello(struct x12_conn *c, const x12_arg *args) {
    if (args[0].u64 != X12_IDL_FORMAT) {
        x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, X12_OBJ_ID_CONNECTION);
        return 0;
    }
    // session policy is a server decision, the skeleton grants seat and
    // output info but never screencopy
    uint32_t caps[2] = { X12_CAPABILITY_SEAT, X12_CAPABILITY_OUTPUT_INFO };
    c->caps = (1u << X12_CAPABILITY_SEAT) | (1u << X12_CAPABILITY_OUTPUT_INFO);
    x12_arg w[2] = {
        { .array = (const uint8_t *)caps, .array_len = 2 },
        { .u64 = X12_CORE_VERSION },
    };
    if (x12_conn_event(c, x12_iface_find("x12_connection"), "welcome",
                       X12_OBJ_ID_CONNECTION, w, 2) != 0) {
        return -1;
    }
    // the client learns the global set from registry.global events, one
    // per advertised instance
    const x12_iface_desc *reg = x12_iface_find("x12_registry");
    for (size_t i = 0; i < sizeof bind_rules / sizeof bind_rules[0]; i++) {
        const struct bind_rule *r = &bind_rules[i];
        x12_arg g[4] = {
            { .str = r->name, .str_len = (uint32_t)strlen(r->name) },
            { .u64 = x12_iface_find(r->name)->version },
            { .u64 = r->required_cap },
            { .u64 = 0 },
        };
        if (x12_conn_event(c, reg, "global", X12_OBJ_ID_REGISTRY, g, 4) != 0) {
            return -1;
        }
    }
    return 0;
}

static int handle_sync(struct x12_conn *c, uint32_t object, const x12_arg *args) {
    uint32_t id = (uint32_t)args[0].u64;
    if (x12_objects_alloc(&c->objects, id, OBJ_CALLBACK,
                          x12_iface_find("x12_callback")) != 0) {
        x12_conn_error(c, X12_ERROR_CODE_INVALID_ID, object);
        return 0;
    }
    // the one shot continuation closes with a fresh serial
    x12_arg d[1] = { { .u64 = x12_conn_next_serial(c) } };
    return x12_conn_event(c, x12_iface_find("x12_callback"), "done", id, d, 1);
}

int x12_handle_connection(struct x12_conn *c, struct x12_object *o,
                          const x12_msg_desc *msg, const x12_arg *args) {
    (void)o;
    if (strcmp(msg->name, "hello") == 0) {
        return handle_hello(c, args);
    }
    if (strcmp(msg->name, "sync") == 0) {
        return handle_sync(c, X12_OBJ_ID_CONNECTION, args);
    }
    if (strcmp(msg->name, "shutdown") == 0) {
        x12_conn_kill(c, "client shutdown");
        return -1;
    }
    x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, X12_OBJ_ID_CONNECTION);
    return 0;
}

static int handle_bind(struct x12_conn *c, const x12_arg *args) {
    if (args[0].str_len >= 64) {
        x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, X12_OBJ_ID_REGISTRY);
        return 0;
    }
    char name[64];
    memcpy(name, args[0].str, args[0].str_len);
    name[args[0].str_len] = 0;

    const struct bind_rule *rule = rule_by_name(name);
    if (!rule) {
        x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, X12_OBJ_ID_REGISTRY);
        return 0;
    }
    // capability gate, refusal is loud and the id stays unused
    if (rule->required_cap != X12_CAPABILITY_NONE &&
        !(c->caps & (1u << rule->required_cap))) {
        x12_conn_error(c, X12_ERROR_CODE_NO_CAPABILITY, X12_OBJ_ID_REGISTRY);
        return 0;
    }
    // only repeated globals care about the instance selector, the
    // skeleton advertises singletons only
    if (args[3].u64 != 0) {
        x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, X12_OBJ_ID_REGISTRY);
        return 0;
    }
    uint32_t id = (uint32_t)args[2].u64;
    if (x12_objects_alloc(&c->objects, id, rule->kind,
                          x12_iface_find(name)) != 0) {
        x12_conn_error(c, X12_ERROR_CODE_INVALID_ID, X12_OBJ_ID_REGISTRY);
        return 0;
    }
    return 0;
}

int x12_handle_registry(struct x12_conn *c, struct x12_object *o,
                        const x12_msg_desc *msg, const x12_arg *args) {
    (void)o;
    if (strcmp(msg->name, "bind") == 0) {
        return handle_bind(c, args);
    }
    x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, X12_OBJ_ID_REGISTRY);
    return 0;
}

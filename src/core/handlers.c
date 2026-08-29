// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#include "handlers.h"

#include <string.h>
#include <unistd.h>

#include "x12_protocol.h"
#include "buffer.h"
#include "frame.h"

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

static int handle_hello(struct x12_conn *c, uint32_t object, const x12_arg *args) {
    if (args[0].u64 != X12_IDL_FORMAT) {
        x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
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

int x12_handle_connection(struct x12_conn *c, uint32_t object,
                          const x12_msg_desc *msg, const x12_arg *args) {
    if (strcmp(msg->name, "hello") == 0) {
        return handle_hello(c, object, args);
    }
    if (strcmp(msg->name, "sync") == 0) {
        return handle_sync(c, object, args);
    }
    if (strcmp(msg->name, "shutdown") == 0) {
        x12_conn_kill(c, "client shutdown");
        return -1;
    }
    x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
    return 0;
}

static int handle_bind(struct x12_conn *c, uint32_t object, const x12_arg *args) {
    if (args[0].str_len >= 64) {
        x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
        return 0;
    }
    char name[64];
    memcpy(name, args[0].str, args[0].str_len);
    name[args[0].str_len] = 0;

    const struct bind_rule *rule = rule_by_name(name);
    if (!rule) {
        x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
        return 0;
    }
    // capability gate, refusal is loud and the id stays unused
    if (rule->required_cap != X12_CAPABILITY_NONE &&
        !(c->caps & (1u << rule->required_cap))) {
        x12_conn_error(c, X12_ERROR_CODE_NO_CAPABILITY, object);
        return 0;
    }
    // only repeated globals care about the instance selector, the
    // skeleton advertises singletons only
    if (args[3].u64 != 0) {
        x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
        return 0;
    }
    uint32_t id = (uint32_t)args[2].u64;
    if (x12_objects_alloc(&c->objects, id, rule->kind,
                          x12_iface_find(name)) != 0) {
        x12_conn_error(c, X12_ERROR_CODE_INVALID_ID, object);
        return 0;
    }
    // a fresh buffer manager announces the formats it composites on
    if (rule->kind == OBJ_BUFFER_MANAGER) {
        const x12_iface_desc *bm = x12_iface_find("x12_buffer_manager");
        uint32_t formats[2] = { X12_FORMAT_XRGB8888, X12_FORMAT_ARGB8888 };
        for (size_t i = 0; i < 2; i++) {
            x12_arg f[1] = { { .u64 = formats[i] } };
            if (x12_conn_event(c, bm, "format", id, f, 1) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

int x12_handle_registry(struct x12_conn *c, uint32_t object,
                        const x12_msg_desc *msg, const x12_arg *args) {
    if (strcmp(msg->name, "bind") == 0) {
        return handle_bind(c, object, args);
    }
    x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
    return 0;
}

int x12_handle_compositor(struct x12_conn *c, uint32_t object,
                          const x12_msg_desc *msg, const x12_arg *args) {
    if (strcmp(msg->name, "create_surface") == 0) {
        uint32_t id = (uint32_t)args[0].u64;
        if (x12_objects_alloc(&c->objects, id, OBJ_SURFACE,
                              x12_iface_find("x12_surface")) != 0) {
            x12_conn_error(c, X12_ERROR_CODE_INVALID_ID, object);
            return 0;
        }
        struct x12_object *o = x12_objects_get(&c->objects, id);
        x12_frame_init(&o->as.surface);
        return 0;
    }
    // roles and regions land with their own parties
    x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
    return 0;
}

int x12_handle_buffer_manager(struct x12_conn *c, uint32_t object,
                              const x12_msg_desc *msg, const x12_arg *args) {
    if (strcmp(msg->name, "create_shm_pool") == 0) {
        int fd = -1;
        if (x12_conn_take_fd(c, (uint32_t)args[0].u64, &fd) != 0) {
            x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
            return 0;
        }
        int rc = x12_pool_create(c, (uint32_t)args[2].u64, fd,
                                 (uint32_t)args[1].u64);
        if (rc != 0) {
            x12_conn_error(c, (uint32_t)rc, object);
        }
        return 0;
    }
    if (strcmp(msg->name, "import_dmabuf") == 0) {
        // the dmabuf path rides with the gpu backend, not here
        x12_conn_error(c, X12_ERROR_CODE_IMPLEMENTATION, object);
        return 0;
    }
    x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
    return 0;
}

int x12_handle_shm_pool(struct x12_conn *c, uint32_t object,
                        const x12_msg_desc *msg, const x12_arg *args) {
    if (strcmp(msg->name, "create_buffer") == 0) {
        int rc = x12_buffer_create(c, object, (uint32_t)args[5].u64,
                                   (uint32_t)args[0].u64, (uint32_t)args[1].u64,
                                   (uint32_t)args[2].u64, (uint32_t)args[3].u64,
                                   (uint32_t)args[4].u64);
        if (rc != 0) {
            x12_conn_error(c, (uint32_t)rc, object);
        }
        return 0;
    }
    if (strcmp(msg->name, "destroy") == 0) {
        x12_pool_destroy(c, object);
        return 0;
    }
    if (strcmp(msg->name, "resize") == 0) {
        int rc = x12_pool_resize(c, object, (uint32_t)args[0].u64);
        if (rc != 0) {
            x12_conn_error(c, (uint32_t)rc, object);
        }
        return 0;
    }
    x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
    return 0;
}

int x12_handle_buffer(struct x12_conn *c, uint32_t object,
                      const x12_msg_desc *msg, const x12_arg *args) {
    (void)args;
    if (strcmp(msg->name, "destroy") == 0) {
        x12_buffer_destroy(c, object);
        return 0;
    }
    x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
    return 0;
}

int x12_handle_surface(struct x12_conn *c, uint32_t object,
                       const x12_msg_desc *msg, const x12_arg *args) {
    struct x12_object *o = x12_objects_get(&c->objects, object);
    struct x12_surface_state *s = &o->as.surface;
    if (strcmp(msg->name, "destroy") == 0) {
        x12_frame_destroy(c, s);
        x12_objects_free(&c->objects, object);
        return 0;
    }
    if (strcmp(msg->name, "set_buffer") == 0) {
        if (x12_frame_set_buffer(c, s, (uint32_t)args[0].u64) != 0) {
            x12_conn_error(c, X12_ERROR_CODE_INVALID_ID, object);
        }
        return 0;
    }
    if (strcmp(msg->name, "set_transform") == 0) {
        s->pending_transform = (uint32_t)args[0].u64;
        return 0;
    }
    if (strcmp(msg->name, "set_scale") == 0) {
        s->pending_scale = (uint32_t)args[0].i64;
        return 0;
    }
    if (strcmp(msg->name, "set_sync") == 0) {
        s->pending_sync = (uint32_t)args[0].u64;
        return 0;
    }
    if (strcmp(msg->name, "damage") == 0) {
        x12_frame_damage(s, (int32_t)args[0].i64, (int32_t)args[1].i64,
                         (uint32_t)args[2].i64, (uint32_t)args[3].i64);
        return 0;
    }
    if (strcmp(msg->name, "frame") == 0) {
        uint32_t id = (uint32_t)args[0].u64;
        if (x12_objects_alloc(&c->objects, id, OBJ_CALLBACK,
                              x12_iface_find("x12_callback")) != 0) {
            x12_conn_error(c, X12_ERROR_CODE_INVALID_ID, object);
            return 0;
        }
        if (x12_frame_add_cb(s, id) != 0) {
            x12_objects_free(&c->objects, id);
            x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
        }
        return 0;
    }
    if (strcmp(msg->name, "commit") == 0) {
        if (x12_frame_commit(c, object, s) != 0) {
            return -1;
        }
        x12_frame_drain(c, s);
        return 0;
    }
    if (strcmp(msg->name, "set_commit_fence") == 0) {
        int fd = -1;
        if (x12_conn_take_fd(c, (uint32_t)args[0].u64, &fd) != 0) {
            x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
            return 0;
        }
        x12_frame_set_fence(c, s, fd);
        return 0;
    }
    x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
    return 0;
}

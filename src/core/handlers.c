// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#include "handlers.h"

#include <string.h>
#include <unistd.h>

#include "x12_protocol.h"
#include "buffer.h"
#include "frame.h"
#include "compositor.h"
#include "windows.h"

static void apply_role_after_commit(struct x12_conn *c, uint32_t surface_id,
                                    struct x12_surface_state *s);
static void dismiss_children(struct x12_conn *c, uint32_t surface_id);
static void surface_logical(const struct x12_surface_state *s, uint32_t *w,
                            uint32_t *h);
static void send_role_configure(struct x12_conn *c, uint32_t role_id,
                                int is_popup, uint32_t w, uint32_t h,
                                int fullscreen, uint32_t *last_config);

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
    if (strcmp(msg->name, "create_region") == 0) {
        // regions land with their own party
        x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
        return 0;
    }
    if (strcmp(msg->name, "create_positioner") == 0) {
        uint32_t id = (uint32_t)args[0].u64;
        if (x12_objects_alloc(&c->objects, id, OBJ_POSITIONER,
                              x12_iface_find("x12_positioner")) != 0) {
            x12_conn_error(c, X12_ERROR_CODE_INVALID_ID, object);
        }
        return 0;
    }
    if (strcmp(msg->name, "create_toplevel") == 0) {
        uint32_t id = (uint32_t)args[0].u64;
        uint32_t surf_id = (uint32_t)args[1].u64;
        struct x12_object *surf = x12_objects_get(&c->objects, surf_id);
        // one role per surface, a second create on the same surface is
        // refused rather than silently replacing the first role
        if (!surf || surf->kind != OBJ_SURFACE ||
            surf->as.surface.role_kind != ROLE_NONE ||
            x12_objects_alloc(&c->objects, id, OBJ_TOPLEVEL,
                              x12_iface_find("x12_toplevel")) != 0) {
            x12_conn_error(c, X12_ERROR_CODE_INVALID_ID, object);
            return 0;
        }
        surf->as.surface.role_kind = ROLE_TOPLEVEL;
        surf->as.surface.role_id = id;
        struct x12_object *o = x12_objects_get(&c->objects, id);
        struct x12_toplevel_state *t = &o->as.toplevel;
        t->surface_id = surf_id;
        // the role opens with an offer carrying the committed size
        uint32_t lw, lh;
        surface_logical(&surf->as.surface, &lw, &lh);
        send_role_configure(c, id, 0, lw, lh, 0, &t->last_config);
        return 0;
    }
    if (strcmp(msg->name, "create_popup") == 0) {
        uint32_t id = (uint32_t)args[0].u64;
        uint32_t surf_id = (uint32_t)args[1].u64;
        uint32_t parent_id = (uint32_t)args[2].u64;
        uint32_t pos_id = (uint32_t)args[3].u64;
        struct x12_object *surf = x12_objects_get(&c->objects, surf_id);
        struct x12_object *parent = x12_objects_get(&c->objects, parent_id);
        struct x12_object *pos = x12_objects_get(&c->objects, pos_id);
        // the parent must carry the toplevel role, popups hang off
        // windows and never off other popups
        if (!surf || surf->kind != OBJ_SURFACE ||
            surf->as.surface.role_kind != ROLE_NONE ||
            !parent || parent->kind != OBJ_SURFACE ||
            parent->as.surface.role_kind != ROLE_TOPLEVEL ||
            !pos || pos->kind != OBJ_POSITIONER ||
            x12_objects_alloc(&c->objects, id, OBJ_POPUP,
                              x12_iface_find("x12_popup")) != 0) {
            x12_conn_error(c, X12_ERROR_CODE_INVALID_ID, object);
            return 0;
        }
        surf->as.surface.role_kind = ROLE_POPUP;
        surf->as.surface.role_id = id;
        struct x12_object *o = x12_objects_get(&c->objects, id);
        struct x12_popup_state *pp = &o->as.popup;
        pp->surface_id = surf_id;
        pp->parent_id = parent_id;
        // the positioner values are copied, the slot stays behind
        pp->rect_x = pos->as.positioner.rect_x;
        pp->rect_y = pos->as.positioner.rect_y;
        pp->rect_w = pos->as.positioner.rect_w;
        pp->rect_h = pos->as.positioner.rect_h;
        pp->anchor = pos->as.positioner.anchor;
        pp->gravity = pos->as.positioner.gravity;
        pp->off_x = pos->as.positioner.off_x;
        pp->off_y = pos->as.positioner.off_y;
        uint32_t lw, lh;
        surface_logical(&surf->as.surface, &lw, &lh);
        send_role_configure(c, id, 1, lw, lh, 0, &pp->last_config);
        return 0;
    }
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
        // the window leaves the stack with its surface, a role object
        // pointing here goes inert and its popups never outlive it
        int view = x12_windows_find(c, object);
        if (view >= 0) {
            x12_windows_remove(view);
        }
        dismiss_children(c, object);
        if (s->role_kind != ROLE_NONE) {
            struct x12_object *ro = x12_objects_get(&c->objects, s->role_id);
            if (ro && ro->kind == OBJ_TOPLEVEL) {
                ro->as.toplevel.surface_id = 0;
            }
            if (ro && ro->kind == OBJ_POPUP) {
                ro->as.popup.surface_id = 0;
            }
        }
        x12_objects_free(&c->objects, object);
        // the pixels it showed are gone, the area needs a repaint
        x12_comp_full_damage();
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
        // the role machine maps, resizes or unmaps the window, its view
        // stack change drives the repaint
        apply_role_after_commit(c, object, s);
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

// the logical size a surface occupies, the buffer carries scale times
// it; a zero scale cannot reach here, set_scale refuses it
static void surface_logical(const struct x12_surface_state *s, uint32_t *w,
                            uint32_t *h) {
    uint32_t sc = s->current_scale ? s->current_scale : 256;
    *w = (s->current.width * 256u + sc / 2) / sc;
    *h = (s->current.height * 256u + sc / 2) / sc;
}

// sends a configure for a window role, the serial ties the offer to the
// client ack_configure. the toplevel event carries a states array, the
// popup event does not
static void send_role_configure(struct x12_conn *c, uint32_t role_id,
                                int is_popup, uint32_t w, uint32_t h,
                                int fullscreen, uint32_t *last_config) {
    uint32_t serial = ++c->serial;
    if (is_popup) {
        x12_arg args[3] = {
            { .u64 = w }, { .u64 = h }, { .u64 = serial },
        };
        if (x12_conn_event(c, x12_iface_find("x12_popup"), "configure",
                           role_id, args, 3) == 0) {
            *last_config = serial;
        }
        return;
    }
    // window_state values follow the idl order, fullscreen is the third
    uint32_t states[1] = { 2 };
    x12_arg args[4] = {
        { .u64 = w }, { .u64 = h },
        { .array = fullscreen ? (const uint8_t *)states : 0,
          .array_len = fullscreen ? 1u : 0u },
        { .u64 = serial },
    };
    if (x12_conn_event(c, x12_iface_find("x12_toplevel"), "configure",
                       role_id, args, 4) == 0) {
        *last_config = serial;
    }
}

// a popup never outlives its parent on screen: when the parent leaves
// the stack, mapped children go with it and hear dismiss
static void dismiss_children(struct x12_conn *c, uint32_t surface_id) {
    for (uint32_t id = 1; id < X12_MAX_OBJECTS; id++) {
        struct x12_object *o = x12_objects_get(&c->objects, id);
        if (!o || o->kind != OBJ_POPUP) {
            continue;
        }
        struct x12_popup_state *pp = &o->as.popup;
        if (pp->parent_id != surface_id || pp->surface_id == 0) {
            continue;
        }
        int view = x12_windows_find(c, pp->surface_id);
        if (view >= 0) {
            x12_windows_remove(view);
        }
        x12_conn_event(c, x12_iface_find("x12_popup"), "dismiss", id, 0, 0);
    }
}

// role state machine after a commit: a roled surface maps only when its
// latest configure was acknowledged, a roleless surface is never
// composited
static void apply_role_after_commit(struct x12_conn *c, uint32_t surface_id,
                                    struct x12_surface_state *s) {
    if (s->role_kind == ROLE_TOPLEVEL) {
        struct x12_object *ro = x12_objects_get(&c->objects, s->role_id);
        if (!ro || ro->kind != OBJ_TOPLEVEL) {
            return;
        }
        struct x12_toplevel_state *t = &ro->as.toplevel;
        int view = x12_windows_find(c, surface_id);
        if (!s->has_current) {
            if (view >= 0) {
                x12_windows_remove(view);
                dismiss_children(c, surface_id);
            }
            return;
        }
        uint32_t lw, lh;
        surface_logical(s, &lw, &lh);
        if (t->acked_config == t->last_config) {
            if (view < 0) {
                int32_t x, y;
                // a fullscreen map skips the cascade, the view pins the
                // output origin
                if (t->fullscreen) {
                    x = 0;
                    y = 0;
                } else {
                    x12_windows_place_toplevel(&x, &y);
                }
                x12_windows_add(c, surface_id, x, y, lw, lh, 0);
            } else {
                const struct x12_winview *v = x12_windows_at(view);
                x12_windows_update(view, v->x, v->y, lw, lh);
                // the pin survives every accepted geometry, a plain
                // commit must not park a fullscreen view back anywhere
                if (t->fullscreen) {
                    x12_windows_place_fullscreen(view, 1);
                }
            }
        } else {
            // client committed without acknowledging the offer, the
            // server reoffers the deal; a fullscreen toplevel is
            // reoffered the whole mode, not the stale committed size
            uint32_t ow = lw, oh = lh;
            if (t->fullscreen) {
                x12_windows_output(&ow, &oh);
            }
            send_role_configure(c, s->role_id, 0, ow, oh, t->fullscreen,
                                &t->last_config);
        }
        return;
    }
    if (s->role_kind == ROLE_POPUP) {
        struct x12_object *ro = x12_objects_get(&c->objects, s->role_id);
        if (!ro || ro->kind != OBJ_POPUP) {
            return;
        }
        struct x12_popup_state *pp = &ro->as.popup;
        int view = x12_windows_find(c, surface_id);
        if (!s->has_current) {
            if (view >= 0) {
                x12_windows_remove(view);
            }
            return;
        }
        uint32_t lw, lh;
        surface_logical(s, &lw, &lh);
        if (pp->acked_config != pp->last_config) {
            send_role_configure(c, s->role_id, 1, lw, lh, 0,
                                &pp->last_config);
            return;
        }
        int parent_view = x12_windows_find(c, pp->parent_id);
        if (parent_view < 0) {
            // a popup never outlives its parent on screen
            return;
        }
        int32_t px, py;
        x12_windows_place_popup(pp->rect_x, pp->rect_y, pp->rect_w,
                                pp->rect_h, pp->anchor, pp->gravity,
                                pp->off_x, pp->off_y, lw, lh,
                                x12_windows_at(parent_view), &px, &py);
        if (view < 0) {
            x12_windows_add(c, surface_id, px, py, lw, lh, 1);
        } else {
            x12_windows_update(view, px, py, lw, lh);
        }
    }
}

int x12_handle_positioner(struct x12_conn *c, uint32_t object,
                          const x12_msg_desc *msg, const x12_arg *args) {
    struct x12_object *o = x12_objects_get(&c->objects, object);
    struct x12_positioner_state *p = &o->as.positioner;
    if (strcmp(msg->name, "set_anchor_rect") == 0) {
        p->rect_x = (int32_t)args[0].i64;
        p->rect_y = (int32_t)args[1].i64;
        p->rect_w = (uint32_t)args[2].u64;
        p->rect_h = (uint32_t)args[3].u64;
        return 0;
    }
    if (strcmp(msg->name, "set_anchor") == 0) {
        p->anchor = (uint32_t)args[0].u64;
        return 0;
    }
    if (strcmp(msg->name, "set_gravity") == 0) {
        p->gravity = (uint32_t)args[0].u64;
        return 0;
    }
    if (strcmp(msg->name, "set_offset") == 0) {
        p->off_x = (int32_t)args[0].i64;
        p->off_y = (int32_t)args[1].i64;
        return 0;
    }
    x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
    return 0;
}

int x12_handle_toplevel(struct x12_conn *c, uint32_t object,
                        const x12_msg_desc *msg, const x12_arg *args) {
    struct x12_object *o = x12_objects_get(&c->objects, object);
    struct x12_toplevel_state *t = &o->as.toplevel;
    if (strcmp(msg->name, "ack_configure") == 0) {
        t->acked_config = (uint32_t)args[0].u64;
        return 0;
    }
    if (strcmp(msg->name, "set_title") == 0) {
        uint32_t n = args[0].str_len;
        if (n >= sizeof t->title) {
            n = (uint32_t)sizeof t->title - 1;
        }
        memcpy(t->title, args[0].str, n);
        t->title[n] = 0;
        return 0;
    }
    if (strcmp(msg->name, "set_app_id") == 0) {
        uint32_t n = args[0].str_len;
        if (n >= sizeof t->app_id) {
            n = (uint32_t)sizeof t->app_id - 1;
        }
        memcpy(t->app_id, args[0].str, n);
        t->app_id[n] = 0;
        return 0;
    }
    if (strcmp(msg->name, "set_min_size") == 0) {
        t->min_w = (uint32_t)args[0].u64;
        t->min_h = (uint32_t)args[1].u64;
        return 0;
    }
    if (strcmp(msg->name, "set_max_size") == 0) {
        t->max_w = (uint32_t)args[0].u64;
        t->max_h = (uint32_t)args[1].u64;
        return 0;
    }
    if (strcmp(msg->name, "set_fullscreen") == 0) {
        uint32_t state = (uint32_t)args[0].u64;
        if (state > 1) {
            x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
            return 0;
        }
        t->fullscreen = (int)state;
        uint32_t ow, oh;
        int view = t->surface_id ? x12_windows_find(c, t->surface_id) : -1;
        if (view >= 0) {
            // a mapped view is offered the size it must take: the whole
            // mode entering fullscreen, the natural size leaving it
            const struct x12_winview *v = x12_windows_at(view);
            if (state) {
                x12_windows_output(&ow, &oh);
            } else {
                ow = v->nat_w;
                oh = v->nat_h;
            }
            x12_windows_place_fullscreen(view, (int)state);
        } else {
            // an unmapped role is offered the committed buffer size
            // unless fullscreen demands the mode
            ow = oh = 0;
            struct x12_object *surf =
                t->surface_id ? x12_objects_get(&c->objects, t->surface_id)
                              : 0;
            if (surf && surf->kind == OBJ_SURFACE) {
                surface_logical(&surf->as.surface, &ow, &oh);
            }
            if (state) {
                x12_windows_output(&ow, &oh);
            }
        }
        send_role_configure(c, object, 0, ow, oh, (int)state,
                            &t->last_config);
        return 0;
    }
    if (strcmp(msg->name, "destroy") == 0) {
        if (t->surface_id) {
            struct x12_object *surf =
                x12_objects_get(&c->objects, t->surface_id);
            if (surf && surf->kind == OBJ_SURFACE &&
                surf->as.surface.role_id == object) {
                surf->as.surface.role_kind = ROLE_NONE;
                surf->as.surface.role_id = 0;
            }
            int view = x12_windows_find(c, t->surface_id);
            if (view >= 0) {
                x12_windows_remove(view);
            }
            dismiss_children(c, t->surface_id);
        }
        x12_objects_free(&c->objects, object);
        return 0;
    }
    x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
    return 0;
}

int x12_handle_popup(struct x12_conn *c, uint32_t object,
                     const x12_msg_desc *msg, const x12_arg *args) {
    struct x12_object *o = x12_objects_get(&c->objects, object);
    struct x12_popup_state *pp = &o->as.popup;
    if (strcmp(msg->name, "grab") == 0) {
        // arming is what enables outside press dismissal. the serial
        // must be a button press this connection actually received, a
        // grab cannot ride on an invented serial. input serials do not
        // exist yet, so nothing can arm until input lands
        x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
        return 0;
    }
    if (strcmp(msg->name, "ack_configure") == 0) {
        pp->acked_config = (uint32_t)args[0].u64;
        return 0;
    }
    if (strcmp(msg->name, "destroy") == 0) {
        if (pp->surface_id) {
            struct x12_object *surf =
                x12_objects_get(&c->objects, pp->surface_id);
            if (surf && surf->kind == OBJ_SURFACE &&
                surf->as.surface.role_id == object) {
                surf->as.surface.role_kind = ROLE_NONE;
                surf->as.surface.role_id = 0;
            }
            int view = x12_windows_find(c, pp->surface_id);
            if (view >= 0) {
                x12_windows_remove(view);
            }
        }
        x12_objects_free(&c->objects, object);
        return 0;
    }
    x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
    return 0;
}

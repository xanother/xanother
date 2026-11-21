// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#include "handlers.h"

#include "backend.h"
#include "output.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "x12_protocol.h"
#include "buffer.h"
#include "frame.h"
#include "compositor.h"
#include "windows.h"
#include "input.h"
#include "selection.h"

#include <sys/mman.h>
#include <xkbcommon/xkbcommon.h>

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
    { "x12_output", OBJ_OUTPUT, X12_CAPABILITY_OUTPUT_INFO },
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
    // the token keeps this session findable after a disconnect
    uint64_t token = x12_conn_mint_session(c);
    x12_arg st[1] = { { .u64 = token } };
    if (x12_conn_event(c, x12_iface_find("x12_connection"), "session",
                       X12_OBJ_ID_CONNECTION, st, 1) != 0) {
        return -1;
    }
    // the client learns the global set from registry.global events, one
    // per advertised instance
    for (size_t i = 0; i < sizeof bind_rules / sizeof bind_rules[0]; i++) {
        const struct bind_rule *r = &bind_rules[i];
        if (r->kind == OBJ_OUTPUT) {
            for (int s = 0; s < X12_OUTPUT_SLOTS; s++) {
                if (!x12_output_describe(s, 0)) {
                    continue;
                }
                if (x12_announce_output(c, (uint32_t)s) != 0) {
                    return -1;
                }
            }
            continue;
        }
        const x12_iface_desc *reg = x12_iface_find("x12_registry");
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

// one registry.global for one output instance, hello sends the whole
// set and hotplug adds late arrivals the same way
int x12_announce_output(struct x12_conn *c, uint32_t instance) {
    const struct bind_rule *rule = rule_by_name("x12_output");
    const x12_iface_desc *reg = x12_iface_find("x12_registry");
    x12_arg g[4] = {
        { .str = "x12_output", .str_len = 10 },
        { .u64 = x12_iface_find("x12_output")->version },
        { .u64 = rule->required_cap },
        { .u64 = instance },
    };
    return x12_conn_event(c, reg, "global", X12_OBJ_ID_REGISTRY, g, 4);
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
    int rc = x12_conn_event(c, x12_iface_find("x12_callback"), "done", id, d, 1);
    // the callback is one shot and its interface takes no requests:
    // the slot goes back right away, a table of 32 ids would otherwise
    // fill up with dead continuations and strand the connection
    x12_objects_free(&c->objects, id);
    return rc;
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
    if (strcmp(msg->name, "resume") == 0) {
        struct x12_conn *p = x12_conn_resume(c, args[0].u64);
        if (!p) {
            x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
            return 0;
        }
        // the parked session took this transport over, its token comes
        // back so the client knows which one to keep. the rest of the
        // batch follows into the session, this conn is done
        uint64_t token = p->resume_token;
        x12_arg st[1] = { { .u64 = token } };
        x12_conn_event(p, x12_iface_find("x12_connection"), "session",
                       X12_OBJ_ID_CONNECTION, st, 1);
        x12_conn_kill(c, "session resumed");
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
    // singletons refuse anything above zero and the outputs name the
    // screen slot they describe
    uint32_t instance = (uint32_t)args[3].u64;
    if (rule->kind == OBJ_OUTPUT) {
        if (args[3].u64 >= X12_OUTPUT_SLOTS ||
            !x12_output_describe((int)instance, 0)) {
            x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
            return 0;
        }
    } else if (instance != 0) {
        x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
        return 0;
    }
    uint32_t id = (uint32_t)args[2].u64;
    if (x12_objects_alloc(&c->objects, id, rule->kind,
                          x12_iface_find(name)) != 0) {
        x12_conn_error(c, X12_ERROR_CODE_INVALID_ID, object);
        return 0;
    }
    // a fresh output announces its screen description and joins the
    // presentation feedback fan out
    if (rule->kind == OBJ_OUTPUT) {
        if (x12_output_bind(c, id, (int)instance) != 0) {
            x12_conn_error(c, X12_ERROR_CODE_OUT_OF_MEMORY, object);
            x12_objects_free(&c->objects, id);
            return 0;
        }
        return x12_output_send_desc(c, id, (int)instance);
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
    // a fresh seat announces what it serves: one bit per device_class
    // value, pointer and keyboard, touch is wired protocol without a
    // runtime so the bit stays off until get_touch stops refusing
    if (rule->kind == OBJ_SEAT) {
        const x12_iface_desc *is = x12_iface_find("x12_seat");
        x12_arg cap[1] = { { .u64 = (1u << 0) | (1u << 1) } };
        if (x12_conn_event(c, is, "capabilities", id, cap, 1) != 0) {
            return -1;
        }
        const char *name = "x12-seat";
        x12_arg nm[1] = {
            { .str = name, .str_len = (uint32_t)strlen(name) },
        };
        if (x12_conn_event(c, is, "name", id, nm, 1) != 0) {
            return -1;
        }
    }
    return 0;
}

int x12_handle_output(struct x12_conn *c, uint32_t object,
                      const x12_msg_desc *msg, const x12_arg *args) {
    if (strcmp(msg->name, "release") == 0) {
        x12_output_release(c, object);
        x12_objects_free(&c->objects, object);
        return 0;
    }
    if (strcmp(msg->name, "set_vrr") == 0) {
        // anything above 1 is refused with bad-value, hosts without vrr
        // support refuse the same way
        uint32_t enable = (uint32_t)args[0].u64;
        if (enable > 1) {
            x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
            return 0;
        }
        int slot = x12_output_slot_of(c, object);
        uint32_t min_millihz = 0, max_millihz = 0;
        if (slot < 0 || !x12_backend->output_vrr ||
            !x12_backend->output_vrr(slot, &min_millihz, &max_millihz) ||
            !x12_backend->set_vrr ||
            x12_backend->set_vrr(slot, (int)enable) != 0) {
            x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
            return 0;
        }
        x12_output_vrr(slot, enable, min_millihz, max_millihz);
        return 0;
    }
    x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
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
    if (strcmp(msg->name, "create_net_pool") == 0) {
        int rc = x12_pool_create_net(c, (uint32_t)args[1].u64,
                                     (uint32_t)args[0].u64);
        if (rc != 0) {
            x12_conn_error(c, (uint32_t)rc, object);
        }
        return 0;
    }
    if (strcmp(msg->name, "import_dmabuf") == 0) {
        int fd = -1;
        if (x12_conn_take_fd(c, (uint32_t)args[0].u64, &fd) != 0) {
            x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
            return 0;
        }
        int rc = x12_buffer_import(c, (uint32_t)args[6].u64, fd,
                                   (uint32_t)args[1].u64,
                                   (uint32_t)args[2].u64,
                                   (uint32_t)args[3].u64, args[4].u64,
                                   (uint32_t)args[5].u64);
        if (rc != 0) {
            x12_conn_error(c, (uint32_t)rc, object);
        }
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
    if (strcmp(msg->name, "write") == 0) {
        int rc = x12_pool_write(c, object, (uint32_t)args[0].u64,
                                (const uint8_t *)args[1].str,
                                args[1].str_len);
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

// server keyboard policy: the real compiled keymap and the repeat
// parameters clients self apply, the server never re-sends key events
#define X12_KEY_REPEAT_RATE 25
#define X12_KEY_REPEAT_DELAY 600

// compiles the default us keymap through libxkbcommon and hands it out
// as a read only memfd, the client receives the exact bytes a real
// keyboard model would ship
static int make_keymap_fd(uint32_t *size_out) {
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx) {
        return -1;
    }
    struct xkb_rule_names names = { .rules = "evdev", .model = "pc105",
                                    .layout = "us" };
    struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, &names,
                                                      XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km) {
        xkb_context_unref(ctx);
        return -1;
    }
    const char *text = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
    int fd = -1;
    size_t len = text ? strlen(text) : 0;
    if (len > 0) {
        fd = memfd_create("x12-keymap", MFD_CLOEXEC);
        if (fd >= 0) {
            if (ftruncate(fd, (off_t)len) == 0 &&
                write(fd, text, len) == (ssize_t)len) {
                // the file offset is shared with the receiver
                lseek(fd, 0, SEEK_SET);
                *size_out = (uint32_t)len;
            } else {
                close(fd);
                fd = -1;
            }
        }
    }
    // xkb hands the text over as a fresh allocation
    free((void *)text);
    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
    return fd;
}

// keymap args follow the idl order: format, fd slot, size
static void announce_keyboard(struct x12_conn *c, uint32_t kb_id) {
    uint32_t ksize = 0;
    int kfd = make_keymap_fd(&ksize);
    if (kfd >= 0) {
        uint32_t fmt = 0;
        (void)x12_input_enum("keymap_format", "xkb-v1", &fmt);
        x12_arg kargs[3] = { { .u64 = fmt }, { .u64 = 0 }, { .u64 = ksize } };
        x12_conn_event_fd(c, x12_iface_find("x12_keyboard"), "keymap", kb_id,
                          kargs, 3, 1, kfd);
    }
    x12_arg rargs[2] = {
        { .u64 = X12_KEY_REPEAT_RATE }, { .u64 = X12_KEY_REPEAT_DELAY },
    };
    x12_conn_event(c, x12_iface_find("x12_keyboard"), "repeat_info", kb_id,
                   rargs, 2);
}

// the logical size a surface occupies, the buffer carries scale times
// it; a zero scale cannot reach here, set_scale refuses it
static void surface_logical(const struct x12_surface_state *s, uint32_t *w,
                            uint32_t *h) {
    uint32_t sc = s->current_scale ? s->current_scale : 256;
    // widened first: a hostile buffer size times the scale fraction
    // wrapped the 32 bit math and produced garbage geometry. the result
    // is clamped to a size no screen could ever need
    uint64_t lw = ((uint64_t)s->current.width * 256u + sc / 2) / sc;
    uint64_t lh = ((uint64_t)s->current.height * 256u + sc / 2) / sc;
    *w = lw > (1u << 24) ? (1u << 24) : (uint32_t)lw;
    *h = lh > (1u << 24) ? (1u << 24) : (uint32_t)lh;
}

// the screen a fullscreen window takes: a fresh map lands on the
// output whose mode matches the surface size, a mapped view stays on
// the output it already covers. the client never positions windows,
// the server is the only one who knows where they are. returns 0 when
// no output lives at all
static int pick_output(int32_t x, int32_t y, uint32_t w, uint32_t h,
                       int prefer_size, struct x12_out_info *out) {
    int best = -1;
    int64_t best_overlap = 0;
    for (int i = 0; i < X12_OUTPUT_SLOTS; i++) {
        struct x12_out_info oi;
        if (!x12_backend->output_info(i, &oi)) {
            continue;
        }
        if (best < 0) {
            best = i;
            *out = oi;
        }
        if (prefer_size && oi.width == w && oi.height == h) {
            *out = oi;
            return 1;
        }
        int64_t x0 = x > oi.x ? x : oi.x;
        int64_t y0 = y > oi.y ? y : oi.y;
        int64_t x1 = x + (int64_t)w;
        int64_t y1 = y + (int64_t)h;
        if (x1 > oi.x + (int64_t)oi.width) {
            x1 = oi.x + (int64_t)oi.width;
        }
        if (y1 > oi.y + (int64_t)oi.height) {
            y1 = oi.y + (int64_t)oi.height;
        }
        int64_t ov = x1 > x0 && y1 > y0 ? (x1 - x0) * (y1 - y0) : 0;
        if (ov > best_overlap) {
            best_overlap = ov;
            best = i;
            *out = oi;
        }
    }
    // a size pick that found no exact match is a miss, the caller
    // falls back to the whole canvas: the rootful surface covers every
    // head and must not pin to one of them
    return prefer_size ? 0 : best >= 0;
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
                // a fullscreen map skips the cascade, the view pins
                // its output origin
                if (t->fullscreen) {
                    struct x12_out_info oi;
                    if (pick_output(0, 0, lw, lh, 1, &oi)) {
                        x = oi.x;
                        y = oi.y;
                    } else {
                        x = 0;
                        y = 0;
                    }
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
                    const struct x12_winview *pv = x12_windows_at(view);
                    struct x12_out_info oi;
                    if (pick_output(pv->x, pv->y, pv->width, pv->height, 0,
                                    &oi)) {
                        x12_windows_place_fullscreen(view, 1, oi.x, oi.y);
                    }
                }
            }
        } else {
            // client committed without acknowledging the offer, the
            // server reoffers the deal; a fullscreen toplevel is
            // reoffered the whole mode, not the stale committed size
            uint32_t ow = lw, oh = lh;
            if (t->fullscreen) {
                struct x12_out_info oi;
                if (pick_output(0, 0, lw, lh, 1, &oi)) {
                    ow = oi.width;
                    oh = oi.height;
                } else {
                    x12_windows_output(&ow, &oh);
                }
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

// a hard length clip can land in the middle of a utf8 sequence and
// hand the compositor a garbled title, so back off to a char boundary
static uint32_t
clip_utf8(const char *s, uint32_t n)
{
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) {
        n--;
    }
    return n;
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
            // the source is longer than the clip point, so the byte
            // under n is inside the string
            n = clip_utf8(args[0].str, (uint32_t)sizeof t->title - 1);
        }
        memcpy(t->title, args[0].str, n);
        t->title[n] = 0;
        return 0;
    }
    if (strcmp(msg->name, "set_app_id") == 0) {
        uint32_t n = args[0].str_len;
        if (n >= sizeof t->app_id) {
            n = clip_utf8(args[0].str, (uint32_t)sizeof t->app_id - 1);
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
            struct x12_out_info oi;
            if (state && pick_output(v->x, v->y, v->width, v->height, 1,
                                     &oi)) {
                ow = oi.width;
                oh = oi.height;
                x12_windows_place_fullscreen(view, 1, oi.x, oi.y);
            } else if (state) {
                x12_windows_output(&ow, &oh);
                x12_windows_place_fullscreen(view, 1, 0, 0);
            } else {
                ow = v->nat_w;
                oh = v->nat_h;
                x12_windows_place_fullscreen(view, 0, 0, 0);
            }
        } else {
            // an unmapped role is offered the committed buffer size
            // unless fullscreen demands a mode
            ow = oh = 0;
            struct x12_object *surf =
                t->surface_id ? x12_objects_get(&c->objects, t->surface_id)
                              : 0;
            if (surf && surf->kind == OBJ_SURFACE) {
                surface_logical(&surf->as.surface, &ow, &oh);
            }
            if (state) {
                struct x12_out_info oi;
                if (pick_output(0, 0, ow, oh, 1, &oi)) {
                    ow = oi.width;
                    oh = oi.height;
                } else {
                    x12_windows_output(&ow, &oh);
                }
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
        // grab cannot ride on an invented serial
        if (!x12_input_serial_valid(c, (uint32_t)args[1].u64)) {
            x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
        } else {
            x12_input_set_grab(c, pp->surface_id);
        }
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

int x12_handle_seat(struct x12_conn *c, uint32_t object,
                    const x12_msg_desc *msg, const x12_arg *args) {
    // one pointer and one keyboard per client for now, a second get_*
    // returns a refusal rather than silently replacing the id
    if (strcmp(msg->name, "get_pointer") == 0) {
        uint32_t id = (uint32_t)args[0].u64;
        if (c->pointer_id ||
            x12_objects_alloc(&c->objects, id, OBJ_POINTER,
                              x12_iface_find("x12_pointer")) != 0) {
            x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
        } else {
            c->pointer_id = id;
        }
        return 0;
    }
    if (strcmp(msg->name, "get_keyboard") == 0) {
        uint32_t id = (uint32_t)args[0].u64;
        if (c->keyboard_id ||
            x12_objects_alloc(&c->objects, id, OBJ_KEYBOARD,
                              x12_iface_find("x12_keyboard")) != 0) {
            x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
        } else {
            c->keyboard_id = id;
            announce_keyboard(c, id);
        }
        return 0;
    }
    if (strcmp(msg->name, "get_touch") == 0) {
        // touch is wired protocol without a runtime yet, a loud
        // refusal beats a silent stub
        x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
        return 0;
    }
    if (strcmp(msg->name, "get_data_device") == 0) {
        uint32_t id = (uint32_t)args[0].u64;
        if (c->data_device_id ||
            x12_objects_alloc(&c->objects, id, OBJ_DATA_DEVICE,
                              x12_iface_find("x12_data_device")) != 0) {
            x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
        } else {
            c->data_device_id = id;
            // the startup signals name the generation the device is
            // joining, 0 means the connection saw no selection yet
            const x12_iface_desc *dd = x12_iface_find("x12_data_device");
            x12_arg gs[1] = { { .u64 = x12_selection_generation(0) } };
            x12_arg gp[1] = { { .u64 = x12_selection_generation(1) } };
            if (x12_conn_event(c, dd, "selection", id, gs, 1) != 0 ||
                x12_conn_event(c, dd, "primary", id, gp, 1) != 0) {
                return 0;
            }
        }
        return 0;
    }
    if (strcmp(msg->name, "release") == 0) {
        x12_objects_free(&c->objects, object);
        return 0;
    }
    x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
    return 0;
}

int x12_handle_pointer(struct x12_conn *c, uint32_t object,
                       const x12_msg_desc *msg, const x12_arg *args) {
    if (strcmp(msg->name, "set_cursor") == 0) {
        uint32_t surf = (uint32_t)args[1].u64;
        if (surf == 0) {
            // a null surface hides the sprite
            c->cursor_surface_id = 0;
            x12_windows_force_dirty();
            return 0;
        }
        struct x12_object *o = x12_objects_get(&c->objects, surf);
        if (!o || o->kind != OBJ_SURFACE) {
            x12_conn_error(c, X12_ERROR_CODE_INVALID_ID, object);
            return 0;
        }
        if (o->as.surface.role_kind != ROLE_NONE) {
            // a cursor rides a roleless surface, roled ones are
            // windows in the stack already
            x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
            return 0;
        }
        c->cursor_surface_id = surf;
        c->cursor_hot_x = (int32_t)args[2].i64;
        c->cursor_hot_y = (int32_t)args[3].i64;
        x12_windows_force_dirty();
        return 0;
    }
    if (strcmp(msg->name, "release") == 0) {
        if (c->pointer_id == object) {
            c->pointer_id = 0;
        }
        x12_objects_free(&c->objects, object);
        return 0;
    }
    x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
    return 0;
}

int x12_handle_keyboard(struct x12_conn *c, uint32_t object,
                        const x12_msg_desc *msg, const x12_arg *args) {
    (void)args;
    if (strcmp(msg->name, "release") == 0) {
        if (c->keyboard_id == object) {
            c->keyboard_id = 0;
        }
        x12_objects_free(&c->objects, object);
        return 0;
    }
    x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
    return 0;
}

// the two selection slots the data device addresses, 0 is the clipboard
// and 1 the primary
static void selection_set(struct x12_conn *c, uint32_t device, int primary,
                          uint32_t source) {
    if (source == 0) {
        struct x12_selection_change chg;
        x12_selection_clear(primary, &chg);
        x12_selection_after_change(&chg, primary, c, 0);
        return;
    }
    struct x12_object *o = x12_objects_get(&c->objects, source);
    if (!o || o->kind != OBJ_DATA_SOURCE) {
        x12_conn_error(c, X12_ERROR_CODE_INVALID_ID, device);
        return;
    }
    struct x12_selection_change chg;
    x12_selection_claim(primary, c, source, &chg);
    x12_selection_after_change(&chg, primary, c, source);
}

// the pull side: a fresh offer object replaying the owner mime list
static void selection_get(struct x12_conn *c, uint32_t device, int primary,
                          uint32_t id) {
    struct x12_conn *owner;
    uint32_t source_id;
    x12_selection_owner(primary, &owner, &source_id);
    if (!owner) {
        // the idl refusal: no offer object into an empty selection
        x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, device);
        return;
    }
    if (x12_objects_alloc(&c->objects, id, OBJ_DATA_OFFER,
                          x12_iface_find("x12_data_offer")) != 0) {
        x12_conn_error(c, X12_ERROR_CODE_INVALID_ID, device);
        return;
    }
    struct x12_object *o = x12_objects_get(&c->objects, id);
    o->as.data_offer.primary = primary;
    o->as.data_offer.generation = x12_selection_generation(primary);
    struct x12_object *src = x12_objects_get(&owner->objects, source_id);
    const x12_iface_desc *io = x12_iface_find("x12_data_offer");
    for (uint32_t i = 0; src && i < src->as.data_source.n_mimes; i++) {
        x12_arg m[1] = {
            { .str = src->as.data_source.mimes[i],
              .str_len = (uint32_t)strlen(src->as.data_source.mimes[i]) },
        };
        if (x12_conn_event(c, io, "offer", id, m, 1) != 0) {
            return;
        }
    }
}

int x12_handle_data_device(struct x12_conn *c, uint32_t object,
                           const x12_msg_desc *msg, const x12_arg *args) {
    if (strcmp(msg->name, "set_selection") == 0) {
        selection_set(c, object, 0, (uint32_t)args[0].u64);
        return 0;
    }
    if (strcmp(msg->name, "set_primary") == 0) {
        selection_set(c, object, 1, (uint32_t)args[0].u64);
        return 0;
    }
    if (strcmp(msg->name, "get_selection") == 0) {
        selection_get(c, object, 0, (uint32_t)args[0].u64);
        return 0;
    }
    if (strcmp(msg->name, "get_primary") == 0) {
        selection_get(c, object, 1, (uint32_t)args[0].u64);
        return 0;
    }
    if (strcmp(msg->name, "create_source") == 0) {
        if (x12_objects_alloc(&c->objects, (uint32_t)args[0].u64,
                              OBJ_DATA_SOURCE,
                              x12_iface_find("x12_data_source")) != 0) {
            x12_conn_error(c, X12_ERROR_CODE_INVALID_ID, object);
        }
        return 0;
    }
    if (strcmp(msg->name, "release") == 0) {
        if (c->data_device_id == object) {
            c->data_device_id = 0;
        }
        x12_objects_free(&c->objects, object);
        return 0;
    }
    x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
    return 0;
}

// wire strings are not nul terminated: the comparison runs over the
// declared length, a strcmp would walk past the end of the frame
static int str_eq(const char *nul_term, const x12_arg *a) {
    return strlen(nul_term) == a->str_len &&
           memcmp(nul_term, a->str, a->str_len) == 0;
}

int x12_handle_data_source(struct x12_conn *c, uint32_t object,
                           const x12_msg_desc *msg, const x12_arg *args) {
    struct x12_object *o = x12_objects_get(&c->objects, object);
    if (strcmp(msg->name, "offer") == 0) {
        uint32_t len = args[0].str_len;
        struct x12_data_source_state *src = &o->as.data_source;
        if (!len || len >= sizeof src->mimes[0]) {
            x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
            return 0;
        }
        int dup = 0;
        for (uint32_t i = 0; i < src->n_mimes; i++) {
            if (str_eq(src->mimes[i], &args[0])) {
                dup = 1;
                break;
            }
        }
        // the fixed cap turns extra mimes into a loud refusal, a silent
        // drop would promise the reader nothing while the list says
        // otherwise
        if (!dup && src->n_mimes >= 8) {
            x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
            return 0;
        }
        if (!dup) {
            memcpy(src->mimes[src->n_mimes], args[0].str, len);
            src->mimes[src->n_mimes][len] = 0;
            src->n_mimes++;
        }
        return 0;
    }
    if (strcmp(msg->name, "destroy") == 0) {
        // a source going away drops any slot it owned. the re-claim
        // guard sees the very same source and skips the cancelled event
        for (int primary = 0; primary < 2; primary++) {
            struct x12_conn *owner;
            uint32_t sid;
            x12_selection_owner(primary, &owner, &sid);
            if (owner == c && sid == object) {
                struct x12_selection_change chg;
                x12_selection_clear(primary, &chg);
                x12_selection_after_change(&chg, primary, c, object);
            }
        }
        x12_objects_free(&c->objects, object);
        return 0;
    }
    x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
    return 0;
}

int x12_handle_data_offer(struct x12_conn *c, uint32_t object,
                          const x12_msg_desc *msg, const x12_arg *args) {
    struct x12_object *o = x12_objects_get(&c->objects, object);
    if (strcmp(msg->name, "receive") == 0) {
        // the fd is consumed first: a refused request must not leave its
        // descriptor parked, the next one would name the wrong slot
        int fd;
        if (x12_conn_take_fd(c, (uint32_t)args[1].u64, &fd) != 0) {
            x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
            return 0;
        }
        struct x12_conn *owner;
        uint32_t sid;
        x12_selection_owner(o->as.data_offer.primary, &owner, &sid);
        struct x12_object *src =
            owner ? x12_objects_get(&owner->objects, sid) : 0;
        int known = 0;
        for (uint32_t i = 0; src && i < src->as.data_source.n_mimes; i++) {
            if (str_eq(src->as.data_source.mimes[i], &args[0])) {
                known = 1;
                break;
            }
        }
        if (!owner || !known || o->as.data_offer.generation !=
                                    x12_selection_generation(
                                        o->as.data_offer.primary)) {
            // stale or empty, the offer is finished already
            close(fd);
            x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
            return 0;
        }
        // the write end rides to the owner through scm rights, the
        // reader keeps the other end of its own pipe; the flush closes
        // our copy, the kernel holds the delivered one
        x12_arg sargs[2] = {
            { .str = args[0].str, .str_len = args[0].str_len },
            { .u64 = 0 },
        };
        if (x12_conn_event_fd(owner, x12_iface_find("x12_data_source"),
                              "send", sid, sargs, 2, 1, fd) != 0) {
            x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
        }
        return 0;
    }
    if (strcmp(msg->name, "destroy") == 0) {
        x12_objects_free(&c->objects, object);
        return 0;
    }
    x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
    return 0;
}

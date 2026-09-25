// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#include "frame.h"

#include <poll.h>
#include <unistd.h>

#include "x12_protocol.h"

// a sync_file fd signals readiness through POLLIN, a missing fence is
// always ready so clients without explicit sync never wait
static int fence_ready(int fd) {
    if (fd < 0) {
        return 1;
    }
    struct pollfd p = { fd, POLLIN, 0 };
    return poll(&p, 1, 0) > 0 && (p.revents & POLLIN) != 0;
}

void x12_frame_init(struct x12_surface_state *s) {
    s->pending_scale = X12_FIXED24P8_ONE;
    s->current_scale = X12_FIXED24P8_ONE;
    s->pending_fence = -1;
    s->fence = -1;
}

int x12_frame_set_buffer(struct x12_conn *c, struct x12_surface_state *s,
                         uint32_t buffer_id) {
    if (buffer_id == 0) {
        s->pending.object = 0;
        s->pending.pixels = 0;
        s->has_pending = 1;
        return 0;
    }
    struct x12_object *b = x12_objects_get(&c->objects, buffer_id);
    if (!b || b->kind != OBJ_BUFFER) {
        return X12_ERROR_CODE_INVALID_ID;
    }
    s->pending.object = buffer_id;
    s->pending.width = b->as.buffer.width;
    s->pending.height = b->as.buffer.height;
    s->pending.stride = b->as.buffer.stride;
    s->pending.format = b->as.buffer.format;
    s->pending.pixels = b->as.buffer.pixels;
    s->has_pending = 1;
    return 0;
}

void x12_frame_damage(struct x12_surface_state *s, int32_t x, int32_t y,
                      uint32_t w, uint32_t h) {
    // damage merges into what is already pending for this commit, an
    // overflow degrades to the whole surface so nothing is missed
    if (s->n_pending_damage < 0) {
        return;
    }
    if (s->n_pending_damage >= X12_MAX_DAMAGE) {
        s->n_pending_damage = -1;
        return;
    }
    struct x12_rect *r = &s->pending_damage[s->n_pending_damage++];
    r->x = x;
    r->y = y;
    r->w = w;
    r->h = h;
}

int x12_frame_set_fence(struct x12_conn *c, struct x12_surface_state *s,
                        int fence) {
    (void)c;
    if (s->pending_fence >= 0) {
        close(s->pending_fence);
    }
    s->pending_fence = fence;
    return 0;
}

int x12_frame_add_cb(struct x12_surface_state *s, uint32_t cb_id) {
    if (s->n_pending_cbs >= X12_MAX_CBS) {
        return -1;
    }
    s->pending_cbs[s->n_pending_cbs++] = (uint32_t)cb_id;
    return 0;
}

// the frame leaving the current slot enters the retire queue, its release
// credit names the buffer that frame committed with and its callbacks die
// with it. one entry per committed frame keeps the pairing exact
static int retire_current(struct x12_conn *c, struct x12_surface_state *s) {
    if (s->n_retire >= X12_MAX_RETIRE) {
        // the client commits faster than its fences signal, the backlog
        // would hide a dead fence forever
        x12_conn_kill(c, "retire backlog");
        return -1;
    }
    struct x12_retire *e = &s->retire[s->n_retire++];
    e->buffer_id = s->current.object;
    e->fence = s->fence;
    s->fence = -1;
    e->n_cbs = s->n_current_cbs;
    for (uint8_t i = 0; i < s->n_current_cbs; i++) {
        e->cbs[i] = s->current_cbs[i];
    }
    s->n_current_cbs = 0;
    return 0;
}

int x12_frame_commit(struct x12_conn *c, uint32_t surface_id,
                     struct x12_surface_state *s) {
    // the previous frame retires first, its credits leave through the
    // queue in commit order and the damage it accumulated goes with it
    if (s->has_frame) {
        if (retire_current(c, s) != 0) {
            return -1;
        }
    }
    s->has_frame = 1;
    // the buffer slot is sticky: a commit re-applies it, only
    // set_buffer(null) empties it and the next commit unmaps. a bare
    // commit after a buffer commit therefore keeps the pixels, that is
    // the damage-only repaint loop clients live on
    if (s->has_pending) {
        s->current = s->pending;
        s->has_current = s->current.object != 0;
        s->current_scale = s->pending_scale;
        s->current_transform = s->pending_transform;
        s->current_sync = s->pending_sync;
        // damage accumulates toward the compositor pass, a full rect
        // stays full across commits
        if (s->n_current_damage < 0 || s->n_pending_damage < 0) {
            s->n_current_damage = -1;
        } else {
            for (int i = 0; i < s->n_pending_damage; i++) {
                if (s->n_current_damage >= X12_MAX_DAMAGE) {
                    s->n_current_damage = -1;
                    break;
                }
                s->current_damage[s->n_current_damage++] = s->pending_damage[i];
            }
        }
    } else {
        // nothing ever sat in the slot, the surface never had pixels
        s->has_current = 0;
    }
    s->fence = s->pending_fence;
    s->pending_fence = -1;
    for (uint8_t i = 0; i < s->n_pending_cbs; i++) {
        s->current_cbs[i] = s->pending_cbs[i];
    }
    s->n_current_cbs = s->n_pending_cbs;
    s->n_pending_cbs = 0;
    s->n_pending_damage = 0;

    // the token comes back as committed so present can name this exact
    // commit instead of a fresh serial
    uint64_t token = x12_conn_next_token(c);
    x12_arg t[1] = { { .u64 = token } };
    return x12_conn_event(c, x12_iface_find("x12_surface"), "committed",
                          surface_id, t, 1);
}

static void emit_release(struct x12_conn *c, uint32_t buffer_id) {
    // the credit goes to the buffer that earned it, a destroyed buffer
    // object gets nothing
    struct x12_object *b = x12_objects_get(&c->objects, buffer_id);
    if (!b || b->kind != OBJ_BUFFER) {
        return;
    }
    x12_conn_event(c, x12_iface_find("x12_buffer"), "release", buffer_id, 0, 0);
}

static void retire_free_cbs(struct x12_conn *c, struct x12_retire *e,
                            int silent) {
    for (uint8_t i = 0; i < e->n_cbs; i++) {
        struct x12_object *o = x12_objects_get(&c->objects, e->cbs[i]);
        if (o && o->kind == OBJ_CALLBACK) {
            if (!silent) {
                x12_arg d[1] = { { .u64 = x12_conn_next_serial(c) } };
                x12_conn_event(c, o->iface, "done", e->cbs[i], d, 1);
            }
            x12_objects_free(&c->objects, e->cbs[i]);
        }
    }
}

void x12_frame_drain(struct x12_conn *c, struct x12_surface_state *s) {
    while (s->n_retire > 0) {
        struct x12_retire *e = &s->retire[0];
        if (!fence_ready(e->fence)) {
            // the front frame owns the order, later credits wait behind it
            return;
        }
        if (e->buffer_id) {
            emit_release(c, e->buffer_id);
        }
        retire_free_cbs(c, e, 0);
        if (e->fence >= 0) {
            close(e->fence);
        }
        for (uint8_t i = 1; i < s->n_retire; i++) {
            s->retire[i - 1] = s->retire[i];
        }
        s->n_retire--;
    }
}

void x12_frame_drain_client(struct x12_conn *c) {
    for (uint32_t id = 1; id < X12_MAX_OBJECTS; id++) {
        struct x12_object *o = x12_objects_get(&c->objects, id);
        if (o && o->kind == OBJ_SURFACE) {
            x12_frame_drain(c, &o->as.surface);
        }
    }
}

static void drain_force(struct x12_conn *c, struct x12_surface_state *s,
                        int silent) {
    // every entry leaves at once, fences do not get to hold the queue
    for (uint8_t i = 0; i < s->n_retire; i++) {
        struct x12_retire *e = &s->retire[i];
        if (e->buffer_id) {
            emit_release(c, e->buffer_id);
        }
        retire_free_cbs(c, e, silent);
        if (e->fence >= 0) {
            close(e->fence);
        }
    }
    s->n_retire = 0;
}

void x12_frame_destroy(struct x12_conn *c, struct x12_surface_state *s) {
    // the frame on screen retires too, its buffer comes back with the
    // rest. its callbacks die silently, no frame will complete for them
    if (s->n_current_cbs > 0) {
        for (uint8_t i = 0; i < s->n_current_cbs; i++) {
            struct x12_object *o = x12_objects_get(&c->objects, s->current_cbs[i]);
            if (o && o->kind == OBJ_CALLBACK) {
                x12_objects_free(&c->objects, s->current_cbs[i]);
            }
        }
        s->n_current_cbs = 0;
    }
    if (s->has_frame && retire_current(c, s) == 0) {
        s->retire[s->n_retire - 1].n_cbs = 0;
    }
    drain_force(c, s, 1);
    if (s->fence >= 0) {
        close(s->fence);
        s->fence = -1;
    }
    if (s->pending_fence >= 0) {
        close(s->pending_fence);
        s->pending_fence = -1;
    }
}

void x12_frame_drop_buffer(struct x12_conn *c, struct x12_surface_state *s,
                           uint32_t buffer_id) {
    (void)c;
    if (s->pending.object == buffer_id) {
        s->pending.object = 0;
        s->pending.pixels = 0;
    }
    if (s->current.object == buffer_id) {
        s->current.object = 0;
        s->current.pixels = 0;
        s->has_current = 0;
    }
}

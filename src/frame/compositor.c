// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#include "compositor.h"

#include <stdlib.h>

#include "backend.h"
#include "cpu.h"

// damage rect capacity of one pass, overflow means full repaint so a
// burst of small updates cannot drown the list
#define X12_FRAME_DAMAGE 16

static uint32_t *fb;
static uint32_t fb_w, fb_h;
static struct x12_rect frame_damage[X12_FRAME_DAMAGE];
static int n_frame_damage;
static int full_damage;

void x12_comp_set_size(uint32_t w, uint32_t h) {
    free(fb);
    fb = calloc((size_t)w * h, sizeof *fb);
    fb_w = w;
    fb_h = h;
    x12_comp_full_damage();
}

void x12_comp_full_damage(void) {
    full_damage = 1;
}

static void damage_add(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (full_damage) {
        return;
    }
    if (n_frame_damage >= X12_FRAME_DAMAGE) {
        full_damage = 1;
        return;
    }
    struct x12_rect *r = &frame_damage[n_frame_damage++];
    r->x = x;
    r->y = y;
    r->w = w;
    r->h = h;
}

// the logical rect of a surface on the canvas: roles drive the origin
// later, the reference pass anchors everything at the origin and takes
// the logical size from the buffer and its scale
static void surface_view(const struct x12_bufref *ref, uint32_t scale,
                         struct x12_view *view) {
    if (scale == 0) {
        scale = 256;
    }
    view->x = 0;
    view->y = 0;
    view->w = ref->width * 256 / scale;
    view->h = ref->height * 256 / scale;
}

int x12_comp_pass(struct x12_conn *conns, size_t n) {
    if (!fb) {
        return 0;
    }
    // collect: every surface gives up its damage to this pass, the
    // compositor is the only consumer so nothing accumulates twice
    n_frame_damage = 0;
    for (size_t i = 0; i < n; i++) {
        struct x12_conn *c = &conns[i];
        if (c->fd < 0) {
            continue;
        }
        for (uint32_t id = 1; id < X12_MAX_OBJECTS; id++) {
            struct x12_object *o = x12_objects_get(&c->objects, id);
            if (!o || o->kind != OBJ_SURFACE) {
                continue;
            }
            struct x12_surface_state *s = &o->as.surface;
            if (s->n_current_damage == 0) {
                continue;
            }
            if (!s->has_current) {
                // the surface shows nothing, unmap already forced the
                // repaint through full damage
                s->n_current_damage = 0;
                continue;
            }
            struct x12_view view;
            surface_view(&s->current, s->current_scale, &view);
            if (s->n_current_damage < 0) {
                damage_add(view.x, view.y, view.w, view.h);
            } else {
                for (int k = 0; k < s->n_current_damage; k++) {
                    const struct x12_rect *d = &s->current_damage[k];
                    // damage is in surface logical coordinates, the view
                    // origin maps it onto the canvas
                    damage_add(view.x + d->x, view.y + d->y, d->w, d->h);
                }
            }
            s->n_current_damage = 0;
        }
    }

    int repaint_all = full_damage;
    struct x12_rect clip;
    if (repaint_all) {
        clip.x = 0;
        clip.y = 0;
        clip.w = fb_w;
        clip.h = fb_h;
        damage_add(clip.x, clip.y, clip.w, clip.h);
    }
    if (n_frame_damage == 0) {
        full_damage = 0;
        return 0;
    }

    // repaint: background first, then every mapped surface over it in
    // the order they stack. z order policy lands with roles, creation
    // order stands in until then
    const struct x12_rect *rects = repaint_all ? &clip : frame_damage;
    int n_rects = repaint_all ? 1 : n_frame_damage;
    for (int k = 0; k < n_rects; k++) {
        x12_cpu_fill(fb, fb_w, fb_h, &rects[k], 0xFF000000u);
        for (size_t i = 0; i < n; i++) {
            struct x12_conn *c = &conns[i];
            if (c->fd < 0) {
                continue;
            }
            for (uint32_t id = 1; id < X12_MAX_OBJECTS; id++) {
                struct x12_object *o = x12_objects_get(&c->objects, id);
                if (!o || o->kind != OBJ_SURFACE || !o->as.surface.has_current) {
                    continue;
                }
                struct x12_surface_state *s = &o->as.surface;
                struct x12_view view;
                surface_view(&s->current, s->current_scale, &view);
                x12_cpu_blend_view(fb, fb_w, fb_h, &view, &s->current,
                                   s->current_scale, &rects[k]);
            }
        }
    }
    full_damage = 0;

    x12_backend.present(fb, rects, n_rects);
    return 1;
}

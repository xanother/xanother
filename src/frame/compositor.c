// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#include "compositor.h"

#include <stdlib.h>

#include "backend.h"
#include "cpu.h"
#include "windows.h"

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

// the logical rect of a surface on the canvas: the window stack owns
// the origin, the logical size comes from the buffer and its scale
static void surface_view(const struct x12_bufref *ref, uint32_t scale,
                         int32_t x, int32_t y, struct x12_view *view) {
    if (scale == 0) {
        scale = 256;
    }
    view->x = x;
    view->y = y;
    view->w = ref->width * 256 / scale;
    view->h = ref->height * 256 / scale;
}

int x12_comp_pass(struct x12_conn *conns, size_t n) {
    if (!fb) {
        return 0;
    }
    // a stack change moved windows without any client damage, the moved
    // areas need a repaint the damage accounting cannot see
    if (x12_windows_take_dirty()) {
        full_damage = 1;
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
            int vi = x12_windows_find(c, id);
            if (!s->has_current || vi < 0) {
                // nothing on screen for this surface, roleless never
                // composites and the map or unmap path already forced
                // any repaint the stack needed
                s->n_current_damage = 0;
                continue;
            }
            const struct x12_winview *v = x12_windows_at(vi);
            if (s->n_current_damage < 0) {
                damage_add(v->x, v->y, v->width, v->height);
            } else {
                for (int k = 0; k < s->n_current_damage; k++) {
                    const struct x12_rect *d = &s->current_damage[k];
                    // damage is in surface logical coordinates, the view
                    // origin maps it onto the canvas
                    damage_add(v->x + d->x, v->y + d->y, d->w, d->h);
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
    // nothing changed and nobody forced a repaint: the canvas is fine
    if (!repaint_all && n_frame_damage == 0) {
        return 0;
    }

    // repaint: background first, then the window stack bottom to top,
    // the array order is the z order the role machine maintains
    const struct x12_rect *rects = repaint_all ? &clip : frame_damage;
    int n_rects = repaint_all ? 1 : n_frame_damage;
    int nw = x12_windows_count();
    for (int k = 0; k < n_rects; k++) {
        x12_cpu_fill(fb, fb_w, fb_h, &rects[k], 0xFF000000u);
        for (int i = 0; i < nw; i++) {
            const struct x12_winview *v = x12_windows_at(i);
            struct x12_object *o =
                x12_objects_get(&v->client->objects, v->surface_id);
            if (!o || o->kind != OBJ_SURFACE || !o->as.surface.has_current) {
                continue;
            }
            struct x12_surface_state *s = &o->as.surface;
            struct x12_view view;
            surface_view(&s->current, s->current_scale, v->x, v->y, &view);
            x12_cpu_blend_view(fb, fb_w, fb_h, &view, &s->current,
                               s->current_scale, &rects[k]);
        }
    }
    full_damage = 0;

    x12_backend.present(fb, rects, n_rects);
    return 1;
}

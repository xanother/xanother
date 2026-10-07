// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#include "compositor.h"

#include <stdlib.h>
#include <string.h>

#include "backend.h"
#include "cpu.h"
#include "frame.h"
#include "gpu.h"
#include "windows.h"
#include "input.h"

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

// the pointer sprite: the hovered client's cursor surface drawn at the
// pointer minus the hotspot. backends that draw a host cursor get no
// sprite at all
static struct x12_view sprite_last;
static uint32_t sprite_last_sid;
static int sprite_drawn;

static int sprite_view(struct x12_view *view, struct x12_conn **owner,
                       uint32_t *sid) {
    if (x12_backend->host_cursor) {
        return 0;
    }
    int32_t px, py;
    struct x12_conn *cc;
    x12_input_pointer(&px, &py, &cc);
    if (!cc || !cc->cursor_surface_id) {
        return 0;
    }
    struct x12_object *o =
        x12_objects_get(&cc->objects, cc->cursor_surface_id);
    if (!o || o->kind != OBJ_SURFACE || !o->as.surface.has_current) {
        return 0;
    }
    surface_view(&o->as.surface.current, o->as.surface.current_scale,
                 px - cc->cursor_hot_x, py - cc->cursor_hot_y, view);
    *owner = cc;
    *sid = cc->cursor_surface_id;
    return 1;
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
    n_frame_damage = 0;
    // the sprite moves with the pointer and repaints with the frames,
    // the union of the old and the new rect is the cost of one move
    struct x12_view sview;
    struct x12_conn *sconn = 0;
    uint32_t ssid = 0;
    int have_sprite = sprite_view(&sview, &sconn, &ssid);
    if (have_sprite != sprite_drawn ||
        (have_sprite && (sview.x != sprite_last.x ||
                         sview.y != sprite_last.y ||
                         sview.w != sprite_last.w ||
                         sview.h != sprite_last.h ||
                         ssid != sprite_last_sid))) {
        if (sprite_drawn) {
            damage_add(sprite_last.x, sprite_last.y, sprite_last.w,
                       sprite_last.h);
        }
        if (have_sprite) {
            damage_add(sview.x, sview.y, sview.w, sview.h);
        }
    }
    // collect: every surface gives up its damage to this pass, the
    // compositor is the only consumer so nothing accumulates twice
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
                // the cursor sprite is roleless on purpose, its damage
                // maps onto the sprite rect instead of the window stack
                if (have_sprite && c == sconn && id == ssid) {
                    if (s->n_current_damage < 0) {
                        damage_add(sview.x, sview.y, sview.w, sview.h);
                    } else {
                        for (int k = 0; k < s->n_current_damage; k++) {
                            const struct x12_rect *d = &s->current_damage[k];
                            damage_add(sview.x + d->x, sview.y + d->y,
                                       d->w, d->h);
                        }
                    }
                }
                // nothing else on screen for this surface, roleless
                // never composites and the map or unmap path already
                // forced any repaint the stack needed
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
    // the array order is the z order the role machine maintains. the
    // gpu path walks the same order on its canvas and pulls the rows
    // the damage covers back into the cpu framebuffer afterwards, so
    // the rest of the pipeline sees the same bytes either way
    int gpu = x12_gpu_ready();
    const struct x12_rect *rects = repaint_all ? &clip : frame_damage;
    int n_rects = repaint_all ? 1 : n_frame_damage;
    int nw = x12_windows_count();
    for (int k = 0; k < n_rects; k++) {
        if (gpu) {
            x12_gpu_fill(&rects[k], 1);
        } else {
            x12_cpu_fill(fb, fb_w, fb_h, &rects[k], 0xFF000000u);
        }
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
            if (gpu) {
                x12_gpu_blend(&view, &s->current, s->current_scale,
                              &rects[k], 1);
            } else {
                x12_cpu_blend_view(fb, fb_w, fb_h, &view, &s->current,
                                   s->current_scale, &rects[k]);
            }
        }
        // the sprite rides on top of the whole stack
        if (have_sprite) {
            struct x12_object *o = x12_objects_get(&sconn->objects, ssid);
            if (o && o->kind == OBJ_SURFACE && o->as.surface.has_current) {
                if (gpu) {
                    x12_gpu_blend(&sview, &o->as.surface.current,
                                  o->as.surface.current_scale, &rects[k], 1);
                } else {
                    x12_cpu_blend_view(fb, fb_w, fb_h, &sview,
                                       &o->as.surface.current,
                                       o->as.surface.current_scale,
                                       &rects[k]);
                }
            }
        }
    }
    if (gpu) {
        // one band per damage rect, full width rows; outside the bands
        // the framebuffer already mirrors the canvas from earlier passes
        for (int k = 0; k < n_rects; k++) {
            uint32_t y0 = rects[k].y < 0 ? 0 : (uint32_t)rects[k].y;
            uint32_t y1 = (uint32_t)(rects[k].y + (int32_t)rects[k].h);
            if (y1 > fb_h) {
                y1 = fb_h;
            }
            x12_gpu_readback_band((uint8_t *)fb, fb_w * 4, y0, y1);
        }
    }
    full_damage = 0;
    sprite_last = sview;
    sprite_last_sid = ssid;
    sprite_drawn = have_sprite;

    x12_backend->present(fb, rects, n_rects);
    // every surface the stack showed takes the moment the display picks
    // the frame up, the retire later reports it as the scanout time. a
    // surface that never reached the pass never gets a present at all
    uint64_t shown = x12_now_ns();
    for (int i = 0; i < nw; i++) {
        const struct x12_winview *v = x12_windows_at(i);
        x12_frame_presented(v->client, v->surface_id, shown);
    }
    return 1;
}

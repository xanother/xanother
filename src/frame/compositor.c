// Copyright (c) lezginskiy <xanother-x12@proton.me>
// Copyright (c) fluent1s (akemi) <m1st1kad3ad1y@gmail.com>
// SPDX-License-Identifier: MIT

#include "compositor.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend.h"
#include "cpu.h"
#include "frame.h"
#include "gpu.h"
#include "windows.h"
#include "input.h"
#include "output.h"
#include "x12_protocol.h"

// damage rect capacity of one pass, overflow means full repaint so a
// burst of small updates cannot drown the list
#define X12_FRAME_DAMAGE 16

static uint32_t *fb;
static uint32_t fb_w, fb_h;
static struct x12_rect frame_damage[X12_FRAME_DAMAGE];
static int n_frame_damage;
static int full_damage;
// the screen shows a scanned out frame the cpu fb never mirrored, the
// next composite repaints everything to get the mirror honest again
static int fb_stale;

// the gpu present pair: two scanout bos the folds land in, the backend
// flips the finished one straight to the screen
static int pres_fds[2] = { -1, -1 };
static uint32_t pres_strides[2];
static int pres_front_idx;

void x12_comp_set_size(uint32_t w, uint32_t h) {
    free(fb);
    fb = calloc((size_t)w * h, sizeof *fb);
    fb_w = w;
    fb_h = h;
    if (x12_gpu_ready()) {
        x12_gpu_present_setup(w, h, pres_fds, pres_strides);
        pres_front_idx = 0;
    }
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

// direct scanout sessions waiting for their feedback: the frame went
// to the screen through a flip, its present comes with the landing and
// its release credit waits for the replacing flip
#define X12_SCANOUT_SLOTS 4

struct scanout_wait {
    int used;
    struct x12_conn *c;
    uint32_t buffer_id;
    uint64_t token;
    int landed;
};

static struct scanout_wait so_wait[X12_SCANOUT_SLOTS];

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

// one fullscreen opaque window 1:1 on the canvas, carrying a dma
// buffer whose fence has signaled: the frame goes to the screen without
// the composite pass. anything else falls through to compositing
static int try_scanout(struct x12_conn *c, struct x12_surface_state *s,
                       int have_sprite) {
    if (!x12_backend->scanout || have_sprite) {
        return 0;
    }
    if (x12_windows_count() != 1 || s->scanned_out) {
        return 0;
    }
    const struct x12_winview *v = x12_windows_at(0);
    // the window must cover exactly one live output 1:1, the flip goes
    // to that head
    int out = -1;
    struct x12_out_info oi;
    for (int i = 0; i < X12_OUTPUT_SLOTS; i++) {
        if (x12_backend->output_info(i, &oi) && v->x == oi.x &&
            v->y == oi.y && v->width == oi.width && v->height == oi.height) {
            out = i;
            break;
        }
    }
    if (out < 0) {
        return 0;
    }
    if (!s->has_current || s->current.dmabuf_fd < 0 ||
        s->current.width != oi.width || s->current.height != oi.height ||
        s->current.format != X12_FORMAT_XRGB8888) {
        return 0;
    }
    if (s->current_scale != X12_FIXED24P8_ONE || s->current_transform != 0) {
        return 0;
    }
    // a fence the client has not signaled yet would show half drawn
    // pixels, the composite fallback tolerates that, scanout does not
    if (s->fence >= 0) {
        struct pollfd p = { .fd = s->fence, .events = POLLIN };
        if (poll(&p, 1, 0) != 1) {
            return 0;
        }
    }
    int slot = -1;
    for (int i = 0; i < X12_SCANOUT_SLOTS; i++) {
        if (!so_wait[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return 0;
    }
    struct x12_scanout_req req = {
        .dmabuf_fd = s->current.dmabuf_fd,
        .width = s->current.width,
        .height = s->current.height,
        .format = s->current.format,
        .stride = s->current.stride,
        .modifier = s->current.modifier,
        .out = out,
        .cookie = (uint32_t)slot,
    };
    if (!x12_backend->scanout(&req)) {
        return 0;
    }
    // the frame left through a flip: its retire gives the release
    // credit away to this session and the screen no longer mirrors
    // the cpu framebuffer, the next composite repaints everything
    s->scanned_out = 1;
    so_wait[slot].used = 1;
    so_wait[slot].c = c;
    so_wait[slot].buffer_id = s->current.object;
    so_wait[slot].token = s->token;
    so_wait[slot].landed = 0;
    // the screen went to the client buffer: whatever repaint the stack
    // owed is paid, the cpu mirror went stale in the same move
    full_damage = 0;
    fb_stale = 1;
    return 1;
}

// scanout progress from the backend, called once per loop round. the
// landing carries the scanout moment for the present feedback and the
// replacement hands the buffer its release credit back
void x12_comp_scanout_pump(void) {
    if (!x12_backend->scanout_poll) {
        return;
    }
    struct x12_scanout_event ev;
    while (x12_backend->scanout_poll(&ev)) {
        if (ev.cookie >= X12_SCANOUT_SLOTS || !so_wait[ev.cookie].used) {
            continue;
        }
        struct scanout_wait *w = &so_wait[ev.cookie];
        if (ev.kind == X12_SCANOUT_LANDED) {
            w->landed = 1;
            x12_output_present(ev.out, w->token, X12_OUTPUT_PRESENT_KIND_FLIP,
                               ev.ts);
        } else if (ev.kind == X12_SCANOUT_REPLACED) {
            x12_frame_credit(w->c, w->buffer_id);
            w->used = 0;
        }
    }
}

// a dying connection drops its sessions, the kernel keeps the buffers
// alive under the flips but the credits go nowhere now
void x12_comp_scanout_drop(struct x12_conn *c) {
    for (int i = 0; i < X12_SCANOUT_SLOTS; i++) {
        if (so_wait[i].used && so_wait[i].c == c) {
            so_wait[i].used = 0;
        }
    }
}

// one bit per output whose screen showed pixels of this window in this
// pass: the window must overlap the output rect and the paint must
// reach both. an output that flipped without this window keeps its bit
// clear, its present never names frames it did not show
static uint32_t shown_mask(const struct x12_winview *v,
                           const struct x12_rect *rects, int n_rects) {
    uint32_t mask = 0;
    for (int oi = 0; oi < X12_OUTPUT_SLOTS; oi++) {
        struct x12_out_info o;
        if (!x12_backend->output_info(oi, &o)) {
            continue;
        }
        int64_t x0 = v->x > o.x ? v->x : o.x;
        int64_t y0 = v->y > o.y ? v->y : o.y;
        int64_t x1 = v->x + (int64_t)v->width;
        int64_t y1 = v->y + (int64_t)v->height;
        if (x1 > o.x + (int64_t)o.width) {
            x1 = o.x + (int64_t)o.width;
        }
        if (y1 > o.y + (int64_t)o.height) {
            y1 = o.y + (int64_t)o.height;
        }
        if (x1 <= x0 || y1 <= y0) {
            continue;
        }
        for (int k = 0; k < n_rects; k++) {
            int64_t rx1 = rects[k].x + (int64_t)rects[k].w;
            int64_t ry1 = rects[k].y + (int64_t)rects[k].h;
            if (rx1 > x0 && rects[k].x < x1 && ry1 > y0 &&
                rects[k].y < y1) {
                mask |= 1u << oi;
                break;
            }
        }
    }
    return mask;
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
    // direct scanout: a single fullscreen dma frame with fresh damage
    // skips the composite pass and flips to the screen on its own. the
    // attempt runs before the collect loop eats the damage
    if (x12_windows_count() == 1) {
        const struct x12_winview *sv = x12_windows_at(0);
        struct x12_object *so =
            x12_objects_get(&sv->client->objects, sv->surface_id);
        if (so && so->kind == OBJ_SURFACE && so->as.surface.n_current_damage &&
            try_scanout(sv->client, &so->as.surface, have_sprite)) {
            // the surface damage was the point of this pass, consume it
            // exactly like the collect loop would have
            so->as.surface.n_current_damage = 0;
            sprite_last = sview;
            sprite_last_sid = ssid;
            sprite_drawn = have_sprite;
            return 1;
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
    // a stale fb widens the paint to everything, the screen stopped
    // mirroring the cpu framebuffer while the scanout owned it
    int paint_all = repaint_all || fb_stale;
    const struct x12_rect *rects = paint_all ? &clip : frame_damage;
    int n_rects = paint_all ? 1 : n_frame_damage;
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
    full_damage = 0;
    fb_stale = 0;
    sprite_last = sview;
    sprite_last_sid = ssid;
    sprite_drawn = have_sprite;

    // the direct path first: flip the gpu folded frame to the screen
    // when the backend takes it, the cpu mirror and the cpu present are
    // skipped entirely and only the fallback pays for the round trip
    int atomic_armed = 0;
    if (gpu && pres_fds[0] >= 0 && x12_backend->present_gpu) {
        int back = 1 - pres_front_idx;
        if (x12_gpu_present_copy(back)) {
            struct x12_gpu_frame frame = {
                .dmabuf_fd = pres_fds[back],
                .width = fb_w,
                .height = fb_h,
                .format = 0x52474241u,  // matches the scanout import path
                .stride = pres_strides[back],
                .modifier = 0,
                .slot = back,
            };
            if (x12_backend->present_gpu(&frame, rects, n_rects)) {
                atomic_armed = 1;
                pres_front_idx = back;
            }
        }
    }
    if (!atomic_armed) {
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
        x12_backend->present(fb, rects, n_rects);
    }
    // every surface the pass showed takes the moment the display picks
    // the frame up, one bit per output whose screen carried its pixels.
    // a surface the paint never touched stays silent, no present lies
    uint64_t shown = x12_now_ns();
    for (int i = 0; i < nw; i++) {
        const struct x12_winview *v = x12_windows_at(i);
        uint32_t outs = shown_mask(v, rects, n_rects);
        if (outs) {
            x12_frame_presented(v->client, v->surface_id, shown, outs);
        }
    }
    return 1;
}

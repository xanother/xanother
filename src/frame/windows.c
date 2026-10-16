// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#include "windows.h"

#include <string.h>

// placement policy for toplevels: a diagonal cascade starting at 32,32
// with a 24 pixel stride, so overlapping windows always expose their top
// left corner and window N is visually distinguishable from window N-1
#define TOPLEVEL_ORIGIN 32
#define TOPLEVEL_STRIDE 24

static struct x12_winview views[X12_MAX_VIEWS];
static int n_views;
static uint32_t out_w, out_h;
static int dirty;

void x12_windows_init(void) {
    memset(views, 0, sizeof views);
    n_views = 0;
    dirty = 1;
}

void x12_windows_set_output(uint32_t w, uint32_t h) {
    out_w = w;
    out_h = h;
}

void x12_windows_output(uint32_t *w, uint32_t *h) {
    *w = out_w;
    *h = out_h;
}

static void mark_dirty(void) {
    dirty = 1;
}

int x12_windows_take_dirty(void) {
    int d = dirty;
    dirty = 0;
    return d;
}

void x12_windows_force_dirty(void) {
    dirty = 1;
}

int x12_windows_count(void) {
    return n_views;
}

const struct x12_winview *x12_windows_at(int i) {
    if (i < 0 || i >= n_views) {
        return 0;
    }
    return &views[i];
}

int x12_windows_add(struct x12_conn *c, uint32_t surface_id,
                    int32_t x, int32_t y, uint32_t w, uint32_t h,
                    int is_popup) {
    if (n_views >= X12_MAX_VIEWS) {
        return -1;
    }
    struct x12_winview *v = &views[n_views];
    v->used = 1;
    v->client = c;
    v->surface_id = surface_id;
    v->x = x;
    v->y = y;
    v->width = w;
    v->height = h;
    v->nat_w = w;
    v->nat_h = h;
    v->is_popup = is_popup;
    mark_dirty();
    return n_views++;
}

int x12_windows_find(struct x12_conn *c, uint32_t surface_id) {
    for (int i = 0; i < n_views; i++) {
        if (views[i].used && views[i].client == c &&
            views[i].surface_id == surface_id) {
            return i;
        }
    }
    return -1;
}

int x12_windows_top_at(int32_t x, int32_t y) {
    for (int i = n_views - 1; i >= 0; i--) {
        const struct x12_winview *v = &views[i];
        if (!v->used) {
            continue;
        }
        if (x >= v->x && y >= v->y &&
            (uint32_t)(x - v->x) < v->width &&
            (uint32_t)(y - v->y) < v->height) {
            return i;
        }
    }
    return -1;
}

void x12_windows_raise_toplevel(int idx) {
    if (idx < 0 || idx >= n_views || views[idx].is_popup) {
        return;
    }
    // find the last toplevel slot, popups always stay above every toplevel
    int last_top = -1;
    for (int i = 0; i < n_views; i++) {
        if (views[i].used && !views[i].is_popup) {
            last_top = i;
        }
    }
    if (last_top < 0 || last_top == idx) {
        return;
    }
    struct x12_winview moved = views[idx];
    for (int i = idx; i < last_top; i++) {
        views[i] = views[i + 1];
    }
    views[last_top] = moved;
    mark_dirty();
}

// removal closes the gap so the array order stays the z order
void x12_windows_remove(int idx) {
    if (idx < 0 || idx >= n_views) {
        return;
    }
    for (int i = idx; i + 1 < n_views; i++) {
        views[i] = views[i + 1];
    }
    n_views--;
    memset(&views[n_views], 0, sizeof views[n_views]);
    mark_dirty();
}

void x12_windows_remove_client(struct x12_conn *c) {
    for (int i = n_views - 1; i >= 0; i--) {
        if (views[i].client == c) {
            x12_windows_remove(i);
        }
    }
}

void x12_windows_update(int idx, int32_t x, int32_t y, uint32_t w,
                        uint32_t h) {
    if (idx < 0 || idx >= n_views) {
        return;
    }
    struct x12_winview *v = &views[idx];
    if (v->x == x && v->y == y && v->width == w && v->height == h) {
        return;
    }
    v->x = x;
    v->y = y;
    v->width = w;
    v->height = h;
    // the natural size only grows, a clamped commit shrinks the view
    // but must not shrink the growth offer
    if (w > v->nat_w) {
        v->nat_w = w;
    }
    if (h > v->nat_h) {
        v->nat_h = h;
    }
    mark_dirty();
}

void x12_windows_place_toplevel(int32_t *x, int32_t *y) {
    int toplevels = 0;
    for (int i = 0; i < n_views; i++) {
        if (views[i].used && !views[i].is_popup) {
            toplevels++;
        }
    }
    int32_t px = TOPLEVEL_ORIGIN + TOPLEVEL_STRIDE * toplevels;
    int32_t py = TOPLEVEL_ORIGIN + TOPLEVEL_STRIDE * toplevels;
    // clamp keeps the title area on screen even for large window counts
    if (out_w && px + 32 > (int32_t)out_w) {
        px = TOPLEVEL_ORIGIN;
    }
    if (out_h && py + 32 > (int32_t)out_h) {
        py = TOPLEVEL_ORIGIN;
    }
    *x = px;
    *y = py;
}

void x12_windows_place_fullscreen(int idx, int on, int32_t ox, int32_t oy) {
    if (idx < 0 || idx >= n_views || views[idx].is_popup) {
        return;
    }
    // fullscreen pins the view to its output origin so the surface
    // covers the mode exactly; leaving returns to the cascade spot the
    // placement policy hands out today. a commit re-pins a fullscreen
    // view every frame, an unchanged pin must not dirty the stack
    int32_t nx = views[idx].x, ny = views[idx].y;
    if (on) {
        nx = ox;
        ny = oy;
    } else {
        x12_windows_place_toplevel(&nx, &ny);
    }
    x12_windows_update(idx, nx, ny, views[idx].width, views[idx].height);
}

void x12_windows_place_popup(int32_t rect_x, int32_t rect_y,
                             uint32_t rect_w, uint32_t rect_h,
                             uint32_t anchor, uint32_t gravity,
                             int32_t off_x, int32_t off_y,
                             uint32_t pop_w, uint32_t pop_h,
                             const struct x12_winview *parent,
                             int32_t *x, int32_t *y) {
    // anchor point on the anchor rect, parent local coordinates
    int32_t ax = rect_x;
    int32_t ay = rect_y;
    if (anchor == 5) {        // right
        ax += (int32_t)rect_w;
    } else if (anchor == 4) { // left
        ax += 0;
    } else if (anchor == 3) { // bottom
        ay += (int32_t)rect_h;
        ax += (int32_t)(rect_w / 2);
    } else if (anchor == 2) { // top
        ax += (int32_t)(rect_w / 2);
    } else {                  // center or none
        ax += (int32_t)(rect_w / 2);
        ay += (int32_t)(rect_h / 2);
    }
    // gravity names the direction the popup expands toward
    int32_t px = ax;
    int32_t py = ay;
    if (gravity == 5) {       // expands right, origin at the anchor
        px = ax;
    } else if (gravity == 4) { // expands left
        px = ax - (int32_t)pop_w;
    } else if (gravity == 3) { // expands downward
        py = ay;
        px = ax - (int32_t)(pop_w / 2);
    } else if (gravity == 2) { // expands upward
        py = ay - (int32_t)pop_h;
        px = ax - (int32_t)(pop_w / 2);
    } else {
        px = ax - (int32_t)(pop_w / 2);
        py = ay - (int32_t)(pop_h / 2);
    }
    px += off_x;
    py += off_y;
    // parent local becomes global, the parent position is the view origin
    px += parent->x;
    py += parent->y;
    // clamp into the output so menus never render off screen
    if (out_w && px + (int32_t)pop_w > (int32_t)out_w) {
        px = (int32_t)out_w - (int32_t)pop_w;
    }
    if (out_h && py + (int32_t)pop_h > (int32_t)out_h) {
        py = (int32_t)out_h - (int32_t)pop_h;
    }
    if (px < 0) {
        px = 0;
    }
    if (py < 0) {
        py = 0;
    }
    *x = px;
    *y = py;
}

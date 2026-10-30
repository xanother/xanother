// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// the X12 client session inside the bridge: the DDX is a regular X12
// client first, an X server second. the surface here is the whole X11
// root, dix renders into the shared pool and the shadow layer pushes
// the dirty strips out through this file

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "libx12.h"
#include "x12_protocol.h"

#include "x12-client.h"

#define X12_FORMAT_XR24 0x34325258u

// the server places toplevels at this origin and clamps offers by it,
// a rootful bridge mirrors the constant until a position query exists
#define X12_BRIDGE_ORIGIN 32
#define X12_BRIDGE_MAX_DEFAULT 4096

static x12c *session;
static int welcomed;
static uint32_t comp_id, bufman_id, surf_id, pool_id, buf_id, tl_id;
static uint32_t surf_w, surf_h;
static void *pool_pix;
static int released;
static uint32_t conf_w, conf_h, conf_serial;
static int conf_pending;
#define X12_BRIDGE_MAX_OUTS 8
// one entry per bound x12_output instance, the mode and geometry events
// keep it current and the RandR layer reads it as the monitor layout
static struct x12_bridge_out outs[X12_BRIDGE_MAX_OUTS];
static int n_outs;
static uint32_t outs_generation;
// set once the session asked for fullscreen, the mode then belongs to
// the surface whole and the placement origin subtraction turns off
static int fullscreen_requested;
static uint32_t seat_id, ptr_id, kbd_id;
static uint32_t max_w, max_h;
// a resize the release gate refused, replayed from the block handler
static uint32_t pend_w, pend_h;
static int pend_set;
// retired buffers wait for the release that permits their destroy
static uint32_t buf_retired[8];
static int buf_retired_n;
// the dmabuf buffers of the swap chain, the server page flips between
// them and the release of the one that left the screen opens the paint
// gate the pool buffer shares
#define X12_DMABUF_SLOTS 2
static uint32_t dmabuf_ids[X12_DMABUF_SLOTS];
static int dmabuf_slots;

static void on_event(x12c *c, const x12c_proxy *p,
                     const x12_msg_desc *desc, const x12_arg *args,
                     size_t nargs)
{
    (void)c;
    (void)nargs;
    if (strcmp(p->iface->name, "x12_connection") == 0 &&
        strcmp(desc->name, "welcome") == 0) {
        welcomed = 1;
        return;
    }
    if (strcmp(p->iface->name, "x12_buffer") == 0 &&
        strcmp(desc->name, "release") == 0) {
        if (p->id == buf_id || p->id == dmabuf_ids[0] ||
            p->id == dmabuf_ids[1]) {
            // the server is done reading the committed frame, the
            // shadow layer may push the next strip
            released = 1;
        } else {
            // a retired buffer is off the surface: its first release
            // is the permission to destroy and the same credit every
            // release carries, the previous frame left the screen and
            // the shadow layer may push the next one. without the
            // credit the gate dies after a buffer swap, the release of
            // the fresh buffer needs a commit that itself waits on the
            // gate
            for (int i = 0; i < buf_retired_n; i++) {
                if (buf_retired[i] == p->id) {
                    x12c_call(session, "x12_buffer", "destroy", p->id,
                              0, 0);
                    buf_retired[i] = buf_retired[--buf_retired_n];
                    released = 1;
                    break;
                }
            }
        }
        return;
    }
    if (strcmp(p->iface->name, "x12_output") == 0) {
        for (int o = 0; o < n_outs; o++) {
            struct x12_bridge_out *out = &outs[o];
            if (out->proxy_id != p->id) {
                continue;
            }
            if (strcmp(desc->name, "mode") == 0 && nargs >= 3) {
                out->mode_w = (uint32_t)args[0].u64;
                out->mode_h = (uint32_t)args[1].u64;
                out->refresh_millihz = (uint32_t)args[2].u64;
                out->have_mode = 1;
                if (!out->live) {
                    out->live = 1;
                    outs_generation++;
                }
            } else if (strcmp(desc->name, "removed") == 0) {
                if (out->live) {
                    out->live = 0;
                    out->have_mode = 0;
                    outs_generation++;
                }
            } else if (strcmp(desc->name, "geometry") == 0 && nargs >= 2) {
                out->org_x = (int32_t)args[0].i64;
                out->org_y = (int32_t)args[1].i64;
                out->have_geom = 1;
            } else if (strcmp(desc->name, "name") == 0 && nargs >= 1 &&
                       args[0].str_len < sizeof out->name) {
                memcpy(out->name, args[0].str, args[0].str_len);
                out->name[args[0].str_len] = 0;
            } else if (strcmp(desc->name, "present") == 0) {
                // the flip kind is the real vblank the present
                // extension completes against, a copy lands between
                // flips and only stamps the moment
                x12_present_flip_seen(x12c_last_event_ts(session),
                                      nargs >= 2 &&
                                      args[1].u64 ==
                                      X12_OUTPUT_PRESENT_KIND_FLIP);
            }
            break;
        }
        return;
    }
    // seat events forward into dix unchanged, the ddx input glue owns
    // the dix queues; coords are surface local fixed24p8 and the keys
    // are evdev codes
    if (p->id == ptr_id) {
        if (strcmp(desc->name, "motion") == 0 && nargs >= 3) {
            x12_bridge_input_motion((int32_t)(args[1].i64 >> 8),
                                    (int32_t)(args[2].i64 >> 8));
        } else if (strcmp(desc->name, "button") == 0 && nargs >= 3) {
            x12_bridge_input_button((uint32_t)args[1].u64,
                                    args[2].u64 == 0);
        } else if (strcmp(desc->name, "axis") == 0 && nargs >= 2) {
            x12_bridge_input_axis((int32_t)args[0].i64,
                                  (int32_t)args[1].i64);
        }
        return;
    }
    if (p->id == kbd_id) {
        if (strcmp(desc->name, "key") == 0 && nargs >= 3) {
            x12_bridge_input_key((uint32_t)args[1].u64,
                                 args[2].u64 == 0);
        }
        return;
    }
    if (strcmp(p->iface->name, "x12_toplevel") == 0 &&
        strcmp(desc->name, "configure") == 0 && p->id == tl_id &&
        nargs >= 3) {
        // the offer is acknowledged right away: the output geometry of
        // the bridge is env driven this wave, session resize arrives
        // with the randr wave
        conf_w = (uint32_t)args[0].u64;
        conf_h = (uint32_t)args[1].u64;
        conf_serial = (uint32_t)args[nargs - 1].u64;
        conf_pending = 1;
        x12_arg ack[1] = { { .u64 = conf_serial } };
        x12c_call(session, "x12_toplevel", "ack_configure", tl_id,
                  ack, 1);
    }
}

int x12_bridge_start(const char *sock_path)
{
    const char *mw = getenv("X12_BRIDGE_MAX_W");
    const char *mh = getenv("X12_BRIDGE_MAX_H");
    max_w = mw ? (uint32_t)atoi(mw) : X12_BRIDGE_MAX_DEFAULT;
    max_h = mh ? (uint32_t)atoi(mh) : X12_BRIDGE_MAX_DEFAULT;
    if (!max_w || !max_h || max_w > 8192 || max_h > 8192) {
        max_w = max_h = X12_BRIDGE_MAX_DEFAULT;
    }
    session = x12c_connect(sock_path, on_event);
    if (!session) {
        return 0;
    }
    // the binds queue before any dispatch: the server announces the
    // globals once right after the welcome, a bind queued later would
    // wait for an announcement that already passed
    comp_id = x12c_bind(session, "x12_compositor", 0);
    bufman_id = x12c_bind(session, "x12_buffer_manager", 0);
    seat_id = x12c_bind(session, "x12_seat", 0);
    if (x12c_flush(session) != 0) {
        return 0;
    }
    for (int i = 0; i < 100 && !welcomed; i++) {
        if (x12c_dispatch(session, 50) < 0) {
            return 0;
        }
    }
    if (!welcomed || !comp_id || !bufman_id || !seat_id) {
        return 0;
    }
    // every announced output instance becomes a monitor of the X
    // screen, the count rode the welcome globals
    for (int i = 0; i < x12c_n_outputs(session) && n_outs < X12_BRIDGE_MAX_OUTS;
         i++) {
        uint32_t id = x12c_bind_instance(session, "x12_output", (uint32_t)i, 0);
        if (!id) {
            break;
        }
        memset(&outs[n_outs], 0, sizeof outs[0]);
        outs[n_outs].proxy_id = id;
        outs[n_outs].instance = (uint32_t)i;
        n_outs++;
    }
    if (!n_outs) {
        return 0;
    }
    for (int i = 0; i < 100; i++) {
        int ready = 1;
        for (int o = 0; o < n_outs; o++) {
            if (!outs[o].have_mode) {
                ready = 0;
            }
        }
        if (ready) {
            break;
        }
        if (x12c_dispatch(session, 50) < 0) {
            return 0;
        }
    }
    // seat devices, the input wave of the phase feeds dix from them
    ptr_id = x12c_reserve(session, "x12_pointer", 0);
    kbd_id = x12c_reserve(session, "x12_keyboard", 0);
    x12_arg pa[1] = { { .u64 = ptr_id } };
    x12_arg ka[1] = { { .u64 = kbd_id } };
    if (!ptr_id || !kbd_id ||
        x12c_call(session, "x12_seat", "get_pointer", seat_id, pa,
                  1) != 0 ||
        x12c_call(session, "x12_seat", "get_keyboard", seat_id, ka,
                  1) != 0) {
        return 0;
    }
    return 1;
}

int x12_bridge_output_setup(uint32_t width, uint32_t height,
                            void **pixels, uint32_t *pitch)
{
    surf_w = width;
    surf_h = height;

    // the bind proxies carry the ids handed out by the global
    // announcements the start handshake consumed
    if (!comp_id || !bufman_id ||
        x12c_proxy_iface(session, comp_id) == 0 ||
        x12c_proxy_iface(session, bufman_id) == 0) {
        return -1;
    }

    surf_id = x12c_reserve(session, "x12_surface", 0);
    x12_arg cr[1] = { { .u64 = surf_id } };
    if (!surf_id ||
        x12c_call(session, "x12_compositor", "create_surface", comp_id,
                  cr, 1) != 0) {
        return -1;
    }

    // one pool for the session at the maximum geometry, buffers carve
    // at offset zero; a resize re-commits a new carve and the memfd
    // pages materialize only where dix actually painted
    size_t pool_size = (size_t)max_w * max_h * 4;
    int memfd = x12c_pool_create(session, bufman_id, pool_size,
                                 &pool_id, 0);
    if (memfd < 0) {
        return -1;
    }
    pool_pix = mmap(0, pool_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    memfd, 0);
    close(memfd);
    if (pool_pix == (void *)-1) {
        pool_pix = NULL;
        return -1;
    }
    // the buffer rides to the server before the first real copy, the
    // pool starts black so the mapping shows a clean frame
    memset(pool_pix, 0, pool_size);

    if (x12c_buffer_create(session, pool_id, 0, surf_w, surf_h,
                           X12_FORMAT_XR24, &buf_id, 0) != 0) {
        return -1;
    }
    x12_arg at[1] = { { .u64 = buf_id } };
    if (x12c_call(session, "x12_surface", "set_buffer", surf_id, at,
                  1) != 0) {
        return -1;
    }
    if (x12c_call(session, "x12_surface", "commit", surf_id, 0, 0) != 0) {
        return -1;
    }

    // the map handshake carries the configure offer, the ack and the
    // re commit, it blocks until the server releases the frame
    tl_id = x12c_toplevel_map(session, comp_id, surf_id);
    if (!tl_id) {
        return -1;
    }
    released = 1;

    // rootful contract: the session covers the whole output, the
    // placement frame of the server must never show around it; the
    // configure offer riding back resizes the dix root to the mode
    // the server actually gave. an operator opts out for debugging
    const char *fs = getenv("X12_BRIDGE_FULLSCREEN");
    if (!fs || strcmp(fs, "0") != 0) {
        x12_arg sf[1] = { { .u64 = 1 } };
        if (x12c_call(session, "x12_toplevel", "set_fullscreen", tl_id,
                      sf, 1) == 0) {
            fullscreen_requested = 1;
        }
    }

    // variable refresh is an operator wish, the opt-in env rides the
    // whole session; a host without vrr support refuses the request
    // and the session runs fixed refresh as usual. the wish goes out
    // after the session is up so the refusals land in the ordinary
    // dispatch beat and never perturb the setup
    const char *vrr = getenv("X12_BRIDGE_VRR");
    if (vrr && strcmp(vrr, "1") == 0) {
        x12_arg on[1] = { { .u64 = 1 } };
        int asked = 0;
        for (int o = 0; o < n_outs; o++) {
            if (x12c_call(session, "x12_output", "set_vrr", outs[o].proxy_id,
                          on, 1) == 0) {
                asked++;
            }
        }
        fprintf(stderr, "X12-BRIDGE: vrr requested on %d of %d outputs\n",
                asked, n_outs);
        x12_bridge_flush();
    }

    if (pixels) {
        *pixels = pool_pix;
    }
    if (pitch) {
        *pitch = surf_w * 4;
    }
    return 0;
}

void *x12_bridge_output_pixels(void **pixels)
{
    if (pixels) {
        *pixels = pool_pix;
    }
    return pool_pix;
}

int x12_bridge_flush(void)
{
    return x12c_flush(session);
}

// one resize step: a new carve at offset zero takes the surface, the
// old buffer joins the retire ring and dies on its first release
static int do_resize(uint32_t w, uint32_t h)
{
    uint32_t nb = 0;
    if (x12c_buffer_create(session, pool_id, 0, w, h,
                           X12_FORMAT_XR24, &nb, 0) != 0) {
        return 0;
    }
    x12_arg at[1] = { { .u64 = nb } };
    if (x12c_call(session, "x12_surface", "set_buffer", surf_id, at,
                  1) != 0) {
        x12c_call(session, "x12_buffer", "destroy", nb, 0, 0);
        return 0;
    }
    if (buf_id && buf_retired_n <
        (int)(sizeof buf_retired / sizeof *buf_retired)) {
        buf_retired[buf_retired_n++] = buf_id;
    }
    buf_id = nb;
    // the fresh carve starts black: the old content belongs to the old
    // geometry and presenting it scaled into the new one is garbage
    memset(pool_pix, 0, (size_t)w * h * 4);
    // no commit here: the first painted frame of the shadow pump takes
    // the commit, so the visible frame jumps from the old geometry
    // straight to painted content instead of flashing an empty buffer
    surf_w = w;
    surf_h = h;
    return 1;
}

int x12_bridge_resize(uint32_t w, uint32_t h)
{
    if (w == surf_w && h == surf_h) {
        return 1;
    }
    if (!w || !h || w > max_w || h > max_h) {
        return 0;
    }
    if (!x12_bridge_gate_open()) {
        pend_w = w;
        pend_h = h;
        pend_set = 1;
        return 0;
    }
    return do_resize(w, h);
}

int x12_bridge_commit_queued(void)
{
    if (!pend_set) {
        return 0;
    }
    if (!x12_bridge_gate_open()) {
        return 0;
    }
    uint32_t w = pend_w, h = pend_h;
    pend_set = 0;
    return do_resize(w, h);
}

void x12_bridge_max_size(uint32_t *w, uint32_t *h)
{
    if (w) {
        *w = max_w;
    }
    if (h) {
        *h = max_h;
    }
}

int x12_bridge_preferred_size(uint32_t *w, uint32_t *h)
{
    // the rootful X screen covers the whole output canvas, the union
    // of the tiled monitor rects; a fullscreen session owns the whole
    // canvas, the plain cascade placed start keeps the placement frame
    // clear by shrinking the canvas by the origin
    uint32_t cw = 0, ch = 0;
    for (int o = 0; o < n_outs; o++) {
        if (!outs[o].have_mode) {
            return 0;
        }
        cw += outs[o].mode_w;
        if (outs[o].mode_h > ch) {
            ch = outs[o].mode_h;
        }
    }
    if (!cw || !ch) {
        return 0;
    }
    if (!fullscreen_requested) {
        cw = cw > X12_BRIDGE_ORIGIN ? cw - X12_BRIDGE_ORIGIN : 1;
        ch = ch > X12_BRIDGE_ORIGIN ? ch - X12_BRIDGE_ORIGIN : 1;
    }
    if (cw > max_w) {
        cw = max_w;
    }
    if (ch > max_h) {
        ch = max_h;
    }
    if (w) {
        *w = cw;
    }
    if (h) {
        *h = ch;
    }
    return 1;
}

uint32_t x12_bridge_out_count(void)
{
    return (uint32_t)n_outs;
}

const struct x12_bridge_out *x12_bridge_out_at(uint32_t index)
{
    if (index >= (uint32_t)n_outs) {
        return NULL;
    }
    return &outs[index];
}

uint32_t x12_bridge_outs_generation(void)
{
    return outs_generation;
}

int x12_bridge_dispatch(void)
{
    return x12c_dispatch(session, 0) < 0 ? -1 : 0;
}

int x12_bridge_gate_open(void)
{
    return released != 0 && pool_pix != NULL;
}

void x12_bridge_commit_rows(int y0, int y1)
{
    if (y0 < 0) {
        y0 = 0;
    }
    if (y1 > (int)surf_h) {
        y1 = (int)surf_h;
    }
    if (y0 >= y1 || !x12_bridge_gate_open()) {
        return;
    }
    x12_arg d[4] = {
        { .i64 = 0 },
        { .i64 = y0 },
        { .i64 = (int32_t)surf_w },
        { .i64 = (uint32_t)(y1 - y0) },
    };
    if (x12c_call(session, "x12_surface", "damage", surf_id, d, 4) != 0 ||
        x12c_call(session, "x12_surface", "commit", surf_id, 0, 0) != 0) {
        return;
    }
    released = 0;
}

int x12_bridge_take_configure(uint32_t *w, uint32_t *h)
{
    if (!conf_pending) {
        return 0;
    }
    if (w) {
        *w = conf_w;
    }
    if (h) {
        *h = conf_h;
    }
    conf_pending = 0;
    return 1;
}

uint32_t x12_bridge_surface_width(void)
{
    return surf_w;
}

uint32_t x12_bridge_surface_height(void)
{
    return surf_h;
}

int x12_bridge_fd(void)
{
    return x12c_fd(session);
}

// the dmabuf mode of the session: the screen backing lives in a linear
// dmabuf the server imports as its scanout, the pump stops copying rows
// and only reports damage. the pool stays allocated as the fallback
// store, a failed re-export on resize walks back to it
static int dmabuf_active;

// imports a screen buffer descriptor into the swap chain without
// pinning it, the caller shows a slot through pin plus commit; answers
// the slot index or -1; the call owns the fd and closes it on every
// path, the caller must not
int
x12_bridge_dmabuf_attach(int fd, uint32_t stride, uint64_t modifier,
                         uint32_t w, uint32_t h)
{
    if (!session || !comp_id || !bufman_id || !surf_id ||
        dmabuf_slots >= X12_DMABUF_SLOTS) {
        close(fd);
        return -1;
    }
    surf_w = w;
    surf_h = h;
    uint32_t id = x12c_reserve(session, "x12_buffer", 0);
    if (!id) {
        close(fd);
        return -1;
    }
    x12_arg a[7] = {
        { .i64 = 0 }, { .u64 = w }, { .u64 = h },
        { .u64 = X12_FORMAT_XR24 }, { .u64 = modifier },
        { .u64 = stride }, { .u64 = id },
    };
    // the call took the fd and closed it whichever way it went
    if (x12c_callfd(session, "x12_buffer_manager", "import_dmabuf",
                    bufman_id, a, 7, 0, fd) != 0) {
        return -1;
    }
    // the first import takes the pool buffer off the surface, the
    // release contract allows its destroy right away
    if (dmabuf_slots == 0 && buf_id) {
        x12c_call(session, "x12_buffer", "destroy", buf_id, 0, 0);
        buf_id = 0;
    }
    dmabuf_active = 1;
    dmabuf_ids[dmabuf_slots] = id;
    return dmabuf_slots++;
}

// parks a swap chain buffer on the surface without a commit, the pump
// damage plus commit shows it as one atomic frame
int
x12_bridge_dmabuf_pin(int slot)
{
    if (slot < 0 || slot >= dmabuf_slots) {
        return 0;
    }
    x12_arg at[1] = { { .u64 = dmabuf_ids[slot] } };
    if (x12c_call(session, "x12_surface", "set_buffer", surf_id, at,
                  1) != 0) {
        return 0;
    }
    return 1;
}

// shows a slot on the surface for real, the first one flips the session
// into the direct mode
int
x12_bridge_dmabuf_show(int slot)
{
    if (!x12_bridge_dmabuf_pin(slot)) {
        return 0;
    }
    if (x12c_call(session, "x12_surface", "commit", surf_id, 0, 0) != 0) {
        return 0;
    }
    released = 0;
    printf("X12-BRIDGE: screen dmabuf exported, scanout direct\n");
    fflush(stdout);
    return 1;
}

// retires the swap chain on a re-export, the old buffers keep their
// release contract through the retired list
void
x12_bridge_dmabuf_reset(void)
{
    for (int i = 0; i < dmabuf_slots; i++) {
        if (dmabuf_ids[i] && buf_retired_n < 8) {
            buf_retired[buf_retired_n++] = dmabuf_ids[i];
        }
    }
    dmabuf_slots = 0;
    dmabuf_active = 0;
}

int
x12_bridge_dmabuf_active(void)
{
    return dmabuf_active;
}

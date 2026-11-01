// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// x12 session of the bridge DDX, see x12-client.c

#ifndef X12_BRIDGE_CLIENT_H
#define X12_BRIDGE_CLIENT_H

#include <stdint.h>

// connects to the X12 server socket and waits for the welcome event;
// the libx12 connect already sends hello, nonzero on success
int x12_bridge_start(const char *sock_path);

// full output session: binds the globals, creates the surface, pool,
// buffer and the rootful toplevel, leaves the initial frame on screen;
// pixels points at the pool the DDX paints into, pitch is the row
// stride in bytes. nonzero on success
int x12_bridge_output_setup(uint32_t width, uint32_t height,
                            void **pixels, uint32_t *pitch);

// the shared pool the DDX copies rows into, NULL before setup
void *x12_bridge_output_pixels(void **pixels);

// pushes the batched requests now; the dix block handler is the only
// guaranteed heartbeat of the bridge, libx12 batches and a commit can
// sit in its buffer forever without this
int x12_bridge_flush(void);

// drains the x12 socket without blocking, nonzero on server hangup
int x12_bridge_dispatch(void);

// nonzero when the server released the last commit and a new one is
// allowed on the wire
int x12_bridge_gate_open(void);

// the monitor layout the session announced, one entry per bound
// x12_output instance; the RandR layer maps these onto RR outputs
struct x12_bridge_out {
    uint32_t proxy_id;
    uint32_t mode_w, mode_h;
    uint32_t refresh_millihz;
    int32_t org_x, org_y;
    uint32_t instance;
    char name[32];
    int have_mode, have_geom;
    int live;
};

// the number of bound outputs, zero before the setup
uint32_t x12_bridge_out_count(void);

// the output at the index, NULL out of range
const struct x12_bridge_out *x12_bridge_out_at(uint32_t index);

// bumps whenever a head came or went, the RandR layer reissues the
// connection state on the change
uint32_t x12_bridge_outs_generation(void);

// one scanout cycle of the session landed, the DDX present layer
// completes its parked vblank requests against the timestamp
void x12_present_flip_seen(uint64_t ts_ns, int is_flip);

// pushes the dirty row strip [y0, y1) to the server: damage plus
// commit, the caller copies the rows into the pool first
void x12_bridge_damage_rows(int y0, int y1);
void x12_bridge_commit(void);
void x12_bridge_commit_rows(int y0, int y1);

// pending configure from the server, nonzero when one was offered;
// the geometry itself stays env driven this wave, the offer is only
// acknowledged
int x12_bridge_take_configure(uint32_t *w, uint32_t *h);

uint32_t x12_bridge_surface_width(void);
uint32_t x12_bridge_surface_height(void);

// the x12 session fd for the dix notify integration
int x12_bridge_fd(void);

// seat event forwarders, called from the session handler, landed in
// dix by the ddx input glue
void x12_bridge_input_motion(int32_t x, int32_t y);
void x12_bridge_input_button(uint32_t button, int pressed);
void x12_bridge_input_axis(int32_t kind, int32_t value);
void x12_bridge_input_key(uint32_t code, int pressed);

// the pool geometry ceiling, the randr size limits ride on it
void x12_bridge_max_size(uint32_t *w, uint32_t *h);

// nonzero when the output mode implies a different root size, the
// bridge fills the output minus the server placement origin
int x12_bridge_preferred_size(uint32_t *w, uint32_t *h);

// resize the session surface to w*h: a new buffer carve, set_buffer,
// commit. queues while the release gate is closed, the dix block
// handler flushes the queue; returns nonzero when committed now
int x12_bridge_resize(uint32_t w, uint32_t h);

// flushes a queued resize once the gate reopens, nonzero when one
// went out
int x12_bridge_commit_queued(void);

#endif
// dmabuf mode: the screen backing is a pair of imported scanout
// buffers the server page flips between; attach takes fd ownership and
// answers the swap chain slot or -1, pin parks a buffer on the surface
// and show commits it for real, reset retires the chain on a re-export
int x12_bridge_dmabuf_attach(int fd, uint32_t stride, uint64_t modifier,
                             uint32_t w, uint32_t h);
int x12_bridge_dmabuf_pin(int slot);
int x12_bridge_dmabuf_show(int slot);
void x12_bridge_dmabuf_reset(void);
int x12_bridge_dmabuf_active(void);

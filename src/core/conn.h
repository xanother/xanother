// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#ifndef X12_CONN_H
#define X12_CONN_H

#include <stddef.h>
#include <stdint.h>

#include "x12_wire.h"
#include "x12_limits.h"
#include "objects.h"

// one connected client: io buffers, fd queues, capability bits and the
// object table. the struct carries every limit the connection can hit
struct x12_conn {
    int fd;
    // tls connection object on tls transports, NULL otherwise
    void *tls;
    // 1 on tcp and tls: no scm rights exists there, fd passing refuses
    int net;
    // envelope staging for the network transports, allocated on first
    // use. bytes arrive framed as [hdr][raw_len][data] and peel into in
    uint8_t *netbuf;
    size_t netbuf_len;
    uint8_t in[X12_IO_BUF];
    size_t in_len;
    uint8_t out[X12_IO_BUF];
    size_t out_len;
    int in_fds[X12_MAX_FDS];
    size_t n_in_fds;
    int out_fds[X12_MAX_FDS];
    size_t n_out_fds;
    // wire slots name an fd by its index into the received fd list below,
    // handlers take the descriptor out of the list when they consume it
    uint64_t out_fds_sent;
    uint32_t caps;
    uint32_t serial;
    // seat objects this client bound, 0 means no device. one pointer
    // and one keyboard per client, a second get refuses loudly
    uint32_t pointer_id, keyboard_id;
    // the clipboard device, one per connection like the seat devices
    uint32_t data_device_id;
    // the cursor sprite the client set: a roleless surface drawn at the
    // pointer minus the hotspot, 0 hides the sprite
    uint32_t cursor_surface_id;
    int32_t cursor_hot_x, cursor_hot_y;
    // shm pool bytes currently mapped for this client, new pools past the
    // budget in limits.h are refused with out-of-memory
    uint64_t pool_bytes;
    // monotonic commit token minted by surface commits
    uint64_t token;
    // backpressure verdict: an event that cannot fit the out buffer means
    // the client stopped reading, the server disconnects it loudly
    // instead of silently dropping events. the main loop reaps dead ones
    int dead;
    // resume session: every hello mints a token, a disconnected client
    // holding one finds its objects and scene presence waiting. while
    // parked the slot keeps its address - the scene keys clients by
    // pointer - and events are dropped, the client rebuilds on resume
    int parked;
    uint64_t resume_token;
    uint64_t park_deadline;
    struct x12_objects objects;
};

void x12_conn_init(struct x12_conn *c, int fd);

// mints the resume token of this session and returns it
uint64_t x12_conn_mint_session(struct x12_conn *c);

// parks the connection for ttl_ns: the slot, its objects and its scene
// presence survive with the transport gone. returns 0 when there is no
// session to park
int x12_conn_park(struct x12_conn *c, uint64_t ttl_ns);

// moves the fresh connection's transport into the parked session
// behind token and hands the parked conn back. NULL when the token is
// unknown, the fresh conn keeps living as its own session then
struct x12_conn *x12_conn_resume(struct x12_conn *fresh, uint64_t token);

// hands every parked session past its deadline to teardown
void x12_conn_parked_sweep(uint64_t now,
                           void (*teardown)(struct x12_conn *));

// runs every complete frame already buffered, for input that arrived
// with a transport move instead of a read
int x12_conn_pump(struct x12_conn *c,
                  int (*on_frame)(struct x12_conn *, const uint8_t *,
                                  size_t));

// reads bytes and hands every complete frame to on_frame, partial frames
// wait for more, framing violations die loudly. returns -1 once dead
int x12_conn_read(struct x12_conn *c,
                  int (*on_frame)(struct x12_conn *, const uint8_t *, size_t));

void x12_conn_flush(struct x12_conn *c);

// queues one event stamped with a monotonic timestamp, the message is
// named the way the idl names it
int x12_conn_event(struct x12_conn *c, const x12_iface_desc *iface,
                   const char *evt, uint32_t object,
                   const x12_arg *args, size_t nargs);

// like x12_conn_event but stamps the event with the given nanosecond
// time instead of now, presentation feedback carries the real scanout
// moment this way. ts zero falls back to now
int x12_conn_event_ts(struct x12_conn *c, const x12_iface_desc *iface,
                      const char *evt, uint32_t object,
                      const x12_arg *args, size_t nargs, uint64_t ts);

// like x12_conn_event but queues a real fd for scm rights delivery, the
// arg at fd_arg_index receives the wire slot of the descriptor and the
// flush is immediate so the fd travels with its event
int x12_conn_event_fd(struct x12_conn *c, const x12_iface_desc *iface,
                      const char *evt, uint32_t object, x12_arg *args,
                      size_t nargs, size_t fd_arg_index, int fd);

// protocol errors are per object events on the connection object
int x12_conn_error(struct x12_conn *c, uint32_t code, uint32_t object);

// backpressure verdict, printed once per client
void x12_conn_kill(struct x12_conn *c, const char *why);

// monotonic serial counter for events that need one
uint32_t x12_conn_next_serial(struct x12_conn *c);

// monotonic commit token, one per surface commit
uint64_t x12_conn_next_token(struct x12_conn *c);

// takes the fd sitting at index in the received list and shifts the rest
// down, fails when the slot is empty
int x12_conn_take_fd(struct x12_conn *c, uint32_t index, int *out_fd);

// closes every fd still parked on the connection, part of the reap path
void x12_conn_release_fds(struct x12_conn *c);

uint64_t x12_now_ns(void);

#endif

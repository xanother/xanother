// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#ifndef X12_LIB_H
#define X12_LIB_H

#include "x12_wire.h"

// client session library over the shared wire engine. one connection, one
// thread, one handler; helpers cover the pool and the toplevel handshake
// so a client never touches raw opcodes

typedef struct x12c x12c;

// a proxy is the client side mirror of one server object
typedef struct {
    uint32_t id;
    const x12_iface_desc *iface;
    void *user;
    int pending;
} x12c_proxy;

// every event lands here; args point into the session receive buffer and
// are valid only for the duration of the call, desc carries the arg
// types for inspection
typedef void (*x12c_handler)(x12c *c, const x12c_proxy *p,
                             const x12_msg_desc *desc, const x12_arg *args,
                             size_t nargs);

x12c *x12c_connect(const char *path, x12c_handler handler);
void x12c_close(x12c *c);
int x12c_fd(x12c *c);
// nonzero once the named proxy was bound from a registry announcement
const x12_iface_desc *x12c_proxy_iface(const x12c *c, uint32_t id);

// sends the batched requests now; a caller polling the fd on its own must
// flush before blocking, dispatch flushes on its own
int x12c_flush(x12c *c);

// queues a bind, it fires when the registry announces the named global;
// returns the reserved proxy id or 0
uint32_t x12c_bind(x12c *c, const char *global, void *user);

// binds one instance of a multi instance global such as x12_output,
// zero on failure
uint32_t x12c_bind_instance(x12c *c, const char *global, uint32_t instance,
                            void *user);

// the number of x12_output instances the welcome globals announced
int x12c_n_outputs(const x12c *c);

// the implicit nanosecond timestamp of the event being dispatched,
// valid only inside the handler
uint64_t x12c_last_event_ts(const x12c *c);

// takes the next free proxy id for a new_id argument or an id argument
// of a factory request
uint32_t x12c_reserve(x12c *c, const char *iface_name, void *user);

int x12c_call(x12c *c, const char *iface_name, const char *request,
              uint32_t object, x12_arg *args, size_t nargs);

// like call but queues one fd with the frame, the arg at fd_arg carries
// the wire slot index; keymap events hand out real fds again
int x12c_callfd(x12c *c, const char *iface_name, const char *request,
                uint32_t object, x12_arg *args, size_t nargs,
                size_t fd_arg, int fd);

// pushes pending bytes and drains the socket, returns 1 when events
// fired, 0 on a quiet timeout, -1 when the server hung up
int x12c_dispatch(x12c *c, int timeout_ms);

// memfd backed shared memory pool, returns the fd to paint into before
// any buffer attaches; the proxy id lands in id_out
int x12c_pool_create(x12c *c, uint32_t bufman_id, size_t size,
                     uint32_t *id_out, void *user);

int x12c_buffer_create(x12c *c, uint32_t pool_id, size_t offset,
                       uint32_t width, uint32_t height, uint32_t format,
                       uint32_t *id_out, void *user);

// toplevel handshake on a surface that already carries a committed
// buffer: the configure offer, ack, commit; blocks in dispatch until the
// server releases the buffer, meaning the frame is composited. returns
// the created toplevel object id, later ack_configure must use it
int x12c_toplevel_map(x12c *c, uint32_t compositor_id, uint32_t surface_id);

#endif

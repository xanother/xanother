// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#ifndef X12_CONN_H
#define X12_CONN_H

#include <stddef.h>
#include <stdint.h>

#include "x12_wire.h"
#include "limits.h"
#include "objects.h"

// one connected client: io buffers, fd queues, capability bits and the
// object table. the struct carries every limit the connection can hit
struct x12_conn {
    int fd;
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
    // shm pool bytes currently mapped for this client, new pools past the
    // budget in limits.h are refused with out-of-memory
    uint64_t pool_bytes;
    // monotonic commit token minted by surface commits
    uint64_t token;
    // backpressure verdict: an event that cannot fit the out buffer means
    // the client stopped reading, the server disconnects it loudly
    // instead of silently dropping events. the main loop reaps dead ones
    int dead;
    struct x12_objects objects;
};

void x12_conn_init(struct x12_conn *c, int fd);

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

// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#ifndef X12_BUFFER_H
#define X12_BUFFER_H

#include <stdint.h>

#include "conn.h"

// shm pools and carved buffers. every size is checked against the pool
// and the per client memory budget before anything is mapped, a bad carve
// is refused with bad-value and leaves the pool untouched

// maps fd as a pool of size bytes, the fd is consumed on success
int x12_pool_create(struct x12_conn *c, uint32_t id, int fd, uint32_t size);

// frees the mapping and every buffer carved out of it
void x12_pool_destroy(struct x12_conn *c, uint32_t id);

// carves a buffer out of a pool at offset, checks width, height, stride
// and format against the pool bounds first
int x12_buffer_create(struct x12_conn *c, uint32_t pool_id, uint32_t id,
                      uint32_t offset, uint32_t width, uint32_t height,
                      uint32_t stride, uint32_t format);

// frees the slot, surface references to it are dropped by the caller
void x12_buffer_destroy(struct x12_conn *c, uint32_t id);

// bytes per pixel for the formats the buffer path understands, 0 when the
// format is unknown
uint32_t x12_format_bpp(uint32_t format);

// teardown hook for the reap path, unmaps every pool of the client
void x12_buffers_teardown(struct x12_conn *c);

#endif

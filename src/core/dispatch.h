// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#ifndef X12_DISPATCH_H
#define X12_DISPATCH_H

#include <stddef.h>
#include <stdint.h>

#include "conn.h"

// routes one frame to its handler, protocol errors become error events
// on the connection object. returns 0 to keep the connection, -1 to drop
int x12_dispatch(struct x12_conn *c, const uint8_t *frame, size_t len);

#endif

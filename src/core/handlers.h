// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#ifndef X12_HANDLERS_H
#define X12_HANDLERS_H

#include "conn.h"

// handlers return 0 to keep the connection and -1 to drop it, protocol
// errors are answered with error events before returning 0

int x12_handle_connection(struct x12_conn *c, struct x12_object *o,
                          const x12_msg_desc *msg, const x12_arg *args);
int x12_handle_registry(struct x12_conn *c, struct x12_object *o,
                        const x12_msg_desc *msg, const x12_arg *args);

#endif

// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#ifndef X12_HANDLERS_H
#define X12_HANDLERS_H

#include "conn.h"

// handlers return 0 to keep the connection and -1 to drop it, protocol
// errors are answered with error events naming the request target

int x12_handle_connection(struct x12_conn *c, uint32_t object,
                          const x12_msg_desc *msg, const x12_arg *args);
int x12_handle_output(struct x12_conn *c, uint32_t object,
                      const x12_msg_desc *msg, const x12_arg *args);

int x12_handle_registry(struct x12_conn *c, uint32_t object,
                        const x12_msg_desc *msg, const x12_arg *args);
int x12_handle_compositor(struct x12_conn *c, uint32_t object,
                          const x12_msg_desc *msg, const x12_arg *args);
int x12_handle_buffer_manager(struct x12_conn *c, uint32_t object,
                              const x12_msg_desc *msg, const x12_arg *args);
int x12_handle_shm_pool(struct x12_conn *c, uint32_t object,
                        const x12_msg_desc *msg, const x12_arg *args);
int x12_handle_buffer(struct x12_conn *c, uint32_t object,
                      const x12_msg_desc *msg, const x12_arg *args);
int x12_handle_surface(struct x12_conn *c, uint32_t object,
                       const x12_msg_desc *msg, const x12_arg *args);
int x12_handle_positioner(struct x12_conn *c, uint32_t object,
                          const x12_msg_desc *msg, const x12_arg *args);
int x12_handle_toplevel(struct x12_conn *c, uint32_t object,
                        const x12_msg_desc *msg, const x12_arg *args);
int x12_handle_popup(struct x12_conn *c, uint32_t object,
                     const x12_msg_desc *msg, const x12_arg *args);
int x12_handle_seat(struct x12_conn *c, uint32_t object,
                    const x12_msg_desc *msg, const x12_arg *args);
int x12_handle_pointer(struct x12_conn *c, uint32_t object,
                       const x12_msg_desc *msg, const x12_arg *args);
int x12_handle_keyboard(struct x12_conn *c, uint32_t object,
                        const x12_msg_desc *msg, const x12_arg *args);
int x12_handle_data_device(struct x12_conn *c, uint32_t object,
                           const x12_msg_desc *msg, const x12_arg *args);
int x12_handle_data_source(struct x12_conn *c, uint32_t object,
                           const x12_msg_desc *msg, const x12_arg *args);
int x12_handle_data_offer(struct x12_conn *c, uint32_t object,
                          const x12_msg_desc *msg, const x12_arg *args);

#endif

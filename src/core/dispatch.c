// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#include "dispatch.h"

#include "x12_protocol.h"
#include "handlers.h"

int x12_dispatch(struct x12_conn *c, const uint8_t *frame, size_t len) {
    uint32_t object = 0;
    uint16_t opcode = 0;
    x12_header_peek(frame, &object, &opcode);

    // unknown ids are a client bug, the error names the offending id and
    // the connection stays up
    struct x12_object *o = x12_objects_get(&c->objects, object);
    if (!o) {
        x12_conn_error(c, X12_ERROR_CODE_INVALID_ID, object);
        return 0;
    }
    const x12_msg_desc *msg = x12_msg_find(o->iface, X12_REQUEST, opcode);
    if (!msg) {
        x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
        return 0;
    }
    x12_arg args[X12_MAX_ARGS];
    uint32_t obj_id = 0;
    uint64_t ts = 0;
    if (x12_decode(o->iface, msg, frame, len, &obj_id, &ts, args,
                   X12_MAX_ARGS) != X12_OK) {
        x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
        return 0;
    }

    switch (o->kind) {
    case OBJ_CONNECTION:
        return x12_handle_connection(c, object, msg, args);
    case OBJ_REGISTRY:
        return x12_handle_registry(c, object, msg, args);
    case OBJ_COMPOSITOR:
        return x12_handle_compositor(c, object, msg, args);
    case OBJ_BUFFER_MANAGER:
        return x12_handle_buffer_manager(c, object, msg, args);
    case OBJ_SHM_POOL:
        return x12_handle_shm_pool(c, object, msg, args);
    case OBJ_BUFFER:
        return x12_handle_buffer(c, object, msg, args);
    case OBJ_SURFACE:
        return x12_handle_surface(c, object, msg, args);
    default:
        // no request handler lives on this kind yet
        x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, object);
        return 0;
    }
}

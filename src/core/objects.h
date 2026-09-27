// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#ifndef X12_OBJECTS_H
#define X12_OBJECTS_H

#include <stdint.h>

#include "x12_wire.h"
#include "limits.h"
#include "state.h"

// object kinds the core skeleton knows, later parties hang their state
// off these
enum x12_obj_kind {
    OBJ_FREE = 0,
    OBJ_CONNECTION,
    OBJ_REGISTRY,
    OBJ_CALLBACK,
    OBJ_COMPOSITOR,
    OBJ_BUFFER_MANAGER,
    OBJ_SHM_POOL,
    OBJ_BUFFER,
    OBJ_SURFACE,
    OBJ_POSITIONER,
    OBJ_TOPLEVEL,
    OBJ_POPUP,
    OBJ_POINTER,
    OBJ_KEYBOARD,
    OBJ_TOUCH,
    OBJ_SEAT,
    OBJ_SCREENCOPY,
    OBJ_DATA_DEVICE,
    OBJ_DATA_SOURCE,
    OBJ_DATA_OFFER,
};

struct x12_object {
    enum x12_obj_kind kind;
    const x12_iface_desc *iface;
    union {
        struct x12_pool_state pool;
        struct x12_buffer_state buffer;
        struct x12_surface_state surface;
        struct x12_positioner_state positioner;
        struct x12_toplevel_state toplevel;
        struct x12_popup_state popup;
        struct x12_data_source_state data_source;
        struct x12_data_offer_state data_offer;
    } as;
};

struct x12_objects {
    struct x12_object slots[X12_MAX_OBJECTS];
};

void x12_objects_init(struct x12_objects *t);

// client allocated ids, the server only checks the range and refuses a
// second life for one id within the connection
int x12_objects_alloc(struct x12_objects *t, uint32_t id,
                      enum x12_obj_kind kind, const x12_iface_desc *iface);

void x12_objects_free(struct x12_objects *t, uint32_t id);

// ids outside the table read as NULL, callers turn that into invalid-id
struct x12_object *x12_objects_get(struct x12_objects *t, uint32_t id);

#endif

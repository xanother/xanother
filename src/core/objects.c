// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#include "objects.h"

#include <string.h>

void x12_objects_init(struct x12_objects *t) {
    memset(t, 0, sizeof *t);
}

int x12_objects_alloc(struct x12_objects *t, uint32_t id,
                      enum x12_obj_kind kind, const x12_iface_desc *iface) {
    // id zero is the null object, ids beyond the table or already taken
    // are client bugs, the caller answers with invalid-id
    if (id == 0 || id >= X12_MAX_OBJECTS || kind == OBJ_FREE) {
        return -1;
    }
    if (t->slots[id].kind != OBJ_FREE) {
        return -1;
    }
    t->slots[id].kind = kind;
    t->slots[id].iface = iface;
    return 0;
}

void x12_objects_free(struct x12_objects *t, uint32_t id) {
    if (id == 0 || id >= X12_MAX_OBJECTS) {
        return;
    }
    t->slots[id].kind = OBJ_FREE;
    t->slots[id].iface = 0;
}

struct x12_object *x12_objects_get(struct x12_objects *t, uint32_t id) {
    if (id == 0 || id >= X12_MAX_OBJECTS) {
        return 0;
    }
    if (t->slots[id].kind == OBJ_FREE) {
        return 0;
    }
    return &t->slots[id];
}

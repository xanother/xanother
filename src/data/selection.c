// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// the clipboard and primary selection store, see selection.h

#include "selection.h"

#include <string.h>

#include "objects.h"
#include "x12_protocol.h"

// slot 0 is the clipboard, slot 1 the primary selection
#define X12_SELECTION_SLOTS 2

static struct {
    int active;
    uint32_t generation;
    struct x12_conn *owner;
    uint32_t source_id;
} sel[X12_SELECTION_SLOTS];

static struct x12_conn *table;
static size_t table_n;

void x12_selection_init(struct x12_conn *conns, size_t n) {
    memset(sel, 0, sizeof sel);
    table = conns;
    table_n = n;
}

void x12_selection_claim(int primary, struct x12_conn *c, uint32_t source_id,
                         struct x12_selection_change *out) {
    struct x12_selection_change ch = { 0 };
    int slot = primary ? 1 : 0;
    // a claim always changes the selection, even onto a previously empty
    // slot: the generation must move and every device must learn it
    ch.changed = 1;
    if (sel[slot].active) {
        // the previous holder loses the slot even when it is the same
        // client re-claiming with a fresh source object, the caller
        // decides whether that deserves a cancelled event
        ch.prev_owner = sel[slot].owner;
        ch.prev_source = sel[slot].source_id;
    }
    sel[slot].active = 1;
    sel[slot].owner = c;
    sel[slot].source_id = source_id;
    sel[slot].generation++;
    if (out) {
        *out = ch;
    }
}

void x12_selection_clear(int primary, struct x12_selection_change *out) {
    struct x12_selection_change ch = { 0 };
    int slot = primary ? 1 : 0;
    // clearing an already empty slot is a no-op, no generation move and
    // nothing to announce
    if (sel[slot].active) {
        ch.prev_owner = sel[slot].owner;
        ch.prev_source = sel[slot].source_id;
        ch.changed = 1;
        sel[slot].generation++;
    }
    sel[slot].active = 0;
    sel[slot].owner = 0;
    sel[slot].source_id = 0;
    if (out) {
        *out = ch;
    }
}

void x12_selection_owner(int primary, struct x12_conn **owner,
                         uint32_t *source_id) {
    int slot = primary ? 1 : 0;
    if (sel[slot].active && sel[slot].owner && sel[slot].owner->fd >= 0 &&
        !sel[slot].owner->dead) {
        *owner = sel[slot].owner;
        *source_id = sel[slot].source_id;
    } else {
        // a dead owner releases its claim lazily, teardown clears it but
        // every read path checks liveness first
        *owner = 0;
        *source_id = 0;
    }
}

uint32_t x12_selection_generation(int primary) {
    return sel[primary ? 1 : 0].generation;
}

// every offer pinned to the old generation is dead, the object goes
// with its finished event so a late receive refuses loudly
static void finish_offers(int primary) {
    uint32_t gen = x12_selection_generation(primary);
    const x12_iface_desc *io = x12_iface_find("x12_data_offer");
    for (size_t i = 0; i < table_n; i++) {
        struct x12_conn *cc = &table[i];
        if (cc->fd < 0) {
            continue;
        }
        for (uint32_t id = 1; id < X12_MAX_OBJECTS; id++) {
            struct x12_object *o = x12_objects_get(&cc->objects, id);
            if (!o || o->kind != OBJ_DATA_OFFER) {
                continue;
            }
            if (o->as.data_offer.primary != primary ||
                o->as.data_offer.generation == gen) {
                continue;
            }
            if (x12_conn_event(cc, io, "finished", id, 0, 0) != 0) {
                continue;
            }
            x12_objects_free(&cc->objects, id);
        }
    }
}

static void broadcast(int primary) {
    const x12_iface_desc *id_ = x12_iface_find("x12_data_device");
    x12_arg gen[1] = { { .u64 = x12_selection_generation(primary) } };
    for (size_t i = 0; i < table_n; i++) {
        struct x12_conn *cc = &table[i];
        if (cc->fd < 0 || cc->dead || !cc->data_device_id) {
            continue;
        }
        x12_conn_event(cc, id_, primary ? "primary" : "selection",
                       cc->data_device_id, gen, 1);
    }
}

void x12_selection_after_change(const struct x12_selection_change *chg,
                                int primary, struct x12_conn *claimer,
                                uint32_t claimed_source) {
    if (chg->changed && chg->prev_owner && chg->prev_owner->fd >= 0 &&
        !chg->prev_owner->dead &&
        !(chg->prev_owner == claimer && chg->prev_source == claimed_source)) {
        x12_conn_event(chg->prev_owner, x12_iface_find("x12_data_source"),
                       "cancelled", chg->prev_source, 0, 0);
    }
    if (chg->changed) {
        finish_offers(primary);
        broadcast(primary);
    }
}

void x12_selection_release_owner(struct x12_conn *c) {
    for (int primary = 0; primary < X12_SELECTION_SLOTS; primary++) {
        struct x12_conn *owner;
        uint32_t sid;
        x12_selection_owner(primary, &owner, &sid);
        if (owner != c) {
            continue;
        }
        struct x12_selection_change chg;
        x12_selection_clear(primary, &chg);
        // no cancelled event, the owner is past hearing anything. the
        // readers still learn the slot emptied
        if (chg.changed) {
            finish_offers(primary);
            broadcast(primary);
        }
    }
}

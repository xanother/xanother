// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#include "input.h"

#include <string.h>

#include "x12_protocol.h"
#include "objects.h"
#include "windows.h"

#define IN_QUEUE_CAP 256
#define FIXED_ONE 256

static struct x12_input_event queue[IN_QUEUE_CAP];
static size_t q_head, q_tail;

static int32_t ptr_x, ptr_y;
static uint32_t ev_serial = 1;

static const x12_iface_desc *if_pointer, *if_keyboard, *if_popup;
static uint32_t st_pressed, st_released, ax_v, ax_h;
static int resolved;

struct x12_target {
    struct x12_conn *client;
    uint32_t surface_id;
};

static struct x12_target hover_t, kb_t, grab_t;
static int has_hover, has_kb, has_grab;

// issued input serials with their owner, a popup grab must reference a
// button press the very same client actually received, anything else is
// a bad value. the ring keeps the recent history, input order is a
// few dozen events per frame at worst
static struct x12_seen_serial {
    uint32_t serial;
    struct x12_conn *c;
    uint8_t press;
} seen_serials[64];
static size_t seen_next;

static void note_serial(uint32_t serial, struct x12_conn *c, int press) {
    struct x12_seen_serial *s = &seen_serials[seen_next++ % 64];
    s->serial = serial;
    s->c = c;
    s->press = (uint8_t)(press != 0);
}

int x12_input_serial_valid(struct x12_conn *c, uint32_t serial) {
    for (size_t i = 0; i < 64; i++) {
        if (seen_serials[i].c == c && seen_serials[i].press &&
            seen_serials[i].serial == serial) {
            return 1;
        }
    }
    return 0;
}

void x12_input_set_grab(struct x12_conn *c, uint32_t surface_id) {
    grab_t.client = c;
    grab_t.surface_id = surface_id;
    has_grab = 1;
}

void x12_input_push(const struct x12_input_event *e) {
    size_t next = (q_tail + 1) % IN_QUEUE_CAP;
    if (next == q_head) {
        // queue full means the host is flooding, drop the newest event
        return;
    }
    queue[q_tail] = *e;
    q_tail = next;
}

static int pop(struct x12_input_event *e) {
    if (q_head == q_tail) {
        return 0;
    }
    *e = queue[q_head];
    q_head = (q_head + 1) % IN_QUEUE_CAP;
    return 1;
}

// table lookup against the generated tables, the idl order is the value
// order so the router never hardcodes wire numbers
static int enum_value(const char *ename, const char *symbol, uint32_t *out) {
    for (size_t i = 0; i < x12_enums_count; i++) {
        const x12_enum_desc *e = &x12_enums[i];
        if (strcmp(e->name, ename) != 0) {
            continue;
        }
        for (uint8_t s = 0; s < e->symbol_count; s++) {
            if (strcmp(e->symbols[s], symbol) == 0) {
                *out = s;
                return 0;
            }
        }
    }
    return -1;
}

static void resolve_tables(void) {
    if (resolved) {
        return;
    }
    if_pointer = x12_iface_find("x12_pointer");
    if_keyboard = x12_iface_find("x12_keyboard");
    if_popup = x12_iface_find("x12_popup");
    if (enum_value("button_state", "pressed", &st_pressed) != 0 ||
        enum_value("button_state", "released", &st_released) != 0 ||
        enum_value("axis_kind", "vertical-scroll", &ax_v) != 0 ||
        enum_value("axis_kind", "horizontal-scroll", &ax_h) != 0) {
        // missing tables are a build defect, the router degrades to silence
        if_pointer = if_keyboard = if_popup = 0;
    }
    resolved = 1;
}

static int target_alive(const struct x12_target *t) {
    if (!t->client || t->client->fd < 0) {
        return 0;
    }
    return x12_windows_find(t->client, t->surface_id) >= 0;
}

static int same_target(const struct x12_target *a, const struct x12_target *b) {
    return a->client == b->client && a->surface_id == b->surface_id;
}

static void send_pointer(const struct x12_target *t, const char *evt,
                         const x12_arg *args, size_t n) {
    if (x12_windows_find(t->client, t->surface_id) >= 0 &&
        t->client->fd >= 0 && t->client->pointer_id) {
        x12_conn_event(t->client, if_pointer, evt, t->client->pointer_id,
                       args, n);
    }
}

static void send_keyboard(const struct x12_target *t, const char *evt,
                          const x12_arg *args, size_t n) {
    if (x12_windows_find(t->client, t->surface_id) >= 0 &&
        t->client->fd >= 0 && t->client->keyboard_id) {
        x12_conn_event(t->client, if_keyboard, evt, t->client->keyboard_id,
                       args, n);
    }
}

// modifier tracking, see the modifiers event comment in the protocol
// spec for the mask convention. depressed follows the physically held
// keys, locked follows the toggles, latched and group stay unused until
// a layout switching source exists
static uint32_t mods_depressed, mods_locked;

static void send_modifiers(const struct x12_target *t) {
    uint32_t ser = ev_serial++;
    x12_arg args[5] = {
        { .u64 = ser },
        { .u64 = mods_depressed },
        { .u64 = mods_locked },
        { .u64 = 0 },
        { .u64 = 0 },
    };
    send_keyboard(t, "modifiers", args, 5);
    note_serial(ser, t->client, 0);
}

// evdev codes to xkb canonical modifier bits, pressed means the physical
// state changed; toggles flip on press like real keyboards do
static void track_modifiers(uint32_t key, uint32_t pressed) {
    uint32_t bit = 0;
    int toggle = 0;
    switch (key) {
    case 29: case 97:  bit = 1u << 2; break;  // left/right control
    case 42: case 54:  bit = 1u << 0; break;  // left/right shift
    case 56: case 100: bit = 1u << 3; break;  // left/right alt
    case 125: case 126: bit = 1u << 6; break; // left/right logo
    case 58: bit = 1u << 1; toggle = 1; break; // caps lock
    case 69: bit = 1u << 4; toggle = 1; break; // num lock
    default: return;
    }
    uint32_t old_d = mods_depressed, old_l = mods_locked;
    if (toggle) {
        if (pressed) {
            mods_locked ^= bit;
        }
    } else if (pressed) {
        mods_depressed |= bit;
    } else {
        mods_depressed &= ~bit;
    }
    if (old_d != mods_depressed || old_l != mods_locked) {
        send_modifiers(&kb_t);
    }
}

static struct x12_target target_at(int32_t x, int32_t y) {
    struct x12_target t = { 0, 0 };
    int idx = x12_windows_top_at(x, y);
    if (idx >= 0) {
        const struct x12_winview *v = x12_windows_at(idx);
        t.client = v->client;
        t.surface_id = v->surface_id;
    }
    return t;
}

// pointer enter or leave against the view the pointer is over
static void send_hover_transition(struct x12_target new_t) {
    int has_new = new_t.client != 0;
    int same = has_new == has_hover && (!has_new || same_target(&new_t, &hover_t));
    if (same) {
        return;
    }
    if (has_hover && target_alive(&hover_t)) {
        uint32_t ser = ev_serial++;
        x12_arg leave[2] = { { .u64 = ser },
                             { .u64 = hover_t.surface_id } };
        send_pointer(&hover_t, "leave", leave, 2);
        note_serial(ser, hover_t.client, 0);
    }
    if (has_new && target_alive(&new_t)) {
        int idx = x12_windows_find(new_t.client, new_t.surface_id);
        const struct x12_winview *v = x12_windows_at(idx);
        uint32_t ser = ev_serial++;
        x12_arg enter[4] = {
            { .u64 = ser },
            { .u64 = v->surface_id },
            { .i64 = (int64_t)(ptr_x - v->x) * FIXED_ONE },
            { .i64 = (int64_t)(ptr_y - v->y) * FIXED_ONE },
        };
        send_pointer(&new_t, "enter", enter, 4);
        note_serial(ser, new_t.client, 0);
    }
    hover_t = new_t;
    has_hover = has_new;
}

static void send_pointer_motion(const struct x12_target *t) {
    int idx = x12_windows_find(t->client, t->surface_id);
    if (idx < 0) {
        return;
    }
    const struct x12_winview *v = x12_windows_at(idx);
    x12_arg motion[3] = {
        { .u64 = v->surface_id },
        { .i64 = (int64_t)(ptr_x - v->x) * FIXED_ONE },
        { .i64 = (int64_t)(ptr_y - v->y) * FIXED_ONE },
    };
    send_pointer(t, "motion", motion, 3);
}

// keyboard focus moves to the toplevel under the press, popups keep the
// focus of their parent and raising happens on focus, not on hover
static void set_keyboard_focus(struct x12_target t) {
    int has_new = t.client != 0;
    if ((has_new == has_kb) && (!has_new || same_target(&t, &kb_t))) {
        return;
    }
    if (has_kb && target_alive(&kb_t)) {
        uint32_t ser = ev_serial++;
        x12_arg leave[2] = { { .u64 = ser },
                             { .u64 = kb_t.surface_id } };
        send_keyboard(&kb_t, "leave", leave, 2);
        note_serial(ser, kb_t.client, 0);
    }
    if (has_new && target_alive(&t)) {
        uint32_t ser = ev_serial++;
        x12_arg enter[3] = {
            { .u64 = ser },
            { .u64 = t.surface_id },
            { .array = 0, .array_len = 0 },
        };
        send_keyboard(&t, "enter", enter, 3);
        note_serial(ser, t.client, 0);
        // the client learns the modifier state at focus time, there is
        // no other way to catch up mid session
        send_modifiers(&t);
    }
    kb_t = t;
    has_kb = has_new;
}

static void dismiss_popup(const struct x12_target *t) {
    int idx = x12_windows_find(t->client, t->surface_id);
    if (idx < 0) {
        if (has_grab && same_target(t, &grab_t)) {
            has_grab = 0;
        }
        return;
    }
    const struct x12_winview *v = x12_windows_at(idx);
    struct x12_object *surf =
        x12_objects_get(&t->client->objects, t->surface_id);
    if (surf && surf->kind == OBJ_SURFACE &&
        surf->as.surface.role_kind == ROLE_POPUP) {
        x12_conn_event(t->client, if_popup, "dismiss",
                       surf->as.surface.role_id, 0, 0);
    }
    if (has_grab && same_target(t, &grab_t)) {
        has_grab = 0;
    }
    if (has_hover && same_target(t, &hover_t)) {
        has_hover = 0;
    }
    x12_windows_remove(idx);
    (void)v;
}

static void route_motion(int32_t x, int32_t y) {
    ptr_x = x;
    ptr_y = y;
    if (has_grab) {
        // a grab holds the pointer even outside the popup bounds, local
        // coordinates may go negative and that is valid fixed24p8
        send_hover_transition(grab_t);
        send_pointer_motion(&grab_t);
        return;
    }
    struct x12_target t = target_at(x, y);
    send_hover_transition(t);
    if (t.client) {
        send_pointer_motion(&t);
    }
}

static void route_button(uint32_t button, uint32_t pressed) {
    struct x12_target t = has_grab ? grab_t : target_at(ptr_x, ptr_y);
    if (has_grab && pressed && t.client) {
        int idx = x12_windows_find(t.client, t.surface_id);
        const struct x12_winview *v = x12_windows_at(idx);
        int inside = ptr_x >= v->x && ptr_y >= v->y &&
                     (uint32_t)(ptr_x - v->x) < v->width &&
                     (uint32_t)(ptr_y - v->y) < v->height;
        if (!inside) {
            // press outside an armed popup dismisses it, the press itself
            // is then rerouted as if the popup never held the pointer
            dismiss_popup(&grab_t);
            t = target_at(ptr_x, ptr_y);
        }
    }
    if (!t.client) {
        return;
    }
    uint32_t ser = ev_serial++;
    x12_arg args[3] = {
        { .u64 = ser },
        { .u64 = button },
        { .u64 = pressed ? st_pressed : st_released },
    };
    send_pointer(&t, "button", args, 3);
    note_serial(ser, t.client, pressed);
    if (!pressed) {
        return;
    }
    // click to focus on toplevels only, popups keep their parent focus
    struct x12_object *surf =
        x12_objects_get(&t.client->objects, t.surface_id);
    if (surf && surf->kind == OBJ_SURFACE &&
        surf->as.surface.role_kind == ROLE_TOPLEVEL) {
        set_keyboard_focus(t);
        int idx = x12_windows_find(t.client, t.surface_id);
        x12_windows_raise_toplevel(idx);
    }
}

static void route_key(uint32_t key, uint32_t pressed) {
    if (!has_kb || !target_alive(&kb_t)) {
        return;
    }
    uint32_t ser = ev_serial++;
    x12_arg args[3] = {
        { .u64 = ser },
        { .u64 = key },
        { .u64 = pressed ? st_pressed : st_released },
    };
    send_keyboard(&kb_t, "key", args, 3);
    note_serial(ser, kb_t.client, 0);
    track_modifiers(key, pressed);
}

// discrete wheel steps carry the axis event and an immediate frame, the
// frame delimits the scroll group so clients may act after it. a smooth
// scrolling source would emit axis_stop, a notched wheel never does
static void route_axis(uint32_t axis, int32_t value) {
    struct x12_target t = has_grab ? grab_t : target_at(ptr_x, ptr_y);
    if (!t.client) {
        return;
    }
    x12_arg ax[2] = {
        { .u64 = axis == ax_v ? ax_v : ax_h },
        { .i64 = value },
    };
    send_pointer(&t, "axis", ax, 2);
    send_pointer(&t, "frame", 0, 0);
}

void x12_input_dispatch(void) {
    resolve_tables();
    if (!if_pointer || !if_keyboard) {
        return;
    }
    // stale focus entries vanish first, a dead client must not receive
    // events that would overflow its out buffer after close
    if (has_hover && !target_alive(&hover_t)) {
        has_hover = 0;
    }
    if (has_kb && !target_alive(&kb_t)) {
        has_kb = 0;
    }
    if (has_grab && !target_alive(&grab_t)) {
        has_grab = 0;
    }
    struct x12_input_event e;
    while (pop(&e)) {
        switch (e.kind) {
        case IN_MOTION:
            route_motion(e.x, e.y);
            break;
        case IN_BUTTON:
            route_button(e.code, e.pressed);
            break;
        case IN_KEY:
            route_key(e.code, e.pressed);
            break;
        case IN_AXIS:
            route_axis(e.axis, e.value);
            break;
        }
    }
}

void x12_input_pointer(int32_t *x, int32_t *y, struct x12_conn **c) {
    *x = ptr_x;
    *y = ptr_y;
    // the grab target wins, the cursor follows the same routing rules
    // the events themselves do
    struct x12_target t = has_grab ? grab_t : hover_t;
    *c = (has_grab || has_hover) && t.client && target_alive(&t) ? t.client : 0;
}

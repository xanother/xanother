// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// evdev input source, backend independent on purpose: seat managers hand
// input fds to the compositor process, the output backend stays output
// only. the decoder understands the small subset of the evdev grammar the
// skeleton needs, relative pointers, absolute pointers from touchpads,
// the first three buttons and plain keys, everything else is ignored.
// linux evdev codes are the protocol key codes, so no translation table
// exists anywhere in the server

#include "source.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <linux/input.h>

#include "input.h"
#include "windows.h"

#define X12_EVDEV_MAX 8

struct x12_evdev {
    int fd;
    // absolute devices report their own coordinate range, relative
    // devices leave has_abs clear and accumulate deltas instead
    int has_abs;
    int32_t abs_x_min, abs_x_max, abs_y_min, abs_y_max;
    int32_t cur_x, cur_y;
    int abs_dirty;
};

static struct x12_evdev devs[X12_EVDEV_MAX];
static int n_devs;

// relative motion accumulates between SYN REPORT batches, that is how
// real evdev streams carry a single pointer move
static int32_t rel_x, rel_y;

int x12_evdev_add(const char *path) {
    if (n_devs >= X12_EVDEV_MAX) {
        return -1;
    }
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    struct x12_evdev *d = &devs[n_devs];
    memset(d, 0, sizeof *d);
    d->fd = fd;
    struct input_absinfo ax, ay;
    if (ioctl(fd, EVIOCGABS(ABS_X), &ax) == 0 &&
        ioctl(fd, EVIOCGABS(ABS_Y), &ay) == 0 && ax.maximum > ax.minimum &&
        ay.maximum > ay.minimum) {
        d->has_abs = 1;
        d->abs_x_min = ax.minimum;
        d->abs_x_max = ax.maximum;
        d->abs_y_min = ay.minimum;
        d->abs_y_max = ay.maximum;
        d->cur_x = ax.minimum;
        d->cur_y = ay.minimum;
    }
    n_devs++;
    return 0;
}

int x12_evdev_fill(struct pollfd *pfds, int max) {
    int n = n_devs < max ? n_devs : max;
    for (int i = 0; i < n; i++) {
        pfds[i].fd = devs[i].fd;
        pfds[i].events = POLLIN;
        pfds[i].revents = 0;
    }
    return n;
}

static void push_motion(void) {
    if (rel_x == 0 && rel_y == 0) {
        return;
    }
    // the pointer starts at the output center and accumulates deltas,
    // relative devices carry no absolute notion of position
    static int centered;
    static int32_t px, py;
    if (!centered) {
        uint32_t w, h;
        x12_windows_output(&w, &h);
        px = (int32_t)(w / 2);
        py = (int32_t)(h / 2);
        centered = 1;
    }
    px += rel_x;
    py += rel_y;
    rel_x = rel_y = 0;
    uint32_t w, h;
    x12_windows_output(&w, &h);
    if (px < 0) {
        px = 0;
    }
    if (py < 0) {
        py = 0;
    }
    if (w && px >= (int32_t)w) {
        px = (int32_t)w - 1;
    }
    if (h && py >= (int32_t)h) {
        py = (int32_t)h - 1;
    }
    struct x12_input_event e;
    memset(&e, 0, sizeof e);
    e.kind = IN_MOTION;
    e.x = px;
    e.y = py;
    x12_input_push(&e);
}

// absolute devices carry their own position, the output range maps
// linearly onto it; int64 because a wide sensor range times a wide
// output overflows int32 before the division brings it back
static void push_abs_motion(struct x12_evdev *d) {
    uint32_t w, h;
    x12_windows_output(&w, &h);
    if (!w || !h) {
        return;
    }
    int64_t x = (int64_t)(d->cur_x - d->abs_x_min) * (int64_t)(w - 1) /
                (int64_t)(d->abs_x_max - d->abs_x_min);
    int64_t y = (int64_t)(d->cur_y - d->abs_y_min) * (int64_t)(h - 1) /
                (int64_t)(d->abs_y_max - d->abs_y_min);
    d->abs_dirty = 0;
    struct x12_input_event e;
    memset(&e, 0, sizeof e);
    e.kind = IN_MOTION;
    e.x = (int32_t)x;
    e.y = (int32_t)y;
    x12_input_push(&e);
}

// button indices follow the x11 numbering the protocol adopted,
// 1 left, 2 right, 3 middle
static void push_button(uint16_t code, int32_t value) {
    uint32_t button = 0;
    if (code == BTN_LEFT || code == BTN_TOUCH) {
        // a touchpad tap is a click, the finger carries no button
        button = 1;
    } else if (code == BTN_RIGHT) {
        button = 2;
    } else if (code == BTN_MIDDLE) {
        button = 3;
    } else {
        return;
    }
    struct x12_input_event e;
    memset(&e, 0, sizeof e);
    e.kind = IN_BUTTON;
    e.code = button;
    e.pressed = value != 0;
    x12_input_push(&e);
}

static void push_key(uint16_t code, int32_t value) {
    if (value == 2) {
        // autorepeat belongs to the keyboard model, not to the decoder
        return;
    }
    struct x12_input_event e;
    memset(&e, 0, sizeof e);
    e.kind = IN_KEY;
    e.code = code;
    e.pressed = value != 0;
    x12_input_push(&e);
}

// wheel notches map to one fixed24p8 unit per click, positive is down
// for the vertical wheel and right for the horizontal one, the same
// direction convention the protocol axis events document
#define WHEEL_ONE 256

static uint32_t ax_vertical, ax_horizontal;
static int ax_resolved;

static void push_axis(uint16_t code, int32_t value) {
    // the axis_kind enum values resolve once from the generated tables,
    // missing symbols are a build defect and drop the event
    if (!ax_resolved) {
        if (x12_input_enum("axis_kind", "vertical-scroll", &ax_vertical) != 0 ||
            x12_input_enum("axis_kind", "horizontal-scroll", &ax_horizontal) != 0) {
            return;
        }
        ax_resolved = 1;
    }
    struct x12_input_event e;
    memset(&e, 0, sizeof e);
    e.kind = IN_AXIS;
    if (code == REL_WHEEL) {
        e.axis = ax_vertical;
        e.value = value * WHEEL_ONE;
    } else if (code == REL_HWHEEL) {
        e.axis = ax_horizontal;
        e.value = value * WHEEL_ONE;
    } else {
        return;
    }
    x12_input_push(&e);
}

static void feed(struct x12_evdev *d, const struct input_event *ev) {
    switch (ev->type) {
    case EV_REL:
        if (ev->code == REL_X) {
            rel_x += ev->value;
        } else if (ev->code == REL_Y) {
            rel_y += ev->value;
        } else if (ev->code == REL_WHEEL || ev->code == REL_HWHEEL) {
            push_axis(ev->code, ev->value);
        }
        break;
    case EV_ABS:
        if (!d->has_abs) {
            break;
        }
        if (ev->code == ABS_X) {
            d->cur_x = ev->value;
            d->abs_dirty = 1;
        } else if (ev->code == ABS_Y) {
            d->cur_y = ev->value;
            d->abs_dirty = 1;
        }
        break;
    case EV_KEY:
        if (ev->code == BTN_LEFT || ev->code == BTN_RIGHT ||
            ev->code == BTN_MIDDLE || ev->code == BTN_TOUCH) {
            push_button(ev->code, ev->value);
        } else {
            push_key(ev->code, ev->value);
        }
        break;
    case EV_SYN:
        if (ev->code == SYN_REPORT) {
            if (d->abs_dirty) {
                push_abs_motion(d);
            } else {
                push_motion();
            }
        }
        break;
    default:
        break;
    }
}

void x12_evdev_pump(const struct pollfd *pfds, int count) {
    for (int i = 0; i < count && i < n_devs; i++) {
        if (!(pfds[i].revents & POLLIN)) {
            continue;
        }
        uint8_t buf[4096];
        ssize_t n;
        while ((n = read(devs[i].fd, buf, sizeof buf)) > 0) {
            // one read usually carries a whole report batch
            size_t per = sizeof(struct input_event);
            for (ssize_t off = 0; off + (ssize_t)per <= n;
                 off += (ssize_t)per) {
                struct input_event ev;
                memcpy(&ev, buf + off, per);
                feed(&devs[i], &ev);
            }
        }
        // EAGAIN ends the batch, anything else is a dead device and
        // poll stops reporting it anyway
    }
}

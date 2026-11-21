// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT
// Copyright (c) fluent1s (akemi) <m1st1kad3ad1y@gmail.com>

// libinput input source: a udev seat feeds the same router queue the
// raw evdev decoder and the file source use. libinput brings the parts
// the raw decoder cannot do: per device calibration, touchpad palm
// rejection, pointer acceleration, hotplug of devices appearing on the
// seat. built only when the toolchain had libinput at build time;
// without it the entry points compile to a polite stub and --evdev
// stays the input path.

#include "source.h"

#ifdef X12_HAVE_LIBINPUT

#include <errno.h>
#include <fcntl.h>
#include <libinput.h>
#include <libudev.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "input.h"
#include "windows.h"

static struct libinput *li;
static struct udev *li_udev;
static int li_running;

static int open_restricted(const char *path, int flags, void *user) {
    (void)user;
    int fd = open(path, flags | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "x12-server: libinput cannot open %s: %s\n", path,
                strerror(errno));
    }
    return fd;
}

static void close_restricted(int fd, void *user) {
    (void)user;
    close(fd);
}

static const struct libinput_interface li_iface = {
    .open_restricted = open_restricted,
    .close_restricted = close_restricted,
};

int x12_libinput_start(void) {
    if (li) {
        return 0;
    }
    li_udev = udev_new();
    if (!li_udev) {
        return -1;
    }
    li = libinput_udev_create_context(&li_iface, NULL, li_udev);
    if (!li) {
        udev_unref(li_udev);
        li_udev = NULL;
        return -1;
    }
    if (libinput_udev_assign_seat(li, "seat0") != 0) {
        libinput_unref(li);
        li = NULL;
        udev_unref(li_udev);
        li_udev = NULL;
        return -1;
    }
    li_running = 1;
    return libinput_get_fd(li) >= 0 ? 0 : -1;
}

int x12_libinput_fill(struct pollfd *pfds, int max) {
    if (!li || max < 1) {
        return 0;
    }
    pfds[0].fd = libinput_get_fd(li);
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    return 1;
}

// the pointer position the relative deltas accumulate onto, the evdev
// decoder starts it at the output center and this one shares the
// behavior: relative devices carry no absolute notion of position
static int centered;
static int32_t px, py;

static void push_rel_motion(double dx, double dy) {
    if (!centered) {
        uint32_t w, h;
        x12_windows_output(&w, &h);
        px = (int32_t)(w / 2);
        py = (int32_t)(h / 2);
        centered = 1;
    }
    px += (int32_t)dx;
    py += (int32_t)dy;
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

static void push_abs_motion(struct libinput_event_pointer *p) {
    uint32_t w, h;
    x12_windows_output(&w, &h);
    if (!w || !h) {
        return;
    }
    struct x12_input_event e;
    memset(&e, 0, sizeof e);
    e.kind = IN_MOTION;
    e.x = (int32_t)libinput_event_pointer_get_absolute_x_transformed(p, w);
    e.y = (int32_t)libinput_event_pointer_get_absolute_y_transformed(p, h);
    px = e.x;
    py = e.y;
    centered = 1;
    x12_input_push(&e);
}

// button indices follow the x11 numbering the protocol adopted,
// 1 left, 2 right, 3 middle, the same map the evdev decoder uses
static void push_button(uint32_t code, int pressed) {
    uint32_t button = 0;
    if (code == BTN_LEFT || code == BTN_TOUCH) {
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
    e.pressed = pressed;
    x12_input_push(&e);
}

static void push_key(uint32_t code, int pressed) {
    struct x12_input_event e;
    memset(&e, 0, sizeof e);
    e.kind = IN_KEY;
    e.code = code;
    e.pressed = pressed;
    x12_input_push(&e);
}

// wheel notches map to one fixed24p8 unit per click, positive is down
// for the vertical wheel and right for the horizontal one, the same
// direction convention the protocol axis events document
#define WHEEL_ONE 256

static uint32_t ax_vertical, ax_horizontal;
static int ax_resolved;

static void push_axis(struct libinput_event_pointer *p) {
    if (!ax_resolved) {
        if (x12_input_enum("axis_kind", "vertical-scroll", &ax_vertical) !=
                0 ||
            x12_input_enum("axis_kind", "horizontal-scroll",
                           &ax_horizontal) != 0) {
            return;
        }
        ax_resolved = 1;
    }
    struct x12_input_event e;
    memset(&e, 0, sizeof e);
    e.kind = IN_AXIS;
    if (libinput_event_pointer_has_axis(
            p, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)) {
        double v = libinput_event_pointer_get_axis_value_discrete(
            p, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
        if (v == 0) {
            return;
        }
        e.axis = ax_vertical;
        e.value = (int32_t)(v * WHEEL_ONE);
    } else if (libinput_event_pointer_has_axis(
                   p, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL)) {
        double v = libinput_event_pointer_get_axis_value_discrete(
            p, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
        if (v == 0) {
            return;
        }
        e.axis = ax_horizontal;
        e.value = (int32_t)(v * WHEEL_ONE);
    } else {
        return;
    }
    x12_input_push(&e);
}

void x12_libinput_pump(const struct pollfd *pfds, int count) {
    (void)count;
    if (!li || !li_running) {
        return;
    }
    // the fd may carry device arrivals without readable events, a
    // quiet fd costs one dispatch round and nothing else
    if (pfds && !(pfds[0].revents & POLLIN)) {
        return;
    }
    if (libinput_dispatch(li) != 0) {
        // EPIPE is the udev monitor dying, the seat is gone; stop
        // pumping instead of spinning on a dead context
        li_running = 0;
        fprintf(stderr, "x12-server: libinput dispatch died, evdev only\n");
        return;
    }
    struct libinput_event *ev;
    while ((ev = libinput_get_event(li)) != NULL) {
        enum libinput_event_type t = libinput_event_get_type(ev);
        switch (t) {
        case LIBINPUT_EVENT_POINTER_MOTION: {
            struct libinput_event_pointer *p =
                libinput_event_get_pointer_event(ev);
            if (p) {
                push_rel_motion(libinput_event_pointer_get_dx(p),
                                libinput_event_pointer_get_dy(p));
            }
            break;
        }
        case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: {
            struct libinput_event_pointer *p =
                libinput_event_get_pointer_event(ev);
            if (p) {
                push_abs_motion(p);
            }
            break;
        }
        case LIBINPUT_EVENT_POINTER_BUTTON: {
            struct libinput_event_pointer *p =
                libinput_event_get_pointer_event(ev);
            if (p) {
                push_button(libinput_event_pointer_get_button(p),
                            libinput_event_pointer_get_button_state(p) ==
                                    LIBINPUT_BUTTON_STATE_PRESSED);
            }
            break;
        }
        case LIBINPUT_EVENT_POINTER_AXIS: {
            struct libinput_event_pointer *p =
                libinput_event_get_pointer_event(ev);
            if (p) {
                push_axis(p);
            }
            break;
        }
        case LIBINPUT_EVENT_KEYBOARD_KEY: {
            struct libinput_event_keyboard *k =
                libinput_event_get_keyboard_event(ev);
            if (k) {
                push_key(libinput_event_keyboard_get_key(k),
                         libinput_event_keyboard_get_key_state(k) ==
                                 LIBINPUT_KEY_STATE_PRESSED);
            }
            break;
        }
        default:
            break;
        }
        libinput_event_destroy(ev);
    }
}

void x12_libinput_stop(void) {
    if (li) {
        libinput_unref(li);
        li = NULL;
    }
    if (li_udev) {
        udev_unref(li_udev);
        li_udev = NULL;
    }
    li_running = 0;
}

#else /* X12_HAVE_LIBINPUT */

int x12_libinput_start(void) {
    return -1;
}

int x12_libinput_fill(struct pollfd *pfds, int max) {
    (void)pfds;
    (void)max;
    return 0;
}

void x12_libinput_pump(const struct pollfd *pfds, int count) {
    (void)pfds;
    (void)count;
}

void x12_libinput_stop(void) {
}

#endif /* X12_HAVE_LIBINPUT */

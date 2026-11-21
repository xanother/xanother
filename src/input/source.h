// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT
// Copyright (c) fluent1s (akemi) <m1st1kad3ad1y@gmail.com>

#ifndef X12_SOURCE_H
#define X12_SOURCE_H

#include <poll.h>

// input sources feed the router queue through x12_input_push. all
// sources share the fill and pump shape: fill registers their fds in
// the main poll loop, pump reads what arrived and pushes events

// file source: the server reads event lines from a fifo or a file named
// by --input. the grammar is one event per line, coordinates are global
// canvas coordinates and comments start with a hash:
//   motion <x> <y>
//   button <1|2|3> <press|release>
//   key <code> <press|release>
//   axis <vertical|horizontal> <value>
// a writer that goes away does not kill the source, it reopens and
// keeps listening
int x12_input_file_open(const char *path);
int x12_input_file_fill(struct pollfd *pfds, int max);
void x12_input_file_pump(const struct pollfd *pfds, int count);

// evdev source: reads real devices and pushes the same events. relative
// pointers accumulate between SYN REPORT batches the way evdev streams
// them, absolute devices report their own range mapped onto the canvas
#define X12_EVDEV_MAX 8

int x12_evdev_add(const char *path);
int x12_evdev_fill(struct pollfd *pfds, int max);
void x12_evdev_pump(const struct pollfd *pfds, int count);

// libinput source: a udev seat feeds the same router queue, pointer
// deltas and absolute positions, buttons, keys, wheel notches and the
// touchpad gestures the button model ignores. the build picks it up
// when libinput-dev is present; without it the entry points compile to
// a polite stub and --evdev stays the input path
int x12_libinput_start(void);
int x12_libinput_fill(struct pollfd *pfds, int max);
void x12_libinput_pump(const struct pollfd *pfds, int count);
void x12_libinput_stop(void);

#endif

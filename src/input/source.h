// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

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
int x12_evdev_add(const char *path);
int x12_evdev_fill(struct pollfd *pfds, int max);
void x12_evdev_pump(const struct pollfd *pfds, int count);

#endif

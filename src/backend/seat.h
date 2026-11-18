// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#ifndef X12_SEAT_H
#define X12_SEAT_H

// the seat session behind the kms backend. systemd-logind hands out
// the drm fd through takecontrol/takedevice when there is a session
// and pulls it away again around vt switches, a plain open with the
// set master dance covers the boxes without logind. pause/resume tell
// the backend when its fd must not be touched and when a fresh one
// arrived to rebuild on

struct x12_seat;

struct x12_seat_ops {
    // the master is gone, drop the per-fd state before returning
    void (*on_pause)(void *ud);
    // a fresh fd replaces the old one, rebuild the per-fd state on it
    void (*on_resume)(void *ud, int fd);
};

// opens node through the session when there is one, raw otherwise.
// 0 with *out set on success, -1 when the node refuses to open
int x12_seat_open(struct x12_seat **out, const char *node,
                  const struct x12_seat_ops *ops, void *ud);

// the fd to talk drm through, -1 while paused. it changes on resume
int x12_seat_fd(struct x12_seat *s);

// one call per backend pump: drains the session signals and runs the
// pending pause/resume work in process context
void x12_seat_pump(struct x12_seat *s);

void x12_seat_close(struct x12_seat *s);

#endif

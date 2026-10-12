// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#ifndef X12_FRAME_H
#define X12_FRAME_H

#include <stdint.h>

#include "conn.h"

// surface frame state: pending lands atomically on commit, damage merges
// toward the compositor pass and every release credit belongs to the
// buffer that earned it. a frame retires when the next commit supersedes
// it, its release and frame callbacks wait for the commit fence

void x12_frame_init(struct x12_surface_state *s);

// 0 detaches the buffer, anything else must name a live buffer object
int x12_frame_set_buffer(struct x12_conn *c, struct x12_surface_state *s,
                         uint32_t buffer_id);

// damage is in surface logical coordinates, overflow degrades to the
// whole surface
void x12_frame_damage(struct x12_surface_state *s, int32_t x, int32_t y,
                      uint32_t w, uint32_t h);

// arms the commit fence, calling it again replaces an unconsumed one
int x12_frame_set_fence(struct x12_conn *c, struct x12_surface_state *s,
                        int fence);

// queues a frame callback on the next commit
int x12_frame_add_cb(struct x12_surface_state *s, uint32_t cb_id);

// the commit: pending becomes current atomically, the previous frame
// enters the retire queue and the token comes back as committed. returns
// -1 when the connection died mid way
int x12_frame_commit(struct x12_conn *c, uint32_t surface_id,
                     struct x12_surface_state *s);

// releases retired frames whose fence has signaled, front first so the
// credits leave in commit order
void x12_frame_drain(struct x12_conn *c, struct x12_surface_state *s);

// drain hook for the main loop, walks every surface of the client
void x12_frame_drain_client(struct x12_conn *c);

// the scanout session hands the release credit back when the buffer
// leaves the screen, a stolen frame waits for exactly this
void x12_frame_credit(struct x12_conn *c, uint32_t buffer_id);

// the compositor stamps the frame it just pushed to the host with the
// moment the display took it, the retire later reports it as the
// scanout time in the output present feedback
void x12_frame_presented(struct x12_conn *c, uint32_t surface_id, uint64_t ns,
                         uint32_t outs);

// surface death: outstanding releases go out now, fences close, frame
// callbacks die silently since no frame will complete
void x12_frame_destroy(struct x12_conn *c, struct x12_surface_state *s);

// a buffer object died, dangling references are dropped but the retire
// bookkeeping stays, the release for it is suppressed at emit time
void x12_frame_drop_buffer(struct x12_conn *c, struct x12_surface_state *s,
                           uint32_t buffer_id);

#endif

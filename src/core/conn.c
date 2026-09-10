// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#include "conn.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "x12_protocol.h"

uint64_t x12_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void x12_conn_init(struct x12_conn *c, int fd) {
    memset(c, 0, sizeof *c);
    c->fd = fd;
    x12_objects_init(&c->objects);
    // well known singletons, the client talks to them before anything else
    x12_objects_alloc(&c->objects, X12_OBJ_ID_CONNECTION,
                      OBJ_CONNECTION, x12_iface_find("x12_connection"));
    x12_objects_alloc(&c->objects, X12_OBJ_ID_REGISTRY,
                      OBJ_REGISTRY, x12_iface_find("x12_registry"));
}

void x12_conn_kill(struct x12_conn *c, const char *why) {
    if (c->dead) {
        return;
    }
    c->dead = 1;
    fprintf(stderr, "x12-server: disconnecting client fd %d, %s\n",
            c->fd, why);
}

uint32_t x12_conn_next_serial(struct x12_conn *c) {
    c->serial++;
    return c->serial;
}

uint64_t x12_conn_next_token(struct x12_conn *c) {
    c->token++;
    return c->token;
}

int x12_conn_take_fd(struct x12_conn *c, uint32_t index, int *out_fd) {
    if (index >= c->n_in_fds) {
        return -1;
    }
    *out_fd = c->in_fds[index];
    memmove(&c->in_fds[index], &c->in_fds[index + 1],
            (c->n_in_fds - index - 1) * sizeof(int));
    c->n_in_fds--;
    return 0;
}

void x12_conn_release_fds(struct x12_conn *c) {
    for (size_t i = 0; i < c->n_in_fds; i++) {
        close(c->in_fds[i]);
    }
    c->n_in_fds = 0;
    for (size_t i = 0; i < c->n_out_fds; i++) {
        close(c->out_fds[i]);
    }
    c->n_out_fds = 0;
}

int x12_conn_event(struct x12_conn *c, const x12_iface_desc *iface,
                   const char *evt, uint32_t object,
                   const x12_arg *args, size_t nargs) {
    if (c->dead) {
        return -1;
    }
    const x12_msg_desc *msg = x12_msg_find_name(iface, X12_EVENT, evt);
    if (!msg) {
        x12_conn_kill(c, "unknown event in server code");
        return -1;
    }
    size_t written = 0;
    int rc = x12_encode(iface, msg, object, x12_now_ns(),
                        args, nargs, c->out + c->out_len,
                        X12_IO_BUF - c->out_len, &written);
    if (rc == X12_ERR_CAP) {
        // the client stopped reading its events, dropping them would
        // fake progress so the connection dies loudly instead
        x12_conn_kill(c, "out buffer full");
        return -1;
    }
    if (rc != X12_OK) {
        x12_conn_kill(c, "event encode failed");
        return -1;
    }
    c->out_len += written;
    return 0;
}

int x12_conn_event_fd(struct x12_conn *c, const x12_iface_desc *iface,
                      const char *evt, uint32_t object, x12_arg *args,
                      size_t nargs, size_t fd_arg_index, int fd) {
    if (c->n_out_fds >= X12_MAX_FDS) {
        close(fd);
        return -1;
    }
    // the wire slot names this fd by its position in the whole
    // connection fd stream, the client maps it onto a growing received
    // list; the counter never resets, a second fd event on the same
    // connection must not collide with the first
    args[fd_arg_index].u64 = c->out_fds_sent++;
    c->out_fds[c->n_out_fds++] = fd;
    if (x12_conn_event(c, iface, evt, object, args, nargs) != 0) {
        c->n_out_fds--;
        close(fd);
        return -1;
    }
    x12_conn_flush(c);
    return 0;
}

int x12_conn_error(struct x12_conn *c, uint32_t code, uint32_t object) {
    x12_arg args[2] = {
        { .u64 = code },
        { .u64 = object },
    };
    return x12_conn_event(c, x12_iface_find("x12_connection"), "error",
                          X12_OBJ_ID_CONNECTION, args, 2);
}

void x12_conn_flush(struct x12_conn *c) {
    // works even for a dead connection, the reap path sends the final
    // error events before the fd goes away
    size_t off = 0;
    while (off < c->out_len) {
        ssize_t n;
        if (c->n_out_fds > 0) {
            union { char buf[CMSG_SPACE(sizeof(int) * X12_MAX_FDS)]; struct cmsghdr align; } ctl;
            memset(ctl.buf, 0, sizeof ctl.buf);
            struct iovec iov = { c->out + off, c->out_len - off };
            struct msghdr msg;
            memset(&msg, 0, sizeof msg);
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = ctl.buf;
            msg.msg_controllen = CMSG_SPACE(sizeof(int) * c->n_out_fds);
            struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
            cm->cmsg_level = SOL_SOCKET;
            cm->cmsg_type = SCM_RIGHTS;
            cm->cmsg_len = CMSG_LEN(sizeof(int) * c->n_out_fds);
            memcpy(CMSG_DATA(cm), c->out_fds, sizeof(int) * c->n_out_fds);
            n = sendmsg(c->fd, &msg, MSG_NOSIGNAL);
            // the fds ride with the first bytes regardless of how much of
            // the payload made it, the kernel holds its own references
            for (size_t i = 0; i < c->n_out_fds; i++) {
                close(c->out_fds[i]);
            }
            c->n_out_fds = 0;
        } else {
            n = send(c->fd, c->out + off, c->out_len - off, MSG_NOSIGNAL);
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // the kernel buffer is full, the remainder stays queued until
            // the next loop pass drains it
            break;
        }
        if (n <= 0) {
            x12_conn_kill(c, "socket write failed");
            break;
        }
        off += (size_t)n;
    }
    // a partial send keeps its tail queued, events are never dropped to
    // fake progress
    if (off == c->out_len) {
        c->out_len = 0;
    } else {
        memmove(c->out, c->out + off, c->out_len - off);
        c->out_len -= off;
    }
}

int x12_conn_read(struct x12_conn *c,
                  int (*on_frame)(struct x12_conn *, const uint8_t *, size_t)) {
    if (c->dead) {
        return -1;
    }
    struct iovec iov = {
        c->in + c->in_len,
        X12_IO_BUF - c->in_len,
    };
    union {
        struct cmsghdr h;
        uint8_t buf[CMSG_SPACE(sizeof(int) * X12_MAX_FDS)];
    } ctl;
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = ctl.buf;
    mh.msg_controllen = sizeof ctl.buf;
    ssize_t n = recvmsg(c->fd, &mh, 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        // non blocking socket, the rest of the frame arrives later and
        // the buffered prefix must survive until then
        return 0;
    }
    if (n <= 0) {
        // peer closed or errored, the main loop reaps the connection
        x12_conn_kill(c, "peer closed");
        return -1;
    }
    c->in_len += (size_t)n;
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
        if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) {
            continue;
        }
        int *fds = (int *)CMSG_DATA(cm);
        size_t cnt = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < cnt; i++) {
            if (c->n_in_fds < X12_MAX_FDS) {
                c->in_fds[c->n_in_fds++] = fds[i];
            } else {
                close(fds[i]);
            }
        }
    }
    size_t off = 0;
    while (c->in_len - off >= X12_HEADER_SIZE) {
        const uint8_t *frame = c->in + off;
        uint32_t size = (uint32_t)frame[0] | ((uint32_t)frame[1] << 8) |
                        ((uint32_t)frame[2] << 16) | ((uint32_t)frame[3] << 24);
        // a size smaller than the header or bigger than the input buffer
        // is a framing violation and unfixable mid stream, bigger than
        // the buffered bytes just means the rest has not arrived yet
        if (size < X12_HEADER_SIZE || size > X12_IO_BUF) {
            x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, 0);
            x12_conn_kill(c, "bad frame size");
            return -1;
        }
        if (size > c->in_len - off) {
            break;
        }
        if (on_frame(c, frame, size) != 0) {
            return -1;
        }
        off += size;
    }
    memmove(c->in, c->in + off, c->in_len - off);
    c->in_len -= off;
    return 0;
}

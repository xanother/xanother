// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#include "conn.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "x12_protocol.h"
#include <stdlib.h>

#include "x12_net.h"

uint64_t x12_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// one scratch frame per encode, the server is single threaded
static uint8_t frame_scratch[X12_IO_BUF];

void x12_conn_init(struct x12_conn *c, int fd) {
    memset(c, 0, sizeof *c);
    c->fd = fd;
    // the caller attaches the tls object and the net flag for network
    // transports right after, unix starts with fd passing live
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
    free(c->netbuf);
    c->netbuf = 0;
    c->netbuf_len = 0;
    fprintf(stderr, "x12-server: disconnecting client fd %d, %s\n",
            c->fd, why);
}

// parked sessions, slots in the conns array that lost their transport
// but kept their identity. the scene keys clients by pointer so a
// parked session never moves
static struct x12_conn *parked[X12_MAX_CLIENTS];
static size_t n_parked;
// set by resume while a batch is mid dispatch, the pump moves the
// remainder of the batch to the session that took the transport over
static struct x12_conn *resume_target;

uint64_t x12_conn_mint_session(struct x12_conn *c) {
    // the token names the session across reconnects, it only has to be
    // unguessable inside the resume window
    uint64_t token = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (read(fd, &token, sizeof token) != (ssize_t)sizeof token) {
            token = 0;
        }
        close(fd);
    }
    if (token == 0) {
        token = (uint64_t)x12_now_ns() ^ ((uint64_t)(uintptr_t)c << 32);
    }
    // zero is the "no session" marker, never hand it out
    if (token == 0) {
        token = 1;
    }
    c->resume_token = token;
    return token;
}

int x12_conn_park(struct x12_conn *c, uint64_t ttl_ns) {
    if (c->resume_token == 0 || n_parked >= X12_MAX_CLIENTS) {
        return 0;
    }
    // whatever the transport had not finished dies with it, the client
    // rebuilds its state on resume. descriptors that never reached
    // either side go back to the kernel instead of vanishing with the
    // counters
    x12_conn_release_fds(c);
    c->in_len = 0;
    c->out_len = 0;
    c->parked = 1;
    c->park_deadline = x12_now_ns() + ttl_ns;
    parked[n_parked++] = c;
    return 1;
}

struct x12_conn *x12_conn_resume(struct x12_conn *fresh, uint64_t token) {
    if (token == 0) {
        return 0;
    }
    for (size_t i = 0; i < n_parked; i++) {
        struct x12_conn *p = parked[i];
        if (p->resume_token != token) {
            continue;
        }
        // the transport moves into the parked slot and the fresh conn
        // closes: the scene keeps pointing at the session it knows. the
        // out queue moves too, the events hello already earned are
        // owed to the client on the other side of this fd
        p->fd = fresh->fd;
        p->tls = fresh->tls;
        p->net = fresh->net;
        p->netbuf = fresh->netbuf;
        p->netbuf_len = fresh->netbuf_len;
        memcpy(p->out, fresh->out, fresh->out_len);
        p->out_len = fresh->out_len;
        memcpy(p->out_fds, fresh->out_fds,
               fresh->n_out_fds * sizeof fresh->out_fds[0]);
        p->n_out_fds = fresh->n_out_fds;
        p->out_fds_sent = fresh->out_fds_sent;
        p->parked = 0;
        p->dead = 0;
        // the fresh conn owns nothing any more, the pump moves the rest
        // of its input batch over before it goes
        fresh->fd = -1;
        fresh->tls = 0;
        fresh->netbuf = 0;
        fresh->netbuf_len = 0;
        fresh->out_len = 0;
        fresh->n_out_fds = 0;
        parked[i] = parked[--n_parked];
        resume_target = p;
        return p;
    }
    return 0;
}

void x12_conn_parked_sweep(uint64_t now,
                           void (*teardown)(struct x12_conn *)) {
    for (size_t i = 0; i < n_parked; i++) {
        struct x12_conn *p = parked[i];
        if (now < p->park_deadline) {
            continue;
        }
        // the resume window closed, the session goes away for good
        parked[i] = parked[--n_parked];
        i--;
        p->parked = 0;
        teardown(p);
        p->dead = 0;
    }
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
    return x12_conn_event_ts(c, iface, evt, object, args, nargs, 0);
}

// ts zero means now; presentation feedback carries the real scanout
// moment instead and must not be rewritten on the way out
int x12_conn_event_ts(struct x12_conn *c, const x12_iface_desc *iface,
                      const char *evt, uint32_t object,
                      const x12_arg *args, size_t nargs, uint64_t ts) {
    if (c->dead) {
        return -1;
    }
    if (c->parked) {
        // nobody is listening, a queued event would only fill the out
        // buffer and kill the parked session later
        return 0;
    }
    const x12_msg_desc *msg = x12_msg_find_name(iface, X12_EVENT, evt);
    if (!msg) {
        x12_conn_kill(c, "unknown event in server code");
        return -1;
    }
    if (ts == 0) {
        ts = x12_now_ns();
    }
    size_t written = 0;
    int rc = x12_encode(iface, msg, object, ts,
                        args, nargs, frame_scratch, sizeof frame_scratch,
                        &written);
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
    // network transports pay the envelope cost, compression inside it
    // keeps the big payloads small on the wire
    size_t wrapped = x12_net_wrap(c->net, c->out + c->out_len,
                                  X12_IO_BUF - c->out_len,
                                  frame_scratch, written);
    if (wrapped == 0) {
        x12_conn_kill(c, "out buffer full");
        return -1;
    }
    c->out_len += wrapped;
    return 0;
}

int x12_conn_event_fd(struct x12_conn *c, const x12_iface_desc *iface,
                      const char *evt, uint32_t object, x12_arg *args,
                      size_t nargs, size_t fd_arg_index, int fd) {
    if (c->dead || c->parked) {
        // the event itself would be dropped the same way, the
        // descriptor must not queue up behind it
        close(fd);
        return -1;
    }
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
        if (c->tls || c->net) {
            // fd passing does not exist past unix sockets, the events
            // that must carry one are refused upstream. the bytes
            // themselves travel the same
            n = x12_net_write(c->tls, c->fd, c->out + off, c->out_len - off);
        }
        else if (c->n_out_fds > 0) {
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
            n = x12_net_write(NULL, c->fd, c->out + off, c->out_len - off);
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
    ssize_t n;
    struct msghdr mh;
    if (c->net) {
        // the envelope staging grows once per network connection and
        // only ever holds bytes that have not peeled into in yet
        if (!c->netbuf) {
            c->netbuf = malloc(X12_NET_STAGING);
            if (!c->netbuf) {
                x12_conn_kill(c, "out of memory");
                return -1;
            }
        }
    }
    union {
        struct cmsghdr h;
        uint8_t buf[CMSG_SPACE(sizeof(int) * X12_MAX_FDS)];
    } ctl;
    struct iovec iov = {
        c->in + c->in_len,
        X12_IO_BUF - c->in_len,
    };
    if (c->tls || c->net) {
        // no scm rights on the network transports
        memset(&mh, 0, sizeof mh);
        if (c->net) {
            // envelopes land in the staging area and peel into in as
            // whole frames
            iov.iov_base = c->netbuf + c->netbuf_len;
            iov.iov_len = X12_NET_STAGING - c->netbuf_len;
        }
        n = x12_net_read(c->tls, c->fd, iov.iov_base, iov.iov_len);
    }
    else {
        memset(&mh, 0, sizeof mh);
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = ctl.buf;
        mh.msg_controllen = sizeof ctl.buf;
        n = recvmsg(c->fd, &mh, 0);
    }
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
    if (c->net) {
        c->netbuf_len += (size_t)n;
        if (x12_net_peel(c->netbuf, &c->netbuf_len,
                         c->in, &c->in_len, X12_IO_BUF) != 0) {
            x12_conn_error(c, X12_ERROR_CODE_BAD_VALUE, 0);
            x12_conn_kill(c, "bad frame envelope");
            return -1;
        }
    }
    else {
        c->in_len += (size_t)n;
    }
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
    return x12_conn_pump(c, on_frame);
}

int x12_conn_pump(struct x12_conn *c,
                  int (*on_frame)(struct x12_conn *, const uint8_t *,
                                  size_t)) {
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
        int rc = on_frame(c, frame, size);
        off += size;
        // a resume moved the transport elsewhere mid batch: the rest of
        // the batch belongs to that session now. this runs even when the
        // handler returned -1, the resume handler always does
        if (resume_target) {
            struct x12_conn *p = resume_target;
            resume_target = 0;
            size_t rest = c->in_len - off;
            memcpy(p->in + p->in_len, c->in + off, rest);
            p->in_len += rest;
            c->in_len = 0;
            return 0;
        }
        if (rc != 0) {
            return -1;
        }
    }
    memmove(c->in, c->in + off, c->in_len - off);
    c->in_len -= off;
    return 0;
}

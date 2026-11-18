// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// client session over the shared wire engine: one connection, one thread,
// one handler. the proxy table mirrors the server registry, binds fire
// from global announcements and the toplevel helper drives the configure
// handshake so clients never touch raw opcodes


#include "libx12.h"
#include "x12_protocol.h"
#include "x12_net.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#define X12C_MAX_PROXIES 256
#define X12C_MAX_BINDS 16
#define X12C_MAX_OUTPUTS 8
#define X12C_MAX_FDS 32
#define X12C_OUT_CAP 131072
#define X12C_IN_CAP 262144
#define X12C_MAX_ARGS 16

struct x12c {
    int fd;
    // tls connection object on tls transports, NULL otherwise
    void *tls;
    // 1 on tcp and tls: fd passing does not exist there
    int net;
    x12c_handler handler;
    x12c_proxy proxies[X12C_MAX_PROXIES];
    struct {
        char name[64];
        uint32_t proxy_id;
        void *user;
        int done;
    } binds[X12C_MAX_BINDS];
    int n_binds;
    int n_outputs;
    uint8_t out[X12C_OUT_CAP];
    size_t out_len;
    int out_fds[X12C_MAX_FDS];
    size_t n_out_fds;
    uint8_t in[X12C_IN_CAP];
    size_t in_len;
    // envelope staging for the network transports
    uint8_t *netbuf;
    size_t netbuf_len;
    int fd_slots[X12C_MAX_FDS];
    int n_fd_slots;
    // the implicit nanosecond timestamp of the event being dispatched,
    // clients measuring latency read it inside the handler
    uint64_t last_event_ts;
    // toplevel_map watches these, the helper drives the handshake by
    // dispatching until the watched events arrive
    uint32_t wait_proxy;
    uint32_t wait_serial;
    int got_configure;
    int got_release;
};

static int encode_into(struct x12c *c, const char *iface_name,
                       const char *request, uint32_t object, x12_arg *args,
                       size_t nargs) {
    const x12_iface_desc *iface = x12_iface_find(iface_name);
    const x12_msg_desc *m =
        iface ? x12_msg_find_name(iface, X12_REQUEST, request) : 0;
    if (!iface || !m) {
        return -1;
    }
    static uint8_t scratch[X12C_OUT_CAP];
    size_t len = 0;
    if (x12_encode(iface, m, object, 0, args, nargs,
                   scratch, sizeof scratch, &len) != X12_OK) {
        return -1;
    }
    size_t wrapped = x12_net_wrap(c->net, c->out + c->out_len,
                                  sizeof c->out - c->out_len,
                                  scratch, len);
    if (wrapped == 0) {
        return -1;
    }
    c->out_len += wrapped;
    return 0;
}

static int flush_out(struct x12c *c) {
    size_t off = 0;
    while (off < c->out_len) {
        ssize_t n;
        if (c->tls || c->net) {
            // fd passing does not exist past unix sockets
            n = x12_net_write(c->tls, c->fd, c->out + off, c->out_len - off);
        }
        else if (c->n_out_fds > 0) {
            union {
                char buf[CMSG_SPACE(sizeof(int) * X12C_MAX_FDS)];
                struct cmsghdr align;
            } ctl;
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
            for (size_t i = 0; i < c->n_out_fds; i++) {
                close(c->out_fds[i]);
            }
            c->n_out_fds = 0;
        } else {
            n = x12_net_write(NULL, c->fd, c->out + off, c->out_len - off);
        }
        if (n <= 0) {
            // a half-written frame poisons the stream: there is no
            // sane retry and the connection dies here, so the queue
            // must not pretend its frames are still sendable
            c->out_len = 0;
            return -1;
        }
        off += (size_t)n;
    }
    c->out_len = 0;
    return 0;
}

x12c *x12c_connect(const char *path, x12c_handler handler) {
    struct x12c *c = calloc(1, sizeof *c);
    if (!c) {
        return 0;
    }
    // a bare path dials a unix socket, tcp: and tls: dial the network
    // transports. the session above is identical on all three
    struct x12_net_addr addr;
    void *tls = NULL;
    if (x12_net_parse(path, &addr) != 0) {
        free(c);
        return 0;
    }
    c->fd = x12_net_connect(&addr, &tls);
    if (c->fd < 0) {
        free(c);
        return 0;
    }
    c->tls = tls;
    c->net = addr.kind != X12_NET_UNIX;
    c->handler = handler;
    // the handshake objects carry the fixed ids of the convention, a
    // connect is enough to talk to connection and registry
    c->proxies[1].id = 1;
    c->proxies[1].iface = x12_iface_find("x12_connection");
    c->proxies[2].id = 2;
    c->proxies[2].iface = x12_iface_find("x12_registry");

    x12_arg hello[2] = { { .u64 = X12_IDL_FORMAT }, { .u64 = 0 } };
    if (encode_into(c, "x12_connection", "hello", 1, hello, 2) != 0 ||
        flush_out(c) != 0) {
        x12_net_tls_free(c->tls);
        close(c->fd);
        free(c);
        return 0;
    }
    return c;
}

void x12c_close(x12c *c) {
    if (!c) {
        return;
    }
    for (int i = 0; i < c->n_fd_slots; i++) {
        close(c->fd_slots[i]);
    }
    x12_net_tls_free(c->tls);
    close(c->fd);
    free(c->netbuf);
    free(c);
}

int x12c_fd(x12c *c) {
    return c->fd;
}

const x12_iface_desc *x12c_proxy_iface(const x12c *c, uint32_t id) {
    if (id >= X12C_MAX_PROXIES) {
        return 0;
    }
    return c->proxies[id].iface;
}

static uint32_t take_id(struct x12c *c) {
    for (uint32_t id = 1; id < X12C_MAX_PROXIES; id++) {
        // pending slots belong to a queued bind waiting for its global
        if (!c->proxies[id].iface && !c->proxies[id].pending) {
            return id;
        }
    }
    return 0;
}

uint32_t x12c_bind(x12c *c, const char *global, void *user) {
    if (c->n_binds >= X12C_MAX_BINDS) {
        return 0;
    }
    uint32_t id = take_id(c);
    if (!id) {
        return 0;
    }
    // the id is taken up front so callers can wire factories to it
    // before the registry announcement ever arrives
    c->binds[c->n_binds].proxy_id = id;
    c->binds[c->n_binds].user = user;
    c->binds[c->n_binds].done = 0;
    snprintf(c->binds[c->n_binds].name, sizeof c->binds[0].name, "%s", global);
    c->n_binds++;
    c->proxies[id].id = id;
    c->proxies[id].user = user;
    c->proxies[id].pending = 1;
    return id;
}

uint32_t x12c_reserve(x12c *c, const char *iface_name, void *user) {
    uint32_t id = take_id(c);
    if (!id) {
        return 0;
    }
    c->proxies[id].id = id;
    c->proxies[id].iface = x12_iface_find(iface_name);
    c->proxies[id].user = user;
    return id;
}

int x12c_call(x12c *c, const char *iface_name, const char *request,
              uint32_t object, x12_arg *args, size_t nargs) {
    // the out buffer flushes when the next frame would not fit, calls
    // stay batched without ever risking a silent refuse
    if (c->out_len > sizeof c->out - 4096 && flush_out(c) != 0) {
        return -1;
    }
    return encode_into(c, iface_name, request, object, args, nargs);
}

int x12c_callfd(x12c *c, const char *iface_name, const char *request,
                uint32_t object, x12_arg *args, size_t nargs,
                size_t fd_arg, int fd) {
    // the call owns the descriptor from here on, it is closed exactly
    // once on every path. a caller close after this would race the fd
    // number against the next open
    if (c->net) {
        // no scm rights on the network transports, the remote pool
        // story is a payload channel of its own
        close(fd);
        return -1;
    }
    if (c->n_out_fds >= X12C_MAX_FDS) {
        close(fd);
        return -1;
    }
    // the wire slot counts the ancillary queue, flushing per call keeps
    // slot and queue position in lockstep
    args[fd_arg].u64 = c->n_out_fds;
    if (encode_into(c, iface_name, request, object, args, nargs) != 0) {
        close(fd);
        return -1;
    }
    c->out_fds[c->n_out_fds++] = fd;
    return flush_out(c);
}

int x12c_flush(x12c *c) {
    return flush_out(c);
}

static void process_frame(struct x12c *c, const uint8_t *frame, size_t len) {
    uint32_t object;
    uint16_t opcode, reserved;
    memcpy(&object, frame + 4, 4);
    memcpy(&opcode, frame + 8, 2);
    memcpy(&reserved, frame + 10, 2);
    if (reserved != 0 || object >= X12C_MAX_PROXIES ||
        !c->proxies[object].iface) {
        // an unknown object means client and server disagree on state,
        // staying silent would hide the divergence
        fprintf(stderr, "libx12: event for unknown object %u\n", object);
        return;
    }
    const x12c_proxy *p = &c->proxies[object];
    const x12_msg_desc *m = x12_msg_find(p->iface, X12_EVENT, opcode);
    if (!m) {
        fprintf(stderr, "libx12: unknown opcode %u on %s\n", opcode,
                p->iface->name);
        return;
    }
    x12_arg args[X12C_MAX_ARGS];
    uint32_t obj_id;
    uint64_t ts;
    if (x12_decode(p->iface, m, frame, len, &obj_id, &ts, args,
                   X12C_MAX_ARGS) != X12_OK) {
        fprintf(stderr, "libx12: decode %s.%s refused\n", p->iface->name,
                m->name);
        return;
    }
    c->last_event_ts = ts;
    if (strcmp(p->iface->name, "x12_registry") == 0 &&
        strcmp(m->name, "global") == 0) {
        if (args[0].str_len == strlen("x12_output") &&
            memcmp(args[0].str, "x12_output", args[0].str_len) == 0 &&
            c->n_outputs < X12C_MAX_OUTPUTS) {
            c->n_outputs++;
        }
        for (int i = 0; i < c->n_binds; i++) {
            size_t n = strlen(c->binds[i].name);
            if (c->binds[i].done || args[0].str_len != n ||
                memcmp(args[0].str, c->binds[i].name, n) != 0) {
                continue;
            }
            c->binds[i].done = 1;
            uint32_t id = c->binds[i].proxy_id;
            x12_arg bind[4] = {
                { .str = c->binds[i].name, .str_len = n },
                { .u64 = args[1].u64 },
                { .u64 = id },
                { .u64 = args[3].u64 },
            };
            if (encode_into(c, "x12_registry", "bind", 2, bind, 4) != 0) {
                return;
            }
            c->proxies[id].id = id;
            c->proxies[id].iface = x12_iface_find(c->binds[i].name);
            c->proxies[id].user = c->binds[i].user;
            c->proxies[id].pending = 0;
        }
    }
    if (object == c->wait_proxy && strcmp(m->name, "configure") == 0) {
        // the serial sits in the last arg of both role offers
        c->wait_serial = (uint32_t)args[m->arg_count - 1].u64;
        c->got_configure = 1;
    }
    if (strcmp(m->name, "release") == 0) {
        // any release during the map is the composited signal, only one
        // buffer of the client is live at that point
        c->got_release = 1;
    }
    // every fd arg names a wire slot into the received ancillary queue,
    // the handler gets the real descriptor; argless events like release
    // carry a null args table and must never be dereferenced
    for (uint32_t ai = 0; ai < m->arg_count; ai++) {
        if (m->args[ai].type != X12_ARG_FD) {
            continue;
        }
        uint32_t slot = (uint32_t)args[ai].u64;
        if (slot < (uint32_t)c->n_fd_slots) {
            args[ai].u64 = (uint64_t)(ssize_t)c->fd_slots[slot];
        } else {
            args[ai].u64 = (uint64_t)(ssize_t)-1;
        }
    }
    c->handler(c, p, m, args, m->arg_count);
}

static int frame_ready(const struct x12c *c, uint32_t *size_out) {
    if (c->in_len < X12_HEADER_SIZE) {
        return 0;
    }
    uint32_t size;
    memcpy(&size, c->in, 4);
    if (size < X12_HEADER_SIZE || size > sizeof c->in) {
        return -1;
    }
    *size_out = size;
    return c->in_len >= size;
}

// processes one already buffered frame without touching the socket, a
// single recv usually carries a burst of events and poll would block on
// the leftovers
static int feed_ready(struct x12c *c) {
    uint32_t size;
    int r = frame_ready(c, &size);
    if (r <= 0) {
        return r;
    }
    process_frame(c, c->in, size);
    memmove(c->in, c->in + size, c->in_len - size);
    c->in_len -= size;
    return 1;
}

static int pump_once(struct x12c *c) {
    if (c->net) {
        // envelopes arrive whole in the staging area and peel into in
        // as complete frames, a frame is never split across peels
        if (!c->netbuf) {
            c->netbuf = malloc(X12C_IN_CAP + 16);
            if (!c->netbuf) {
                return -1;
            }
        }
        while (c->in_len < X12_HEADER_SIZE) {
            ssize_t n = x12_net_read(c->tls, c->fd,
                                     c->netbuf + c->netbuf_len,
                                     X12C_IN_CAP + 16 - c->netbuf_len);
            if (n <= 0) {
                return -1;
            }
            c->netbuf_len += (size_t)n;
            if (x12_net_peel(c->netbuf, &c->netbuf_len,
                             c->in, &c->in_len, sizeof c->in) != 0) {
                return -1;
            }
        }
    }
    while (!c->net && c->in_len < X12_HEADER_SIZE) {
        struct iovec iov = { c->in + c->in_len, sizeof c->in - c->in_len };
        union {
            char buf[CMSG_SPACE(sizeof(int) * 4)];
            struct cmsghdr align;
        } ctl;
        memset(ctl.buf, 0, sizeof ctl.buf);
        struct msghdr msg;
        memset(&msg, 0, sizeof msg);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctl.buf;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * 4);
        ssize_t n = recvmsg(c->fd, &msg, MSG_NOSIGNAL);
        if (n <= 0) {
            return -1;
        }
        c->in_len += (size_t)n;
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm;
             cm = CMSG_NXTHDR(&msg, cm)) {
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
                int *fds = (int *)CMSG_DATA(cm);
                int cnt = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                for (int i = 0; i < cnt && c->n_fd_slots < X12C_MAX_FDS; i++) {
                    c->fd_slots[c->n_fd_slots++] = fds[i];
                }
            }
        }
    }
    uint32_t size;
    memcpy(&size, c->in, 4);
    if (size < X12_HEADER_SIZE || size > sizeof c->in) {
        return -1;
    }
    while (!c->net && c->in_len < size) {
        ssize_t n = x12_net_read(c->tls, c->fd, c->in + c->in_len,
                                 size - c->in_len);
        if (n <= 0) {
            return -1;
        }
        c->in_len += (size_t)n;
    }
    process_frame(c, c->in, size);
    memmove(c->in, c->in + size, c->in_len - size);
    c->in_len -= size;
    return 1;
}

int x12c_n_outputs(const x12c *c) {
    return c->n_outputs;
}

uint64_t x12c_last_event_ts(const x12c *c) {
    return c->last_event_ts;
}

uint32_t
x12c_bind_instance(x12c *c, const char *global, uint32_t instance, void *user)
{
    if (c->n_binds >= X12C_MAX_BINDS) {
        return 0;
    }
    uint32_t id = x12c_reserve(c, global, user);
    if (!id) {
        return 0;
    }
    size_t n = strlen(global);
    const x12_iface_desc *iface = x12_iface_find(global);
    x12_arg bind[4] = {
        { .str = global, .str_len = n },
        { .u64 = iface ? iface->version : 1 },
        { .u64 = id },
        { .u64 = instance },
    };
    if (encode_into(c, "x12_registry", "bind", 2, bind, 4) != 0) {
        return 0;
    }
    c->proxies[id].id = id;
    c->proxies[id].iface = iface;
    c->proxies[id].user = user;
    c->proxies[id].pending = 0;
    return id;
}

int x12c_dispatch(struct x12c *c, int timeout_ms) {
    if (c->out_len > 0 && flush_out(c) != 0) {
        return -1;
    }
    int fired = 0;
    struct pollfd pfd = { c->fd, POLLIN, 0 };
    int n;
    // buffered frames first, then a nonblocking drain, then one blocking
    // poll: a burst between iterations never stalls the client behind a
    // timeout
    for (;;) {
        int r = feed_ready(c);
        if (r < 0) {
            return -1;
        }
        if (!r) {
            break;
        }
        fired = 1;
    }
    while ((n = poll(&pfd, 1, 0)) > 0 && (pfd.revents & POLLIN)) {
        int r = pump_once(c);
        if (r < 0) {
            return -1;
        }
        fired = 1;
        for (;;) {
            r = feed_ready(c);
            if (r < 0) {
                return -1;
            }
            if (!r) {
                break;
            }
            fired = 1;
        }
    }
    if (n < 0 && errno != EINTR) {
        return -1;
    }
    if (!fired && timeout_ms > 0) {
        n = poll(&pfd, 1, timeout_ms);
        if (n > 0 && (pfd.revents & POLLIN)) {
            int r = pump_once(c);
            if (r < 0) {
                return -1;
            }
            fired = 1;
            for (;;) {
                r = feed_ready(c);
                if (r < 0) {
                    return -1;
                }
                if (!r) {
                    break;
                }
                fired = 1;
            }
        }
    }
    return fired;
}

int x12c_pool_create(x12c *c, uint32_t bufman_id, size_t size,
                     uint32_t *id_out, void *user) {
    int memfd = (int)syscall(SYS_memfd_create, "x12c-pool",
                            MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (memfd < 0 || ftruncate(memfd, (off_t)size) != 0) {
        if (memfd >= 0) {
            close(memfd);
        }
        return -1;
    }
    // flush_out owns the fds it sends and closes them, so the wire
    // travels with a duplicate and the caller keeps the original to
    // paint into
    int wire_fd = dup(memfd);
    if (wire_fd < 0) {
        close(memfd);
        return -1;
    }
    uint32_t id = x12c_reserve(c, "x12_shm_pool", user);
    if (!id) {
        close(memfd);
        close(wire_fd);
        return -1;
    }
    x12_arg args[3] = { { .u64 = 0 }, { .u64 = size }, { .u64 = id } };
    if (x12c_callfd(c, "x12_buffer_manager", "create_shm_pool", bufman_id,
                    args, 3, 0, wire_fd) != 0) {
        // callfd closed the wire copy already
        close(memfd);
        return -1;
    }
    *id_out = id;
    return memfd;
}

int x12c_buffer_create(x12c *c, uint32_t pool_id, size_t offset,
                       uint32_t width, uint32_t height, uint32_t format,
                       uint32_t *id_out, void *user) {
    uint32_t id = x12c_reserve(c, "x12_buffer", user);
    if (!id) {
        return -1;
    }
    x12_arg args[6] = { { .u64 = offset }, { .u64 = width },
                        { .u64 = height }, { .u64 = width * 4 },
                        { .u64 = format }, { .u64 = id } };
    if (x12c_call(c, "x12_shm_pool", "create_buffer", pool_id, args, 6) != 0) {
        return -1;
    }
    *id_out = id;
    return 0;
}

int x12c_toplevel_map(x12c *c, uint32_t compositor_id, uint32_t surface_id) {
    uint32_t tl = x12c_reserve(c, "x12_toplevel", 0);
    if (!tl) {
        return -1;
    }
    // the configure lands on the new toplevel proxy, the release lands
    // on the buffer
    c->wait_proxy = tl;
    c->got_configure = c->got_release = 0;
    x12_arg ctl[2] = { { .u64 = tl }, { .u64 = surface_id } };
    if (x12c_call(c, "x12_compositor", "create_toplevel", compositor_id,
                  ctl, 2) != 0) {
        return -1;
    }
    while (!c->got_configure) {
        if (x12c_dispatch(c, 2000) <= 0) {
            return -1;
        }
    }
    x12_arg ack[1] = { { .u64 = c->wait_serial } };
    if (x12c_call(c, "x12_toplevel", "ack_configure", tl, ack, 1) != 0 ||
        x12c_call(c, "x12_surface", "commit", surface_id, 0, 0) != 0) {
        return -1;
    }
    // the release of the retired frame is the composited signal
    while (!c->got_release) {
        if (x12c_dispatch(c, 2000) <= 0) {
            return -1;
        }
    }
    c->wait_proxy = 0;
    return (int)tl;
}

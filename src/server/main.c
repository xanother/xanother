// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// entry point: unix socket, poll loop, accepts clients and hands their
// frames to the dispatch table. this file owns no protocol logic

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "conn.h"
#include "dispatch.h"
#include "limits.h"
#include "buffer.h"
#include "frame.h"
#include "compositor.h"
#include "backend.h"

// the reap path gives everything back: surfaces retire their buffers,
// pools unmap and parked fds close before the socket goes away
static void client_teardown(struct x12_conn *c) {
    for (uint32_t id = 1; id < X12_MAX_OBJECTS; id++) {
        struct x12_object *o = x12_objects_get(&c->objects, id);
        if (o && o->kind == OBJ_SURFACE) {
            x12_frame_destroy(c, &o->as.surface);
        }
    }
    x12_buffers_teardown(c);
    x12_conn_release_fds(c);
}

static volatile sig_atomic_t running = 1;

static void on_term(int sig) {
    (void)sig;
    running = 0;
}

static int make_socket(const char *path) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) {
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);
    unlink(path);
    if (bind(s, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(s, 8) != 0) {
        close(s);
        return -1;
    }
    return s;
}

int main(int argc, char **argv) {
    const char *sock_path = argc > 1 ? argv[1] : "/tmp/x12.sock";
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigaction(SIGINT, &sa, 0);
    sigaction(SIGTERM, &sa, 0);
    signal(SIGPIPE, SIG_IGN);

    int srv = make_socket(sock_path);
    if (srv < 0) {
        perror("socket");
        return 1;
    }
    if (x12_backend.init(argc, argv) != 0) {
        fprintf(stderr, "x12-server: bad backend arguments\n");
        close(srv);
        return 1;
    }
    uint32_t canvas_w = 0, canvas_h = 0;
    x12_backend.get_size(&canvas_w, &canvas_h);
    x12_comp_set_size(canvas_w, canvas_h);
    fprintf(stderr, "x12-server: listening on %s, canvas %ux%u\n",
            sock_path, canvas_w, canvas_h);

    struct x12_conn conns[X12_MAX_CLIENTS];
    for (size_t i = 0; i < X12_MAX_CLIENTS; i++) {
        conns[i].fd = -1;
    }

    while (running) {
        struct pollfd pfd[1 + X12_MAX_CLIENTS];
        size_t slot_of[1 + X12_MAX_CLIENTS];
        pfd[0].fd = srv;
        pfd[0].events = POLLIN;
        nfds_t n = 1;
        for (size_t i = 0; i < X12_MAX_CLIENTS; i++) {
            if (conns[i].fd < 0) {
                continue;
            }
            pfd[n].fd = conns[i].fd;
            pfd[n].events = POLLIN;
            if (conns[i].out_len > 0) {
                pfd[n].events |= POLLOUT;
            }
            slot_of[n] = i;
            n++;
        }
        int rc = poll(pfd, n, 250);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }
        if (x12_backend.halt_requested()) {
            break;
        }

        if (pfd[0].revents & POLLIN) {
            int fd = accept(srv, 0, 0);
            if (fd >= 0) {
                size_t i;
                for (i = 0; i < X12_MAX_CLIENTS; i++) {
                    if (conns[i].fd < 0) {
                        break;
                    }
                }
                if (i == X12_MAX_CLIENTS) {
                    // the limit is loud, the extra peer just never joins
                    fprintf(stderr, "x12-server: refusing client, connection limit\n");
                    close(fd);
                } else {
                    x12_conn_init(&conns[i], fd);
                }
            }
        }

        for (nfds_t k = 1; k < n; k++) {
            struct x12_conn *c = &conns[slot_of[k]];
            if (pfd[k].revents & (POLLERR | POLLHUP)) {
                x12_conn_kill(c, "poll error");
            } else {
                if (pfd[k].revents & POLLIN) {
                    x12_conn_read(c, x12_dispatch);
                }
                if (pfd[k].revents & POLLOUT) {
                    x12_conn_flush(c);
                }
            }
            if (c->dead) {
                // the final error events go out before the fd goes away,
                // a loud disconnect names its reason
                x12_conn_flush(c);
                client_teardown(c);
                close(c->fd);
                c->fd = -1;
            }
        }
        // drain whatever queued while handlers were answering, and let
        // fences that signaled between passes retire their frames
        for (size_t i = 0; i < X12_MAX_CLIENTS; i++) {
            if (conns[i].fd >= 0) {
                x12_frame_drain_client(&conns[i]);
            }
            if (conns[i].fd >= 0 && conns[i].out_len > 0) {
                x12_conn_flush(&conns[i]);
            }
            if (conns[i].dead && conns[i].fd >= 0) {
                x12_conn_flush(&conns[i]);
                client_teardown(&conns[i]);
                close(conns[i].fd);
                conns[i].fd = -1;
            }
        }
        // the composite pass repaints whatever the commits damaged, one
        // pass per loop round keeps the damage accounting single writer
        x12_comp_pass(conns, X12_MAX_CLIENTS);
        x12_backend.pump();
    }

    for (size_t i = 0; i < X12_MAX_CLIENTS; i++) {
        if (conns[i].fd >= 0) {
            close(conns[i].fd);
        }
    }
    x12_backend.shutdown();
    close(srv);
    unlink(sock_path);
    fprintf(stderr, "x12-server: bye\n");
    return 0;
}

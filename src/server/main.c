// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// entry point: a unix socket, a tcp port or a tls port, poll loop,
// accepts clients and hands their frames to the dispatch table. this
// file owns no protocol logic

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
#include "handlers.h"
#include "x12_limits.h"
#include "output.h"
#include "buffer.h"
#include "frame.h"
#include "windows.h"
#include "compositor.h"
#include "backend.h"
#include "input.h"
#include "gpu.h"
#include "source.h"
#include "selection.h"
#include "x12_net.h"

const struct x12_backend_ops *x12_backend;

// the reap path gives everything back: surfaces retire their buffers,
// pools unmap and parked fds close before the socket goes away
static void client_teardown(struct x12_conn *c) {
    // a dying selection owner drops both slots, the readers learn the
    // generation moved and their offers are finished
    x12_selection_release_owner(c);
    // output bindings go with the connection, the fan out skips dead
    // pointers but the slot goes back now
    x12_output_unbind(c);
    // scanout credits go nowhere now, drop the sessions
    x12_comp_scanout_drop(c);
    x12_windows_remove_client(c);
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

// cables move while the server runs: outputs announce themselves, dead
// heads say so and the canvas follows the bounding box of the live ones
static void sync_outputs(struct x12_conn *conns, size_t n) {
    struct x12_output_event ev;
    int changed = 0;
    while (x12_backend->output_poll && x12_backend->output_poll(&ev)) {
        changed = 1;
        if (ev.kind == X12_OUTPUT_REMOVED) {
            x12_output_drop(ev.out);
            x12_output_removed(ev.out);
            continue;
        }
        struct x12_out_info oi;
        struct x12_output_desc d;
        if (x12_backend->output_info(ev.out, &oi)) {
            memset(&d, 0, sizeof d);
            snprintf(d.name, sizeof d.name, "%s",
                     oi.name ? oi.name : "X12");
            d.x = oi.x;
            d.y = oi.y;
            d.w = oi.width;
            d.h = oi.height;
            d.mm_w = oi.mm_width;
            d.mm_h = oi.mm_height;
            d.refresh_millihz = oi.refresh_millihz;
            x12_output_set(ev.out, &d);
        }
        // the bindings that survived hear the screen is back and every
        // connection up now learns the instance for fresh binds
        x12_output_refresh(ev.out);
        for (size_t i = 0; i < n; i++) {
            if (conns[i].fd >= 0 && !conns[i].dead) {
                x12_announce_output(&conns[i], (uint32_t)ev.out);
            }
        }
    }
    if (!changed) {
        return;
    }
    uint32_t w = 0, h = 0;
    x12_backend->get_size(&w, &h);
    if (w == 0 || h == 0) {
        // every head is down, the canvas keeps its last size until one
        // of them comes home
        return;
    }
    x12_comp_set_size(w, h);
    x12_windows_set_output(w, h);
    x12_comp_full_damage();
}

// the resume window, tests shrink it through the environment
static uint64_t session_ttl_ns = X12_SESSION_TTL_NS;

int main(int argc, char **argv) {
    const char *sock_path = argc > 1 ? argv[1] : "/tmp/x12.sock";
    struct sigaction sa;

    // the host is a startup decision: --kms rides the drm card the flag
    // names, everything else runs headless
    x12_backend = &x12_backend_headless;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--kms") == 0) {
            x12_backend = &x12_backend_kms;
        }
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigaction(SIGINT, &sa, 0);
    sigaction(SIGTERM, &sa, 0);
    signal(SIGPIPE, SIG_IGN);

    // the positional argument is an address: a bare path or unix: for
    // a local socket, tcp:HOST:PORT and tls:HOST:PORT for the network
    // transports. the wire above never learns which one it got
    struct x12_net_addr addr;
    void *tls_ctx = NULL;
    if (x12_net_parse(sock_path, &addr) != 0) {
        fprintf(stderr, "x12-server: bad listen address %s\n", sock_path);
        return 1;
    }
    int srv = x12_net_listen(&addr, &tls_ctx);
    if (srv < 0) {
        return 1;
    }
    if (x12_backend->init(argc, argv) != 0) {
        fprintf(stderr, "x12-server: bad backend arguments\n");
        close(srv);
        // the socket file stays behind otherwise and the next spawn
        // connects into the void instead of a fresh server
        if (addr.kind == X12_NET_UNIX) {
            unlink(addr.path);
        }
        return 1;
    }
    uint32_t canvas_w = 0, canvas_h = 0;
    x12_backend->get_size(&canvas_w, &canvas_h);
    // the output globals report the real screens the backend drives
    for (int i = 0; i < x12_backend->output_count(); i++) {
        struct x12_out_info oi;
        struct x12_output_desc d;
        if (!x12_backend->output_info(i, &oi)) {
            continue;
        }
        memset(&d, 0, sizeof d);
        snprintf(d.name, sizeof d.name, "%s", oi.name ? oi.name : "X12");
        d.x = oi.x;
        d.y = oi.y;
        d.w = oi.width;
        d.h = oi.height;
        d.mm_w = oi.mm_width;
        d.mm_h = oi.mm_height;
        d.refresh_millihz = oi.refresh_millihz;
        x12_output_set(i, &d);
    }
    x12_comp_set_size(canvas_w, canvas_h);
    x12_windows_init();
    x12_windows_set_output(canvas_w, canvas_h);

    // input sources feed the router, --input opens the event line file
    // and --evdev may repeat for every device node. libinput is the
    // default seat input when the build found libinput and the seat
    // answers, --no-libinput pins the raw evdec path, --libinput
    // forces the attempt even on a build that never had it (the stub
    // refuses politely)
    // -1 = auto, 1 = forced on, 0 = off
    int want_libinput = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            i++;
            if (x12_input_file_open(argv[i]) != 0) {
                fprintf(stderr, "x12-server: cannot open input %s\n", argv[i]);
                close(srv);
                // same reason as the backend path: a stale socket file
                // would swallow the next spawn
                if (addr.kind == X12_NET_UNIX) {
                    unlink(addr.path);
                }
                return 1;
            }
        } else if (strcmp(argv[i], "--evdev") == 0 && i + 1 < argc) {
            i++;
            if (x12_evdev_add(argv[i]) != 0) {
                fprintf(stderr, "x12-server: cannot open evdev %s\n", argv[i]);
                close(srv);
                if (addr.kind == X12_NET_UNIX) {
                    unlink(addr.path);
                }
                return 1;
            }
        } else if (strcmp(argv[i], "--libinput") == 0) {
            want_libinput = 1;
        } else if (strcmp(argv[i], "--no-libinput") == 0) {
            want_libinput = 0;
        }
    }

    // the seat input: libinput carries calibration, acceleration and
    // device hotplug that the raw evdec lacks. auto stands for "when
    // the build had libinput", a failed seat start still leaves the
    // server alive, the raw paths keep feeding the router
    int libinput_on = want_libinput;
#ifdef X12_HAVE_LIBINPUT
    if (libinput_on < 0) {
        libinput_on = 1;
    }
#endif
    if (libinput_on > 0) {
        if (x12_libinput_start() == 0) {
            fprintf(stderr, "x12-server: libinput seat0 armed\n");
        } else {
            fprintf(stderr,
                    "x12-server: libinput unavailable, evdev only\n");
        }
    }

    // --gpu blends through the gl canvas, --gpu-node puts the context
    // on a render node so the gbm present can share its device. a gl
    // stack that does not come up degrades to the cpu path, the server
    // never dies over missing gpu support
    int want_gpu = 0;
    const char *gpu_node = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--gpu") == 0) {
            want_gpu = 1;
        } else if (strcmp(argv[i], "--gpu-node") == 0 && i + 1 < argc) {
            want_gpu = 1;
            gpu_node = argv[++i];
        }
    }
    if (want_gpu) {
        if (x12_gpu_init(gpu_node)) {
            // a gl stack that comes up but cannot take the canvas size
            // must not print a success it cannot back up: the cpu path
            // still owns the screen then
            if (x12_gpu_canvas(canvas_w, canvas_h)) {
                fprintf(stderr, "x12-server: gpu canvas active\n");
            } else {
                fprintf(stderr,
                        "x12-server: gpu canvas unavailable, cpu path\n");
            }
        } else {
            fprintf(stderr, "x12-server: gpu engine unavailable, cpu path\n");
        }
    }
    fprintf(stderr, "x12-server: listening on %s, canvas %ux%u\n",
            sock_path, canvas_w, canvas_h);

    {
        const char *ttl = getenv("X12_SESSION_TTL_MS");
        if (ttl && atoi(ttl) > 0) {
            session_ttl_ns = (uint64_t)atoi(ttl) * 1000000ull;
        }
    }
    // static, not stack: the connection array carries a full object
    // table per client, a few hundred kilobytes belong in bss
    static struct x12_conn conns[X12_MAX_CLIENTS];
    for (size_t i = 0; i < X12_MAX_CLIENTS; i++) {
        conns[i].fd = -1;
    }
    x12_selection_init(conns, X12_MAX_CLIENTS);

    while (running) {
        struct pollfd pfd[2 + X12_MAX_CLIENTS + X12_EVDEV_MAX + 1];
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
        // input sources join the same poll round behind the clients
        nfds_t n_clients = n;
        int n_file = x12_input_file_fill(&pfd[n], 1);
        n += (nfds_t)n_file;
        int n_ev = x12_evdev_fill(&pfd[n], X12_EVDEV_MAX);
        n += (nfds_t)n_ev;
        int n_li = x12_libinput_fill(&pfd[n], 1);
        n += (nfds_t)n_li;
        int rc = poll(pfd, n, 250);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }
        if (x12_backend->halt_requested()) {
            break;
        }

        if (pfd[0].revents & POLLIN) {
            int fd = accept(srv, 0, 0);
            if (fd >= 0) {
                size_t i;
                for (i = 0; i < X12_MAX_CLIENTS; i++) {
                    if (conns[i].fd < 0 && !conns[i].parked) {
                        break;
                    }
                }
                if (i == X12_MAX_CLIENTS) {
                    // the limit is loud, the extra peer just never joins
                    fprintf(stderr, "x12-server: refusing client, connection limit\n");
                    close(fd);
                } else {
                    void *tls = NULL;
                    if (x12_net_accept(tls_ctx, &addr, fd, &tls) != 0) {
                        close(fd);
                    } else {
                        x12_conn_init(&conns[i], fd);
                        conns[i].tls = tls;
                        conns[i].net = addr.kind != X12_NET_UNIX;
                    }
                }
            }
        }

        for (nfds_t k = 1; k < n_clients; k++) {
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
                // a resume moved a half dispatched batch over, it reads
                // as already arrived input
                if (c->in_len >= X12_HEADER_SIZE) {
                    x12_conn_pump(c, x12_dispatch);
                }
            }
            if (c->dead) {
                // the final error events go out before the fd goes away,
                // a loud disconnect names its reason
                x12_conn_flush(c);
                // a session survives its transport: the objects and the
                // scene presence wait behind the resume token
                if (!x12_conn_park(c, session_ttl_ns)) {
                    client_teardown(c);
                    // fully reaped: the later pass must not run the
                    // teardown a second time
                    c->dead = 0;
                }
                else {
                    // the transport is gone: a parked client cannot
                    // serve a selection transfer, but its windows and
                    // scene presence wait for the resume
                    x12_selection_release_owner(c);
                }
                x12_net_tls_free(c->tls);
                c->tls = 0;
                close(c->fd);
                c->fd = -1;
            }
        }
        // the resume window of disconnected sessions runs out here
        x12_conn_parked_sweep((uint64_t)x12_now_ns(), client_teardown);
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
                if (!x12_conn_park(&conns[i], session_ttl_ns)) {
                    client_teardown(&conns[i]);
                    conns[i].dead = 0;
                }
                else {
                    x12_selection_release_owner(&conns[i]);
                }
                x12_net_tls_free(conns[i].tls);
                conns[i].tls = 0;
                close(conns[i].fd);
                conns[i].fd = -1;
            }
            if (conns[i].dead && conns[i].fd < 0 && !conns[i].parked) {
                // the transport went to a resumed session, whatever the
                // fresh conn owned goes now
                client_teardown(&conns[i]);
                conns[i].dead = 0;
            }
        }
        // sources pump what arrived this round, the router turns it into
        // seat events before the composite pass reads the pointer
        x12_input_file_pump(&pfd[n_clients], n_file);
        x12_evdev_pump(&pfd[n_clients + n_file], n_ev);
        x12_libinput_pump(&pfd[n_clients + n_file + n_ev], n_li);
        x12_input_dispatch();
        // the composite pass repaints whatever the commits damaged, one
        // pass per loop round keeps the damage accounting single writer
        x12_comp_pass(conns, X12_MAX_CLIENTS);
        x12_backend->pump();
        // flip landings become present feedback, replacements become
        // release credits for the buffers that waited on screen
        x12_comp_scanout_pump();
        // cables move: outputs announce themselves, dead heads say so
        // and the canvas follows the bounding box of the live ones
        sync_outputs(conns, X12_MAX_CLIENTS);
    }

    for (size_t i = 0; i < X12_MAX_CLIENTS; i++) {
        if (conns[i].fd >= 0) {
            close(conns[i].fd);
        }
    }
    x12_gpu_shutdown();
    x12_libinput_stop();
    x12_backend->shutdown();
    close(srv);
    if (addr.kind == X12_NET_UNIX) {
        unlink(addr.path);
    }
    fprintf(stderr, "x12-server: bye\n");
    return 0;
}

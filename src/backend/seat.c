// Copyright (c) lezginskiy <xanother-x12@proton.me>
// Copyright (c) fluent1s (akemi) <m1st1kad3ad1y@gmail.com>
// SPDX-License-Identifier: MIT

// seat plumbing: logind takes the device under our session and pokes
// us with pause/resume around vt switches. the fd itself comes from
// takedevice when the logind on the box passes live fds, some new ones
// hand out numbers that die with the reply message and there the node
// is opened raw with the master dance instead. the vt choreography is
// watched through the session signals and the active property, so a
// switch is caught either way

#include "seat.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/vt.h>
#include <unistd.h>

#include <systemd/sd-bus.h>
#include <xf86drm.h>

struct x12_seat {
    const struct x12_seat_ops *ops;
    void *ud;
    const char *node;
    int fd;
    int own_fd;
    int got_master;  // drmSetMaster landed, the drops stay gated on it
    int paused;
    int controlled;
    int tty_fd;
    int last_active;
    uint32_t major, minor;

    sd_bus *bus;
    char *path;
    sd_bus_slot *pause_slot, *resume_slot;
};

// the vt process mode talks in signals, so all they do is raise a flag
// for the pump to pick up
static volatile sig_atomic_t vt_want_away, vt_want_back;

static void on_vt_away(int sig) {
    (void)sig;
    vt_want_away = 1;
}

static void on_vt_back(int sig) {
    (void)sig;
    vt_want_back = 1;
}

static void seat_do_pause(struct x12_seat *s) {
    if (s->paused) {
        return;
    }
    s->paused = 1;
    if (s->fd >= 0 && s->got_master) {
        drmDropMaster(s->fd);
        s->got_master = 0;
        fprintf(stderr, "seat: device paused, master dropped\n");
    } else {
        fprintf(stderr, "seat: device paused\n");
    }
    if (s->ops->on_pause) {
        s->ops->on_pause(s->ud);
    }
}

static void seat_do_resume(struct x12_seat *s) {
    if (!s->paused) {
        return;
    }
    if (s->fd >= 0) {
        s->got_master = drmSetMaster(s->fd) == 0;
    }
    s->paused = 0;
    fprintf(stderr, "seat: device resumed, master %s\n",
            s->got_master ? "set" : "still busy");
    if (s->ops->on_resume) {
        s->ops->on_resume(s->ud, s->fd);
    }
}

static int on_pause_signal(sd_bus_message *m, void *ud, sd_bus_error *err) {
    struct x12_seat *s = ud;
    uint32_t major = 0, minor = 0;
    const char *type = NULL;
    (void)err;
    if (sd_bus_message_read(m, "uus", &major, &minor, &type) < 0) {
        return 0;
    }
    if (major != s->major || minor != s->minor) {
        return 0;
    }
    seat_do_pause(s);
    // the pause is only complete once we say so
    sd_bus_call_method(s->bus, "org.freedesktop.login1", s->path,
                       "org.freedesktop.login1.Session", "PauseDeviceComplete",
                       NULL, NULL, "uu", major, minor);
    return 0;
}

static int on_resume_signal(sd_bus_message *m, void *ud, sd_bus_error *err) {
    struct x12_seat *s = ud;
    uint32_t major = 0, minor = 0;
    int fd = -1;
    (void)err;
    if (sd_bus_message_read(m, "uh", &major, &minor, &fd) < 0) {
        return 0;
    }
    if (major != s->major || minor != s->minor || fd < 0) {
        return 0;
    }
    // a taken fd replaces the old one, our own raw fd stays as it is
    // and just takes the master back
    if (!s->own_fd) {
        int fresh = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (fresh >= 0) {
            if (s->fd >= 0) {
                close(s->fd);
            }
            s->fd = fresh;
        }
    }
    seat_do_resume(s);
    return 0;
}

// one look at the session active property tells whether the seat is
// on the front vt, this is the switch tell that works even on the
// logind builds whose device fds come back dead
static void poll_active(struct x12_seat *s) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    int active = 0;

    if (!s->bus || !s->path) {
        return;
    }
    if (sd_bus_get_property(s->bus, "org.freedesktop.login1", s->path,
                            "org.freedesktop.login1.Session", "Active", &err,
                            &m, "b") < 0) {
        sd_bus_error_free(&err);
        return;
    }
    sd_bus_message_read(m, "b", &active);
    sd_bus_message_unref(m);
    sd_bus_error_free(&err);
    if (active == s->last_active) {
        return;
    }
    s->last_active = active;
    if (active) {
        seat_do_resume(s);
    } else {
        seat_do_pause(s);
    }
}

// without logind the vt dance is ours: process mode makes the kernel
// ping us before it switches the screen away and when it hands the
// screen back. no controlling tty (a socket session, a container) =
// nothing to hold and nothing to do
static void vt_attach(struct x12_seat *s) {
    struct vt_mode vm;
    struct sigaction sa;

    s->tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (s->tty_fd < 0) {
        return;
    }
    memset(&vm, 0, sizeof vm);
    vm.mode = VT_PROCESS;
    vm.relsig = SIGUSR1;
    vm.acqsig = SIGUSR2;
    if (ioctl(s->tty_fd, VT_SETMODE, &vm) != 0) {
        // not a virtual terminal, the pts sessions end up here
        close(s->tty_fd);
        s->tty_fd = -1;
        return;
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_vt_away;
    sigaction(SIGUSR1, &sa, NULL);
    sa.sa_handler = on_vt_back;
    sigaction(SIGUSR2, &sa, NULL);
    fprintf(stderr, "seat: holding the vt around switches\n");
}

// the fd logind passes must survive its reply message on every
// systemd, when it does not the node is ours to open
static int own_fd_open(struct x12_seat *s) {
    s->own_fd = 1;
    s->fd = open(s->node, O_RDWR | O_CLOEXEC);
    if (s->fd < 0) {
        fprintf(stderr, "seat: cannot open drm node %s: %s\n", s->node,
                strerror(errno));
        return -1;
    }
    s->got_master = drmSetMaster(s->fd) == 0;
    if (!s->got_master) {
        fprintf(stderr,
                "seat: drm master busy on %s, working without it: %s\n",
                s->node, strerror(errno));
        // a stale master entry of a dead run drops away and the retry
        // picks the device up right after
        if (drmDropMaster(s->fd) == 0) {
            s->got_master = drmSetMaster(s->fd) == 0;
            if (s->got_master) {
                fprintf(stderr, "seat: master acquired after drop\n");
            }
        }
    }
    if (s->got_master) {
        fprintf(stderr, "seat: raw open on %s, master set\n", s->node);
    }
    return 0;
}

// the session object path comes straight from logind through
// getsessionbypid, so no id spelling of ours can disagree with the
// one logind put in the object path
static int logind_attach(struct x12_seat *s) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    const char *path = NULL;
    int r, inactive = 0;

    if (sd_bus_open_system(&s->bus) < 0) {
        fprintf(stderr, "seat: no system bus, raw open it is\n");
        return -1;
    }
    r = sd_bus_call_method(s->bus, "org.freedesktop.login1",
                           "/org/freedesktop/login1",
                           "org.freedesktop.login1.Manager", "GetSessionByPID",
                           &err, &m, "u", (uint32_t)getpid());
    if (r < 0) {
        fprintf(stderr, "seat: no logind session here, raw open it is\n");
        goto fail;
    }
    r = sd_bus_message_read(m, "o", &path);
    if (r < 0 || !path) {
        fprintf(stderr, "seat: the session has no object path\n");
        goto fail;
    }
    s->path = strdup(path);
    sd_bus_message_unref(m);
    m = NULL;

    // control before devices, logind refuses takedevice otherwise.
    // without it the session still watches, it just never gets a fd
    r = sd_bus_call_method(s->bus, "org.freedesktop.login1", s->path,
                           "org.freedesktop.login1.Session", "TakeControl",
                           &err, NULL, "b", 0);
    if (r < 0) {
        fprintf(stderr, "seat: takecontrol refused: %s\n", err.message);
        sd_bus_error_free(&err);
        err = SD_BUS_ERROR_NULL;
    } else {
        s->controlled = 1;
    }

    if (s->controlled) {
        r = sd_bus_call_method(s->bus, "org.freedesktop.login1", s->path,
                               "org.freedesktop.login1.Session", "TakeDevice",
                               &err, &m, "uu", s->major, s->minor);
        if (r < 0) {
            fprintf(stderr, "seat: takedevice refused: %s\n", err.message);
            sd_bus_error_free(&err);
            err = SD_BUS_ERROR_NULL;
        } else {
            int raw = -1;
            sd_bus_message_read(m, "hb", &raw, &inactive);
            sd_bus_message_unref(m);
            m = NULL;
            if (raw >= 0) {
                s->fd = fcntl(raw, F_DUPFD_CLOEXEC, 0);
            }
            if (s->fd >= 0) {
                s->paused = inactive ? 1 : 0;
                fprintf(stderr, "seat: session %s holds drm %u:%u\n", s->path,
                        s->major, s->minor);
            } else {
                // the fd died with its message, this logind passes
                // numbers where the socket carried none
                fprintf(stderr,
                        "seat: the takedevice fd came back dead, taking "
                        "our own\n");
                sd_bus_call_method(s->bus, "org.freedesktop.login1", s->path,
                                   "org.freedesktop.login1.Session",
                                   "ReleaseDevice", NULL, NULL, "uu", s->major,
                                   s->minor);
                sd_bus_call_method(s->bus, "org.freedesktop.login1", s->path,
                                   "org.freedesktop.login1.Session",
                                   "ReleaseControl", NULL, NULL, NULL);
                s->controlled = 0;
            }
        }
    }
    if (s->fd < 0 && own_fd_open(s) != 0) {
        goto fail;
    }

    // pause/resume arrive as session signals, the filter keeps other
    // sessions off our plate. the active poll covers the builds where
    // the signals never come
    char match[512];
    snprintf(match, sizeof match,
             "type='signal',sender='org.freedesktop.login1',path='%s',"
             "interface='org.freedesktop.login1.Session',member='PauseDevice'",
             s->path);
    sd_bus_add_match(s->bus, &s->pause_slot, match, on_pause_signal, s);
    snprintf(match, sizeof match,
             "type='signal',sender='org.freedesktop.login1',path='%s',"
             "interface='org.freedesktop.login1.Session',member='ResumeDevice'",
             s->path);
    sd_bus_add_match(s->bus, &s->resume_slot, match, on_resume_signal, s);

    // register the session type so loginctl shows a real x session.
    // best effort: older logind refuses the switch and that is fine
    sd_bus_call_method(s->bus, "org.freedesktop.login1", s->path,
                       "org.freedesktop.login1.Session", "SetType", NULL,
                       NULL, "s", "x11");
    sd_bus_error_free(&err);
    return 0;

fail:
    sd_bus_error_free(&err);
    sd_bus_message_unref(m);
    if (s->controlled) {
        sd_bus_call_method(s->bus, "org.freedesktop.login1", s->path,
                           "org.freedesktop.login1.Session", "ReleaseControl",
                           NULL, NULL, NULL);
        s->controlled = 0;
    }
    free(s->path);
    s->path = NULL;
    sd_bus_unref(s->bus);
    s->bus = NULL;
    return -1;
}

int x12_seat_open(struct x12_seat **out, const char *node,
                  const struct x12_seat_ops *ops, void *ud) {
    struct stat st;
    struct x12_seat *s = calloc(1, sizeof *s);
    if (!s) {
        return -1;
    }
    s->ops = ops;
    s->ud = ud;
    s->node = node;
    s->fd = -1;
    s->tty_fd = -1;
    s->last_active = -1;
    if (stat(node, &st) != 0 || !S_ISCHR(st.st_mode)) {
        fprintf(stderr, "seat: %s is not a device node\n", node);
        free(s);
        return -1;
    }
    s->major = major(st.st_rdev);
    s->minor = minor(st.st_rdev);

    if (logind_attach(s) != 0) {
        // no session, no bus, or logind said no: the root console and
        // container path opens the node itself and claims the master
        if (own_fd_open(s) != 0) {
            free(s);
            return -1;
        }
        vt_attach(s);
    }
    *out = s;
    return 0;
}

int x12_seat_fd(struct x12_seat *s) {
    return s->paused ? -1 : s->fd;
}

void x12_seat_pump(struct x12_seat *s) {
    // the vt flags first: releasing the screen is what the kernel is
    // waiting for, everything else can lag a turn
    if (vt_want_away && s->tty_fd >= 0) {
        vt_want_away = 0;
        seat_do_pause(s);
        ioctl(s->tty_fd, VT_RELDISP, 1);
    }
    if (vt_want_back && s->tty_fd >= 0) {
        vt_want_back = 0;
        seat_do_resume(s);
        ioctl(s->tty_fd, VT_RELDISP, VT_ACKACQ);
    }
    if (!s->bus) {
        return;
    }
    // the handlers do the work, one drain empties the queue
    while (sd_bus_process(s->bus, NULL) > 0) {
    }
    poll_active(s);
}

void x12_seat_close(struct x12_seat *s) {
    if (!s) {
        return;
    }
    if (s->bus && s->controlled) {
        sd_bus_call_method(s->bus, "org.freedesktop.login1", s->path,
                           "org.freedesktop.login1.Session", "ReleaseDevice",
                           NULL, NULL, "uu", s->major, s->minor);
        sd_bus_call_method(s->bus, "org.freedesktop.login1", s->path,
                           "org.freedesktop.login1.Session", "ReleaseControl",
                           NULL, NULL, NULL);
    }
    if (s->tty_fd >= 0) {
        // hand the vt back to the kernel before leaving, a stuck
        // process mode would freeze the console switching forever
        struct vt_mode vm;
        memset(&vm, 0, sizeof vm);
        vm.mode = VT_AUTO;
        ioctl(s->tty_fd, VT_SETMODE, &vm);
        close(s->tty_fd);
    }
    if (s->fd >= 0) {
        if (s->got_master) {
            drmDropMaster(s->fd);
        }
        close(s->fd);
    }
    sd_bus_slot_unref(s->pause_slot);
    sd_bus_slot_unref(s->resume_slot);
    sd_bus_unref(s->bus);
    free(s->path);
    free(s);
}

// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// kms backend: drives real screens over dumb buffer chains, one chain
// per output. every present copies the composited canvas into the next
// slot of the outputs the damage touched and queues a page flip per
// output, the flip completion event lands the frame and frees the slot.
// the canvas is the staging area, the chains keep the scanout safe
// while the compositor redraws

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <poll.h>
#include <time.h>

#include <libudev.h>

#include <drm_fourcc.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "backend.h"
#include "output.h"
#include "seat.h"

// one dumb buffer of the chain: dumb buffers map through the drm fd
// itself so a display only driver like vkms works without any gl stack
struct dumb_bo {
    uint32_t handle;
    uint32_t fb_id;
    uint32_t pitch;
    uint64_t size;
    void *map;
};

// a client buffer imported for direct scanout: the gem handle lives on
// this device, the fb wraps it and the flip takes it straight to the
// screen. the flip event names the slot so the progress reporting can
// tell landed and replaced apart
struct so_fb {
    uint32_t magic;  // flip event tag, tells the two flip kinds apart
    int in_use;
    int queued;
    int out;  // output the buffer scans out on
    uint32_t fb_id;
    uint32_t handle;
    uint32_t cookie;
};

// one physical head: its connector, its crtc and its own dumb chain.
// a down head keeps its slot, the neighbours never move
struct output {
    uint32_t magic;  // flip event tag
    int live;
    uint32_t conn_id, crtc_id;
    drmModeModeInfo mode;
    int32_t x, y;  // position on the global canvas
    uint32_t mm_w, mm_h;
    char name[32];

    struct dumb_bo slot[2];
    int front;    // slot on screen
    int pending;  // slot queued for the next flip, -1 = none
};

#define FLIP_MAGIC_OUT 0x4f555450u
#define FLIP_MAGIC_SO 0x5343414eu

#define X12_OUT_MAX 4

static int card_fd = -1;
static struct x12_seat *seat;
static int seat_paused;
// the ppm watch sits next to the real screen when a path was named,
// the nameless fallback keeps pixels for the test tools
static char ppm_path[512];
static drmEventContext evctx;

static void recheck_connectors(void);
static struct output outs[X12_OUT_MAX];
static int n_out_slots;
static uint32_t used_crtcs[32];
static int n_used_crtcs;

static struct so_fb so[2];
static struct so_fb *so_front[X12_OUT_MAX];
static struct x12_scanout_event so_evq[8];
static int so_evq_n;

// hotplug: udev watches the drm subsystem, the kernel fd carries no in
// band message when a cable moves
static struct udev *udev_ctx;
static struct udev_monitor *udev_mon;
static struct x12_output_event out_evq[8];
static int out_evq_n;

static void push_out_event(int kind, int out) {
    if (out_evq_n < (int)(sizeof out_evq / sizeof out_evq[0])) {
        out_evq[out_evq_n].kind = kind;
        out_evq[out_evq_n].out = out;
        out_evq_n++;
    }
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void push_event(int kind, uint32_t cookie, uint64_t ts, int out) {
    if (so_evq_n < (int)(sizeof so_evq / sizeof so_evq[0])) {
        so_evq[so_evq_n].kind = kind;
        so_evq[so_evq_n].cookie = cookie;
        so_evq[so_evq_n].ts = ts;
        so_evq[so_evq_n].out = out;
        so_evq_n++;
    }
}

// dumb buffer lifecycle through the raw drm ioctls
static int dumb_alloc(int fd, struct dumb_bo *b, uint32_t w, uint32_t h) {
    struct drm_mode_create_dumb arg = { .width = w, .height = h, .bpp = 32 };
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &arg) != 0) {
        return -1;
    }
    b->handle = arg.handle;
    b->pitch = arg.pitch;
    b->size = arg.size;
    if (drmModeAddFB(fd, w, h, 24, 32, b->pitch, b->handle, &b->fb_id) !=
        0) {
        return -1;
    }
    struct drm_mode_map_dumb map = { .handle = b->handle };
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
        return -1;
    }
    b->map = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                  map.offset);
    if (b->map == MAP_FAILED) {
        b->map = NULL;
        return -1;
    }
    return 0;
}

static void dumb_free(int fd, struct dumb_bo *b) {
    if (b->map) {
        munmap(b->map, b->size);
        b->map = NULL;
    }
    if (b->fb_id) {
        drmModeRmFB(fd, b->fb_id);
        b->fb_id = 0;
    }
    if (b->handle) {
        struct drm_mode_destroy_dumb arg = { .handle = b->handle };
        drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &arg);
        b->handle = 0;
    }
}

static void so_free(struct so_fb *b) {
    if (b->fb_id) {
        drmModeRmFB(card_fd, b->fb_id);
    }
    if (b->handle) {
        struct drm_gem_close gc = { .handle = b->handle };
        drmIoctl(card_fd, DRM_IOCTL_GEM_CLOSE, &gc);
    }
    b->fb_id = 0;
    b->handle = 0;
    b->in_use = 0;
    b->queued = 0;
}

static int mode_millihz(const drmModeModeInfo *m) {
    if (!m->htotal || !m->vtotal) {
        return 0;
    }
    return (int)((uint64_t)m->clock * 1000000ull /
                 ((uint64_t)m->htotal * m->vtotal));
}

// the kernel name pattern for a connector: type letter run plus the
// type id, the same spelling the drm sysfs tree uses
static const char *type_name(uint32_t type) {
    switch (type) {
    case DRM_MODE_CONNECTOR_VGA: return "VGA";
    case DRM_MODE_CONNECTOR_DVII: return "DVI-I";
    case DRM_MODE_CONNECTOR_DVID: return "DVI-D";
    case DRM_MODE_CONNECTOR_DVIA: return "DVI-A";
    case DRM_MODE_CONNECTOR_Composite: return "Composite";
    case DRM_MODE_CONNECTOR_SVIDEO: return "SVIDEO";
    case DRM_MODE_CONNECTOR_LVDS: return "LVDS";
    case DRM_MODE_CONNECTOR_Component: return "Component";
    case DRM_MODE_CONNECTOR_9PinDIN: return "DIN";
    case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
    case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
    case DRM_MODE_CONNECTOR_HDMIB: return "HDMI-B";
    case DRM_MODE_CONNECTOR_TV: return "TV";
    case DRM_MODE_CONNECTOR_eDP: return "eDP";
    case DRM_MODE_CONNECTOR_DSI: return "DSI";
    case DRM_MODE_CONNECTOR_DPI: return "DPI";
    case DRM_MODE_CONNECTOR_WRITEBACK: return "Writeback";
    case DRM_MODE_CONNECTOR_SPI: return "SPI";
    case DRM_MODE_CONNECTOR_USB: return "USB";
    default: return "Virtual";
    }
}

static void fill_black(struct dumb_bo *b, uint32_t w, uint32_t h) {
    for (uint32_t y = 0; y < h; y++) {
        memset((uint8_t *)b->map + (size_t)y * b->pitch, 0, (size_t)w * 4);
    }
}

// a connector takes a crtc no other head drives. the encoder behind it
// names the candidates, the bit mask keeps the picks apart
static uint32_t pick_crtc(drmModeRes *res, const drmModeConnector *conn) {
    if (conn->encoder_id) {
        drmModeEncoder *enc = drmModeGetEncoder(card_fd, conn->encoder_id);
        if (enc) {
            uint32_t id = enc->crtc_id;
            drmModeFreeEncoder(enc);
            for (int i = 0; i < n_used_crtcs; i++) {
                if (used_crtcs[i] == id) {
                    return 0;
                }
            }
            if (id) {
                return id;
            }
        }
    }
    for (int e = 0; e < conn->count_encoders; e++) {
        drmModeEncoder *ce = drmModeGetEncoder(card_fd, conn->encoders[e]);
        if (!ce) {
            continue;
        }
        uint32_t id = 0;
        for (int c = 0; c < res->count_crtcs; c++) {
            if (!(ce->possible_crtcs & (1u << c))) {
                continue;
            }
            int used = 0;
            for (int i = 0; i < n_used_crtcs; i++) {
                if (used_crtcs[i] == res->crtcs[c]) {
                    used = 1;
                    break;
                }
            }
            if (!used) {
                id = res->crtcs[c];
                break;
            }
        }
        drmModeFreeEncoder(ce);
        if (id) {
            return id;
        }
    }
    return 0;
}

// a head that goes down keeps its slot and its place on the canvas,
// only the chain and the scanout state die with the connector
static void out_take_down(struct output *o) {
    int oi = (int)(o - outs);
    uint64_t ts = now_ns();
    // scanout buffers riding this head never land now, their sessions
    // get the replacement credit instead of a lost buffer
    for (int i = 0; i < 2; i++) {
        if (so[i].in_use && so[i].out == oi) {
            push_event(X12_SCANOUT_REPLACED, so[i].cookie, ts, oi);
            so_free(&so[i]);
            if (so_front[oi] == &so[i]) {
                so_front[oi] = 0;
            }
        }
    }
    drmModeSetCrtc(card_fd, o->crtc_id, 0, 0, 0, NULL, 0, NULL);
    for (int k = 0; k < 2; k++) {
        dumb_free(card_fd, &o->slot[k]);
    }
    o->front = 0;
    o->pending = -1;
    o->live = 0;
    fprintf(stderr, "kms backend: output %d (%s) down\n", oi, o->name);
    push_out_event(X12_OUTPUT_REMOVED, oi);
}

// a returning connector gets a fresh pair of dumb buffers on its own
// mode, the tiling slot and the instance stay where they were
static int out_rearm(int oi) {
    struct output *o = &outs[oi];
    drmModeConnector *conn = drmModeGetConnector(card_fd, o->conn_id);
    if (!conn || conn->connection != DRM_MODE_CONNECTED ||
        conn->count_modes == 0) {
        if (conn) {
            drmModeFreeConnector(conn);
        }
        return 0;
    }
    o->mode = conn->modes[0];
    drmModeFreeConnector(conn);
    uint32_t w = o->mode.hdisplay, h = o->mode.vdisplay;
    for (int i = 0; i < 2; i++) {
        if (dumb_alloc(card_fd, &o->slot[i], w, h) != 0) {
            for (int k = 0; k < i; k++) {
                dumb_free(card_fd, &o->slot[k]);
            }
            return 0;
        }
        fill_black(&o->slot[i], w, h);
    }
    if (drmModeSetCrtc(card_fd, o->crtc_id, o->slot[0].fb_id, 0, 0,
                       &o->conn_id, 1, &o->mode) != 0) {
        fprintf(stderr, "kms backend: re-arm refused on %s: %s\n", o->name,
                strerror(errno));
        for (int k = 0; k < 2; k++) {
            dumb_free(card_fd, &o->slot[k]);
        }
        return 0;
    }
    o->front = 0;
    o->pending = -1;
    o->live = 1;
    fprintf(stderr, "kms backend: output %d (%s) back\n", oi, o->name);
    push_out_event(X12_OUTPUT_ADDED, oi);
    return 1;
}

// the seat pulls the master away around vt switches. pause drops the
// per fd state (gem handles die with the fd), the sessions riding a
// scanout get the replacement credit their frames never landed
static void kms_seat_pause(void *ud) {
    (void)ud;
    seat_paused = 1;
    for (int i = 0; i < n_out_slots; i++) {
        struct output *o = &outs[i];
        if (o->conn_id == 0) {
            continue;
        }
        for (int k = 0; k < 2; k++) {
            dumb_free(card_fd, &o->slot[k]);
        }
        o->front = 0;
        o->pending = -1;
    }
    for (int i = 0; i < 2; i++) {
        if (so[i].in_use) {
            push_event(X12_SCANOUT_REPLACED, so[i].cookie, now_ns(), so[i].out);
            so_free(&so[i]);
            for (int k = 0; k < X12_OUT_MAX; k++) {
                if (so_front[k] == &so[i]) {
                    so_front[k] = 0;
                }
            }
        }
    }
}

// resume rebuild: the chain is per fd state and the fd just changed,
// the mode and the slot stay where they were. a refused crtc means
// the head is gone for real and the hotplug path owns it from here
static void out_rebuild(int oi) {
    struct output *o = &outs[oi];
    uint32_t w = o->mode.hdisplay, h = o->mode.vdisplay;
    for (int i = 0; i < 2; i++) {
        if (dumb_alloc(card_fd, &o->slot[i], w, h) != 0) {
            for (int k = 0; k < i; k++) {
                dumb_free(card_fd, &o->slot[k]);
            }
            out_take_down(o);
            return;
        }
        fill_black(&o->slot[i], w, h);
    }
    if (drmModeSetCrtc(card_fd, o->crtc_id, o->slot[0].fb_id, 0, 0,
                       &o->conn_id, 1, &o->mode) != 0) {
        fprintf(stderr, "kms backend: resume refused on %s: %s\n", o->name,
                strerror(errno));
        for (int k = 0; k < 2; k++) {
            dumb_free(card_fd, &o->slot[k]);
        }
        out_take_down(o);
        return;
    }
    o->front = 0;
    o->pending = -1;
    fprintf(stderr, "kms backend: output %d (%s) back on its feet\n", oi,
            o->name);
}

static void kms_seat_resume(void *ud, int fd) {
    (void)ud;
    card_fd = fd;
    seat_paused = 0;
    for (int i = 0; i < n_out_slots; i++) {
        if (outs[i].conn_id != 0) {
            out_rebuild(i);
        }
    }
    // cables may have moved while the vt was dark
    recheck_connectors();
}

// claims a free slot for one connected connector and arms its chain.
// returns the slot or -1 when nothing is free or usable
static int output_bring_up(drmModeRes *res, uint32_t conn_id) {
    drmModeConnector *conn = drmModeGetConnector(card_fd, conn_id);
    if (!conn) {
        return -1;
    }
    if (conn->connection != DRM_MODE_CONNECTED || conn->count_modes == 0 ||
        conn->connector_type == DRM_MODE_CONNECTOR_WRITEBACK) {
        drmModeFreeConnector(conn);
        return -1;
    }
    uint32_t crtc_id = pick_crtc(res, conn);
    if (!crtc_id) {
        drmModeFreeConnector(conn);
        return -1;
    }
    int idx = -1;
    for (int i = 0; i < X12_OUT_MAX; i++) {
        if (!outs[i].live && outs[i].conn_id == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0 && n_out_slots < X12_OUT_MAX) {
        idx = n_out_slots;
    }
    if (idx < 0) {
        drmModeFreeConnector(conn);
        return -1;
    }
    struct output *o = &outs[idx];
    memset(o, 0, sizeof *o);
    o->magic = FLIP_MAGIC_OUT;
    o->conn_id = conn_id;
    o->crtc_id = crtc_id;
    o->mode = conn->modes[0];
    o->mm_w = conn->mmWidth;
    o->mm_h = conn->mmHeight;
    snprintf(o->name, sizeof o->name, "%s-%u", type_name(conn->connector_type),
             conn->connector_type_id);
    drmModeFreeConnector(conn);
    if (idx >= n_out_slots) {
        n_out_slots = idx + 1;
    }
    if (n_used_crtcs < (int)(sizeof used_crtcs / sizeof used_crtcs[0])) {
        used_crtcs[n_used_crtcs++] = crtc_id;
    }
    // outputs tile left to right at y zero, a fresh head takes the next
    // free place on the right edge of what is already there
    int32_t x = 0;
    for (int i = 0; i < n_out_slots; i++) {
        if (i != idx && outs[i].live &&
            outs[i].x + (int32_t)outs[i].mode.hdisplay > x) {
            x = outs[i].x + (int32_t)outs[i].mode.hdisplay;
        }
    }
    o->x = x;
    o->y = 0;

    uint32_t w = o->mode.hdisplay, h = o->mode.vdisplay;
    for (int i = 0; i < 2; i++) {
        if (dumb_alloc(card_fd, &o->slot[i], w, h) != 0) {
            fprintf(stderr, "kms backend: dumb buffer refused on %s\n",
                    o->name);
            for (int k = 0; k < i; k++) {
                dumb_free(card_fd, &o->slot[k]);
            }
            o->conn_id = 0;
            o->magic = 0;
            return -1;
        }
        fill_black(&o->slot[i], w, h);
    }
    if (drmModeSetCrtc(card_fd, o->crtc_id, o->slot[0].fb_id, 0, 0,
                       &o->conn_id, 1, &o->mode) != 0) {
        fprintf(stderr, "kms backend: set crtc refused on %s: %s\n", o->name,
                strerror(errno));
        for (int k = 0; k < 2; k++) {
            dumb_free(card_fd, &o->slot[k]);
        }
        o->conn_id = 0;
        o->magic = 0;
        return -1;
    }
    o->front = 0;
    o->pending = -1;
    o->live = 1;
    return idx;
}

// the bounding box of every live head, the canvas the compositor draws
void x12_kms_bbox(uint32_t *w, uint32_t *h) {
    uint32_t cw = 0, ch = 0;
    for (int i = 0; i < n_out_slots; i++) {
        if (!outs[i].live) {
            continue;
        }
        if ((uint32_t)outs[i].x + outs[i].mode.hdisplay > cw) {
            cw = (uint32_t)outs[i].x + outs[i].mode.hdisplay;
        }
        if ((uint32_t)outs[i].y + outs[i].mode.vdisplay > ch) {
            ch = (uint32_t)outs[i].y + outs[i].mode.vdisplay;
        }
    }
    *w = cw;
    *h = ch;
}

static void flip_handler(int fd, unsigned int seq, unsigned int tv_sec,
                         unsigned int tv_usec, void *user) {
    (void)fd;
    (void)seq;
    uint64_t ts = (uint64_t)tv_sec * 1000000000ull + (uint64_t)tv_usec * 1000ull;
    if (!user) {
        return;
    }
    struct so_fb *probe = user;
    if (probe->magic == FLIP_MAGIC_SO) {
        struct so_fb *landed = user;
        int out = landed->out;
        // whatever this output showed before this landing is gone now,
        // a scanout buffer that leaves gets its replacement reported so
        // the release credit can go home
        if (so_front[out] && so_front[out] != landed) {
            push_event(X12_SCANOUT_REPLACED, so_front[out]->cookie, ts, out);
            so_free(so_front[out]);
            so_front[out] = 0;
        }
        // the client buffer hit the screen, the flip moment is the
        // scanout time the present feedback reports
        landed->queued = 0;
        push_event(X12_SCANOUT_LANDED, landed->cookie, ts, out);
        so_front[out] = landed;
        return;
    }
    // a dumb chain slot just landed on screen, the chain is free again
    // and whatever scanned out on that head is replaced
    struct output *o = user;
    int oi = (int)(o - outs);
    if (o->pending >= 0) {
        o->front = o->pending;
        o->pending = -1;
    }
    if (oi >= 0 && oi < X12_OUT_MAX && so_front[oi]) {
        push_event(X12_SCANOUT_REPLACED, so_front[oi]->cookie, ts, oi);
        so_free(so_front[oi]);
        so_front[oi] = 0;
    }
}

static void drain_flip_events(void) {
    struct pollfd p = { .fd = card_fd, .events = POLLIN };
    while (poll(&p, 1, 0) > 0 && (p.revents & POLLIN)) {
        if (drmHandleEvent(card_fd, &evctx) != 0) {
            return;
        }
    }
}

// one flip in flight per head, a second frame must wait its turn
static int out_busy(const struct output *o) {
    if (o->pending >= 0) {
        return 1;
    }
    int oi = (int)(o - outs);
    for (int i = 0; i < 2; i++) {
        if (so[i].in_use && so[i].queued && so[i].out == oi) {
            return 1;
        }
    }
    return 0;
}

// waits until the queued flip lands or the screen proves dead. the
// chain only holds two slots, a third frame must not race the flip
static void out_wait(struct output *o) {
    for (int spin = 0; out_busy(o) && spin < 64; spin++) {
        struct pollfd p = { .fd = card_fd, .events = POLLIN };
        if (poll(&p, 1, 100) <= 0) {
            break;
        }
        if (drmHandleEvent(card_fd, &evctx) != 0) {
            break;
        }
    }
    if (o->pending >= 0) {
        // the flip never reported, keep the chain moving anyway
        o->front = o->pending;
        o->pending = -1;
    }
}

static int kms_init(int argc, char **argv) {
    const char *node = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--kms") == 0 && i + 1 < argc) {
            node = argv[i + 1];
        }
        if (strcmp(argv[i], "--ppm") == 0 && i + 1 < argc) {
            snprintf(ppm_path, sizeof ppm_path, "%s", argv[i + 1]);
        }
    }
    if (!node) {
        fprintf(stderr, "x12-server: --kms needs a drm node\n");
        return -1;
    }
    static const struct x12_seat_ops seat_ops = {
        .on_pause = kms_seat_pause,
        .on_resume = kms_seat_resume,
    };
    if (x12_seat_open(&seat, node, &seat_ops, NULL) != 0) {
        return -1;
    }
    card_fd = x12_seat_fd(seat);

    // every connected connector claims a crtc and a chain, the heads
    // tile left to right in scan order
    drmModeRes *res = drmModeGetResources(card_fd);
    if (!res) {
        goto fail;
    }
    for (int i = 0; i < res->count_connectors && n_out_slots < X12_OUT_MAX;
         i++) {
        output_bring_up(res, res->connectors[i]);
    }
    drmModeFreeResources(res);
    if (n_out_slots == 0) {
        fprintf(stderr,
                "x12-server: no usable connector with a live crtc on %s\n",
                node);
        goto fail;
    }

    memset(&evctx, 0, sizeof evctx);
    evctx.version = DRM_EVENT_CONTEXT_VERSION;
    evctx.page_flip_handler = flip_handler;

    // hotplug arrives as a udev change on the drm subsystem, without
    // the monitor the pump falls back to polling every connector
    udev_ctx = udev_new();
    if (udev_ctx) {
        udev_mon = udev_monitor_new_from_netlink(udev_ctx, "udev");
        if (udev_mon) {
            udev_monitor_filter_add_match_subsystem_devtype(udev_mon, "drm",
                                                            NULL);
            if (udev_monitor_enable_receiving(udev_mon) == 0) {
                int mfd = udev_monitor_get_fd(udev_mon);
                int fl = fcntl(mfd, F_GETFL, 0);
                if (fl >= 0) {
                    fcntl(mfd, F_SETFL, fl | O_NONBLOCK);
                }
                printf("kms backend: hotplug monitor armed on udev\n");
            } else {
                udev_monitor_unref(udev_mon);
                udev_mon = NULL;
            }
        }
        if (!udev_mon) {
            udev_unref(udev_ctx);
            udev_ctx = NULL;
        }
    }
    return 0;

fail:
    for (int i = 0; i < X12_OUT_MAX; i++) {
        for (int k = 0; k < 2; k++) {
            dumb_free(card_fd, &outs[i].slot[k]);
        }
        outs[i].conn_id = 0;
    }
    n_out_slots = 0;
    if (card_fd >= 0) {
        drmDropMaster(card_fd);
        close(card_fd);
        card_fd = -1;
    }
    return -1;
}

static void kms_destroy(void) {
    if (udev_mon) {
        udev_monitor_unref(udev_mon);
        udev_mon = NULL;
    }
    if (udev_ctx) {
        udev_unref(udev_ctx);
        udev_ctx = NULL;
    }
    if (card_fd < 0) {
        return;
    }
    for (int i = 0; i < X12_OUT_MAX; i++) {
        struct output *o = &outs[i];
        if (o->live) {
            out_wait(o);
        }
        for (int k = 0; k < 2; k++) {
            dumb_free(card_fd, &o->slot[k]);
        }
        if (o->live && o->crtc_id) {
            drmModeSetCrtc(card_fd, o->crtc_id, 0, 0, 0, NULL, 0, NULL);
        }
        o->live = 0;
        o->conn_id = 0;
    }
    for (int i = 0; i < 2; i++) {
        so_free(&so[i]);
    }
    for (int i = 0; i < X12_OUT_MAX; i++) {
        so_front[i] = 0;
    }
    x12_seat_close(seat);
    seat = NULL;
    card_fd = -1;
}

static void kms_get_size(uint32_t *w, uint32_t *h) {
    x12_kms_bbox(w, h);
}

static void write_ppm(const char *path, const uint32_t *fb, uint32_t w,
                      uint32_t h) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return;
    }
    fprintf(f, "P6 %u %u 255\n", w, h);
    for (uint32_t i = 0; i < w * h; i++) {
        const uint8_t *p = (const uint8_t *)&fb[i];
        fputc(p[2], f);
        fputc(p[1], f);
        fputc(p[0], f);
    }
    fclose(f);
}

static void kms_present(const uint32_t *fb, const struct x12_rect *rects,
                        int n_rects) {
    if (seat_paused) {
        // the master is away, the next present after the resume
        // rebuild carries the frame
        return;
    }
    uint32_t cw = 0, ch = 0;
    x12_kms_bbox(&cw, &ch);
    int flipped = 0;
    for (int oi = 0; oi < n_out_slots; oi++) {
        struct output *o = &outs[oi];
        if (!o->live) {
            continue;
        }
        // only the outputs the damage touched flip, the rest keep
        // showing the frame they already have
        int dirty = 0;
        for (int k = 0; k < n_rects && !dirty; k++) {
            int64_t x0 = rects[k].x > o->x ? rects[k].x : o->x;
            int64_t y0 = rects[k].y > o->y ? rects[k].y : o->y;
            int64_t x1 = rects[k].x + (int64_t)rects[k].w;
            int64_t y1 = rects[k].y + (int64_t)rects[k].h;
            if (x1 > o->x + (int64_t)o->mode.hdisplay) {
                x1 = o->x + (int64_t)o->mode.hdisplay;
            }
            if (y1 > o->y + (int64_t)o->mode.vdisplay) {
                y1 = o->y + (int64_t)o->mode.vdisplay;
            }
            dirty = x1 > x0 && y1 > y0;
        }
        if (!dirty) {
            continue;
        }
        if (out_busy(o)) {
            // the previous flip has not landed, its slot must stay put
            out_wait(o);
        }
        // the flip goes to the slot the screen is not reading
        int cur = 1 - o->front;
        struct dumb_bo *bo = &o->slot[cur];
        if (!bo->map) {
            continue;
        }
        // the canvas packs rows at its own width, the dumb pitch is
        // bytes, the copy runs row by row so both stay honest
        uint32_t w = o->mode.hdisplay, h = o->mode.vdisplay;
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *src =
                (const uint8_t *)fb +
                ((size_t)(o->y + (int32_t)y) * cw + (size_t)o->x) * 4;
            memcpy((uint8_t *)bo->map + (size_t)y * bo->pitch, src,
                   (size_t)w * 4);
        }
        if (drmModePageFlip(card_fd, o->crtc_id, bo->fb_id,
                            DRM_MODE_PAGE_FLIP_EVENT, o) != 0) {
            fprintf(stderr, "x12-server: page flip failed on %s: %s\n",
                    o->name, strerror(errno));
            continue;
        }
        o->pending = cur;
        flipped = 1;
    }
    if (ppm_path[0]) {
        write_ppm(ppm_path, fb, cw, ch);
    } else if (!flipped) {
        // nothing live to show it on, keep the pixels around for the
        // test tools instead of dropping them silently
        write_ppm("screen.ppm", fb, cw, ch);
    }
}

static int kms_output_count(void) {
    return n_out_slots;
}

static int kms_output_info(int out, struct x12_out_info *info) {
    if (out < 0 || out >= n_out_slots || !outs[out].live) {
        return 0;
    }
    struct output *o = &outs[out];
    info->x = o->x;
    info->y = o->y;
    info->width = o->mode.hdisplay;
    info->height = o->mode.vdisplay;
    info->mm_width = o->mm_w;
    info->mm_height = o->mm_h;
    info->refresh_millihz = (uint32_t)mode_millihz(&o->mode);
    info->name = o->name;
    info->live = 1;
    return 1;
}

// object properties by name, the type tag keeps the lookup on the
// right drm object class
static int prop_get(uint32_t obj_type, uint32_t obj, const char *name,
                    uint64_t *val) {
    drmModeObjectPropertiesPtr p =
        drmModeObjectGetProperties(card_fd, obj, obj_type);
    if (!p) {
        return -1;
    }
    int rc = -1;
    for (uint32_t i = 0; i < p->count_props; i++) {
        drmModePropertyPtr pr = drmModeGetProperty(card_fd, p->props[i]);
        if (pr && strcmp(pr->name, name) == 0) {
            *val = p->prop_values[i];
            rc = 0;
        }
        if (pr) {
            drmModeFreeProperty(pr);
        }
        if (rc == 0) {
            break;
        }
    }
    drmModeFreeObjectProperties(p);
    return rc;
}

static int prop_set(uint32_t obj_type, uint32_t obj, const char *name,
                    uint64_t val) {
    drmModeObjectPropertiesPtr p =
        drmModeObjectGetProperties(card_fd, obj, obj_type);
    if (!p) {
        return -1;
    }
    int rc = -1;
    for (uint32_t i = 0; i < p->count_props; i++) {
        drmModePropertyPtr pr = drmModeGetProperty(card_fd, p->props[i]);
        if (pr && strcmp(pr->name, name) == 0) {
            rc = drmModeObjectSetProperty(card_fd, obj, pr->prop_id, val,
                                          pr->flags);
            drmModeFreeProperty(pr);
            break;
        }
        if (pr) {
            drmModeFreeProperty(pr);
        }
    }
    drmModeFreeObjectProperties(p);
    return rc;
}

// the panel advertises its refresh window in the edid range limits
// descriptor, the mode list only bounds the fixed rates it happens to
// expose. returns 0 when the descriptor is absent
static int edid_vrr_range(uint32_t conn_id, int *min_millihz,
                          int *max_millihz) {
    uint64_t blob_id = 0;
    if (prop_get(DRM_MODE_OBJECT_CONNECTOR, conn_id, "EDID", &blob_id) != 0 ||
        !blob_id) {
        return 0;
    }
    drmModePropertyBlobPtr b = drmModeGetPropertyBlob(card_fd, blob_id);
    if (!b) {
        return 0;
    }
    int rc = 0;
    const uint8_t *e = b->data;
    // monitor descriptors start at byte 54 of every 128 byte block
    for (size_t blk = 1; blk * 128 < b->length && !rc; blk++) {
        const uint8_t *d = e + blk * 128 + 54;
        for (int k = 0; k < 4; k++, d += 18) {
            if (d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 0xFD &&
                d[5] > 0 && d[6] >= d[5]) {
                *min_millihz = d[5] * 1000;
                *max_millihz = d[6] * 1000;
                rc = 1;
            }
        }
    }
    drmModeFreePropertyBlob(b);
    return rc;
}

static int kms_output_vrr(int out, uint32_t *min_millihz,
                          uint32_t *max_millihz) {
    if (out < 0 || out >= n_out_slots || !outs[out].live) {
        return 0;
    }
    uint64_t capable = 0;
    if (prop_get(DRM_MODE_OBJECT_CONNECTOR, outs[out].conn_id, "vrr_capable",
                 &capable) != 0 ||
        !capable) {
        return 0;
    }
    int mn = 0, mx = 0;
    if (edid_vrr_range(outs[out].conn_id, &mn, &mx) && mn > 0) {
        *min_millihz = (uint32_t)mn;
        *max_millihz = (uint32_t)mx;
        return 1;
    }
    // no range descriptor: the mode list bounds the window instead
    mn = -1;
    mx = 0;
    drmModeConnector *conn =
        drmModeGetConnector(card_fd, outs[out].conn_id);
    if (conn) {
        for (int i = 0; i < conn->count_modes; i++) {
            int hz = mode_millihz(&conn->modes[i]);
            if (hz > 0 && (mn < 0 || hz < mn)) {
                mn = hz;
            }
            if (hz > mx) {
                mx = hz;
            }
        }
        drmModeFreeConnector(conn);
    }
    if (mn < 0) {
        return 0;
    }
    *min_millihz = (uint32_t)mn;
    *max_millihz = (uint32_t)mx;
    return 1;
}

static int kms_set_vrr(int out, int enable) {
    if (out < 0 || out >= n_out_slots || !outs[out].live) {
        return -1;
    }
    uint64_t capable = 0;
    if (prop_get(DRM_MODE_OBJECT_CONNECTOR, outs[out].conn_id, "vrr_capable",
                 &capable) != 0 ||
        !capable) {
        return -1;
    }
    if (prop_set(DRM_MODE_OBJECT_CRTC, outs[out].crtc_id, "VRR_ENABLED",
                 enable ? 1 : 0) != 0) {
        return -1;
    }
    return 0;
}

// any drm change rechecks the connectors: cables that left take their
// head down and the session keeps running on the rest, new cables take
// free slots and never steal the identity of a down head
static void recheck_connectors(void) {
    for (int i = 0; i < n_out_slots; i++) {
        struct output *o = &outs[i];
        if (!o->conn_id) {
            continue;
        }
        drmModeConnector *conn = drmModeGetConnector(card_fd, o->conn_id);
        int connected = conn && conn->connection == DRM_MODE_CONNECTED &&
                        conn->count_modes > 0;
        if (conn) {
            drmModeFreeConnector(conn);
        }
        if (o->live && !connected) {
            out_take_down(o);
        } else if (!o->live && connected) {
            out_rearm(i);
        }
    }
    // connectors nobody ever saw take the next free slot
    drmModeRes *res = drmModeGetResources(card_fd);
    if (!res) {
        return;
    }
    for (int i = 0; i < res->count_connectors && n_out_slots < X12_OUT_MAX;
         i++) {
        int owned = 0;
        for (int k = 0; k < n_out_slots; k++) {
            if (outs[k].conn_id == res->connectors[i]) {
                owned = 1;
                break;
            }
        }
        if (owned) {
            continue;
        }
        drmModeConnector *conn =
            drmModeGetConnector(card_fd, res->connectors[i]);
        int connected = conn && conn->connection == DRM_MODE_CONNECTED &&
                        conn->count_modes > 0 &&
                        conn->connector_type != DRM_MODE_CONNECTOR_WRITEBACK;
        if (conn) {
            drmModeFreeConnector(conn);
        }
        if (!connected) {
            continue;
        }
        int idx = output_bring_up(res, res->connectors[i]);
        if (idx >= 0) {
            fprintf(stderr, "kms backend: output %d (%s) appeared\n", idx,
                    outs[idx].name);
            push_out_event(X12_OUTPUT_ADDED, idx);
        }
    }
    drmModeFreeResources(res);
}

static void drain_udev(void) {
    if (!udev_mon) {
        // no monitor, the connectors are polled every so often instead
        static int counter;
        if (++counter % 50 == 0) {
            recheck_connectors();
        }
        return;
    }
    int changed = 0;
    struct udev_device *dev;
    while ((dev = udev_monitor_receive_device(udev_mon)) != NULL) {
        changed = 1;
        udev_device_unref(dev);
    }
    if (changed) {
        recheck_connectors();
    }
}

static int kms_output_poll(struct x12_output_event *ev) {
    if (out_evq_n == 0) {
        return 0;
    }
    *ev = out_evq[0];
    for (int i = 1; i < out_evq_n; i++) {
        out_evq[i - 1] = out_evq[i];
    }
    out_evq_n--;
    return 1;
}

static void kms_pump(void) {
    // the seat drains its signals first: a pause may land here and
    // the rest of the turn must stay off the fd
    x12_seat_pump(seat);
    if (seat_paused) {
        return;
    }
    // flip completions land here every loop turn and free the chains,
    // cable moves arrive on the udev monitor next to them
    drain_flip_events();
    drain_udev();
}

static int kms_halt_requested(void) {
    return 0;
}

static int kms_scanout(const struct x12_scanout_req *req) {
    if (seat_paused) {
        // 0 means the caller composites this round, which is exactly
        // what a dark vt wants
        return 0;
    }
    if (!req || req->dmabuf_fd < 0) {
        return 0;
    }
    if (req->out < 0 || req->out >= n_out_slots || !outs[req->out].live) {
        return 0;
    }
    struct output *o = &outs[req->out];
    if (out_busy(o)) {
        out_wait(o);
    }
    if (out_busy(o)) {
        // a flip never reported, the caller composites this round
        return 0;
    }
    struct so_fb *b = 0;
    for (int i = 0; i < 2; i++) {
        if (!so[i].in_use) {
            b = &so[i];
            break;
        }
    }
    if (!b) {
        return 0;
    }
    // the client buffer becomes a gem handle on this device and an fb
    // the crtc can flip to directly, no copy anywhere
    uint32_t handle = 0;
    if (drmPrimeFDToHandle(card_fd, req->dmabuf_fd, &handle) != 0) {
        return 0;
    }
    uint32_t handles[4] = { handle, 0, 0, 0 };
    uint32_t pitches[4] = { req->stride, 0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };
    uint32_t fb_id = 0;
    int rc;
    if (req->modifier && req->modifier != DRM_FORMAT_MOD_INVALID) {
        uint64_t mods[4] = { req->modifier, 0, 0, 0 };
        rc = drmModeAddFB2WithModifiers(card_fd, req->width, req->height,
                                        req->format, handles, pitches,
                                        offsets, mods, &fb_id,
                                        DRM_MODE_FB_MODIFIERS);
    } else {
        rc = drmModeAddFB2(card_fd, req->width, req->height, req->format,
                           handles, pitches, offsets, &fb_id, 0);
    }
    if (rc != 0) {
        struct drm_gem_close gc = { .handle = handle };
        drmIoctl(card_fd, DRM_IOCTL_GEM_CLOSE, &gc);
        return 0;
    }
    if (drmModePageFlip(card_fd, o->crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT,
                        b) != 0) {
        fprintf(stderr, "x12-server: scanout flip failed on %s: %s\n",
                o->name, strerror(errno));
        drmModeRmFB(card_fd, fb_id);
        struct drm_gem_close gc = { .handle = handle };
        drmIoctl(card_fd, DRM_IOCTL_GEM_CLOSE, &gc);
        return 0;
    }
    b->magic = FLIP_MAGIC_SO;
    b->in_use = 1;
    b->queued = 1;
    b->out = req->out;
    b->fb_id = fb_id;
    b->handle = handle;
    b->cookie = req->cookie;
    return 1;
}

static int kms_scanout_poll(struct x12_scanout_event *ev) {
    if (so_evq_n == 0) {
        return 0;
    }
    *ev = so_evq[0];
    for (int i = 1; i < so_evq_n; i++) {
        so_evq[i - 1] = so_evq[i];
    }
    so_evq_n--;
    return 1;
}

const struct x12_backend_ops x12_backend_kms = {
    .init = kms_init,
    .get_size = kms_get_size,
    .present = kms_present,
    .output_count = kms_output_count,
    .output_info = kms_output_info,
    .output_vrr = kms_output_vrr,
    .set_vrr = kms_set_vrr,
    .output_poll = kms_output_poll,
    .pump = kms_pump,
    .halt_requested = kms_halt_requested,
    .shutdown = kms_destroy,
    .host_cursor = 0,
    .scanout = kms_scanout,
    .scanout_poll = kms_scanout_poll,
};

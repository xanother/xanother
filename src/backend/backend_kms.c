// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// kms backend: drives one real screen over a dumb buffer chain. every
// present copies the composited canvas into the next chain slot and
// queues a page flip, the flip completion event lands the frame and
// frees the slot. the canvas is the staging area, the chain keeps the
// scanout safe while the compositor redraws

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <poll.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "backend.h"
#include "output.h"

// one dumb buffer of the chain: dumb buffers map through the drm fd
// itself so a display only driver like vkms works without any gl stack
struct dumb_bo {
    uint32_t handle;
    uint32_t fb_id;
    uint32_t pitch;
    uint64_t size;
    void *map;
};

struct scanout {
    int fd;
    uint32_t conn_id, crtc_id;
    drmModeModeInfo mode;

    struct dumb_bo slot[2];
    int front;    // slot on screen
    int pending;  // slot queued for the next flip, -1 = none
    int active;
};

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

static struct scanout sc;
static const char *ppm_path = "screen.ppm";
static drmEventContext evctx;

static void flip_handler(int fd, unsigned int seq, unsigned int tv_sec,
                         unsigned int tv_usec, void *user) {
    (void)fd;
    (void)seq;
    (void)tv_sec;
    (void)tv_usec;
    (void)user;
    // the queued slot just landed on screen, the chain is free again
    if (sc.pending >= 0) {
        sc.front = sc.pending;
        sc.pending = -1;
    }
}

static void drain_flip_events(void) {
    struct pollfd p = { .fd = sc.fd, .events = POLLIN };
    while (poll(&p, 1, 0) > 0 && (p.revents & POLLIN)) {
        if (drmHandleEvent(sc.fd, &evctx) != 0) {
            return;
        }
    }
}

// waits until the queued flip lands or the screen proves dead. the
// chain only holds two slots, a third frame must not race the flip
static void wait_pending(void) {
    for (int spin = 0; sc.pending >= 0 && spin < 64; spin++) {
        struct pollfd p = { .fd = sc.fd, .events = POLLIN };
        if (poll(&p, 1, 100) <= 0) {
            break;
        }
        if (drmHandleEvent(sc.fd, &evctx) != 0) {
            break;
        }
    }
    if (sc.pending >= 0) {
        // the flip never reported, keep the chain moving anyway
        sc.front = sc.pending;
        sc.pending = -1;
    }
}

static int kms_init(int argc, char **argv) {
    const char *node = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--kms") == 0 && i + 1 < argc) {
            node = argv[i + 1];
        }
    }
    if (!node) {
        fprintf(stderr, "x12-server: --kms needs a drm node\n");
        return -1;
    }
    sc.fd = open(node, O_RDWR | O_CLOEXEC);
    if (sc.fd < 0) {
        fprintf(stderr, "x12-server: cannot open drm node %s: %s\n", node,
                strerror(errno));
        return -1;
    }
    if (drmSetMaster(sc.fd) != 0) {
        fprintf(stderr,
                "x12-server: no drm master on %s, a session still owns it: "
                "%s\n",
                node, strerror(errno));
        close(sc.fd);
        sc.fd = -1;
        return -1;
    }

    // first connected connector keeps its current mode, the encoder
    // behind it names the crtc to drive
    drmModeRes *res = drmModeGetResources(sc.fd);
    if (!res) {
        goto fail;
    }
    int found = 0;
    for (int i = 0; i < res->count_connectors && !found; i++) {
        drmModeConnector *conn =
            drmModeGetConnector(sc.fd, res->connectors[i]);
        if (!conn) {
            continue;
        }
        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            sc.conn_id = conn->connector_id;
            sc.mode = conn->modes[0];
            drmModeEncoder *enc = NULL;
            if (conn->encoder_id) {
                enc = drmModeGetEncoder(sc.fd, conn->encoder_id);
            }
            if (enc) {
                sc.crtc_id = enc->crtc_id;
                drmModeFreeEncoder(enc);
            } else {
                for (int e = 0; e < conn->count_encoders; e++) {
                    drmModeEncoder *ce =
                        drmModeGetEncoder(sc.fd, conn->encoders[e]);
                    if (!ce) {
                        continue;
                    }
                    for (int c = 0; c < res->count_crtcs; c++) {
                        if (ce->possible_crtcs & (1u << c)) {
                            sc.crtc_id = res->crtcs[c];
                            break;
                        }
                    }
                    drmModeFreeEncoder(ce);
                    if (sc.crtc_id) {
                        break;
                    }
                }
            }
            found = sc.crtc_id != 0;
        }
        drmModeFreeConnector(conn);
    }
    drmModeFreeResources(res);
    if (!found) {
        fprintf(stderr,
                "x12-server: no usable connector with a live crtc on %s\n",
                node);
        goto fail;
    }

    // two dumb buffers, the chain of the party
    uint32_t w = sc.mode.hdisplay, h = sc.mode.vdisplay;
    for (int i = 0; i < 2; i++) {
        if (dumb_alloc(sc.fd, &sc.slot[i], w, h) != 0) {
            fprintf(stderr, "x12-server: dumb buffer %d failed\n", i);
            goto fail;
        }
    }

    // take over the crtc with the first slot, the chain starts here
    if (drmModeSetCrtc(sc.fd, sc.crtc_id, sc.slot[0].fb_id, 0, 0, &sc.conn_id,
                       1, &sc.mode) != 0) {
        fprintf(stderr, "x12-server: set crtc failed on %s: %s\n", node,
                strerror(errno));
        goto fail;
    }
    sc.front = 0;
    sc.pending = -1;
    sc.active = 1;

    memset(&evctx, 0, sizeof evctx);
    evctx.version = DRM_EVENT_CONTEXT_VERSION;
    evctx.page_flip_handler = flip_handler;

    // the output global reports the real screen from here on
    static char name[32];
    snprintf(name, sizeof name, "X12-KMS-%u", sc.conn_id);
    x12_output_info(w, h, (uint32_t)(sc.mode.vrefresh * 1000u), name);
    return 0;

fail:
    for (int i = 0; i < 2; i++) {
        dumb_free(sc.fd, &sc.slot[i]);
    }
    drmDropMaster(sc.fd);
    close(sc.fd);
    sc.fd = -1;
    return -1;
}

static void kms_destroy(void) {
    if (sc.fd < 0) {
        return;
    }
    wait_pending();
    for (int i = 0; i < 2; i++) {
        dumb_free(sc.fd, &sc.slot[i]);
    }
    drmDropMaster(sc.fd);
    close(sc.fd);
    sc.fd = -1;
}

static void kms_get_size(uint32_t *w, uint32_t *h) {
    *w = sc.mode.hdisplay;
    *h = sc.mode.vdisplay;
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
    // damage rects are advisory here, the dumb chain copies the whole
    // staging canvas, correctness does not depend on the damage math
    (void)rects;
    (void)n_rects;
    if (!sc.active) {
        write_ppm(ppm_path, fb, sc.mode.hdisplay, sc.mode.vdisplay);
        return;
    }
    if (sc.pending >= 0) {
        // the previous flip has not landed, its slot must stay put
        wait_pending();
    }

    // the flip goes to the slot the screen is not reading
    int cur = 1 - sc.front;
    struct dumb_bo *bo = &sc.slot[cur];
    if (!bo->map) {
        return;
    }
    // the canvas packs rows at its own width, the dumb pitch is bytes,
    // the copy runs row by row so different pitches stay honest
    size_t row_bytes = (size_t)sc.mode.hdisplay * 4;
    for (uint32_t y = 0; y < sc.mode.vdisplay; y++) {
        memcpy((uint8_t *)bo->map + (size_t)y * bo->pitch,
               (const uint8_t *)fb + (size_t)y * row_bytes, row_bytes);
    }

    if (drmModePageFlip(sc.fd, sc.crtc_id, bo->fb_id,
                        DRM_MODE_PAGE_FLIP_EVENT, NULL) != 0) {
        fprintf(stderr, "x12-server: page flip failed: %s\n",
                strerror(errno));
        return;
    }
    sc.pending = cur;
}

static void kms_pump(void) {
    // flip completions land here every loop turn and free the chain
    drain_flip_events();
}

static int kms_halt_requested(void) {
    return 0;
}

const struct x12_backend_ops x12_backend_kms = {
    .init = kms_init,
    .get_size = kms_get_size,
    .present = kms_present,
    .pump = kms_pump,
    .halt_requested = kms_halt_requested,
    .shutdown = kms_destroy,
    .host_cursor = 0,
};

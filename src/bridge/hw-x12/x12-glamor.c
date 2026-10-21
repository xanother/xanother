// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// glamor as the rendering engine of the bridge: dix draws through
// opengl on a surfaceless egl context. the egl display rides the gbm
// platform on the display gpu, the card with a connected output, so
// the screen texture, the dri3 clients and the dmabuf target all
// share one device with the kms server; the surfaceless default
// device stays as the fallback when no card opens. the screen pixmap
// is a glamor texture, the present path reads the damaged rows back
// into the shared pool with the flip that the gl bottom left origin
// costs. structure mirrors the xwayland glamor glue, the x12 surface
// replaces the wayland compositor

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>

#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "scrnintstr.h"
#include "windowstr.h"
#include "pixmapstr.h"
#include "os.h"
#include "globals.h"
#include "glamor.h"
#include "glamor_priv.h"
#include "glamor_context.h"
#include "glamor_glx_provider.h"

int x12_glamor_active;

static EGLDisplay egl_dpy;
static EGLContext egl_ctx;
// the card fd and its gbm device stay open for the display lifetime,
// egl borrows both and never closes them
static int card_fd = -1;
static struct gbm_device *gbm_dev;
static char card_path[32];

const char *
x12_glamor_card(void)
{
    return card_fd >= 0 ? card_path : NULL;
}
static struct glamor_context glamor_ctx;
static void *staging;
static size_t staging_size;

// the glx layer caches the current context outside egl, the pump must
// keep that cache honest or the next indirect glx request lands on a
// context we displaced
extern void *lastGLContext;

static void
x12_glamor_bind_context(struct glamor_context *ctx)
{
    eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    if (!eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        ctx->ctx)) {
        FatalError("Xx12: failed to make the glamor context current");
    }
}

void
x12_glamor_make_current(void)
{
    if (lastGLContext == egl_ctx) {
        return;
    }
    lastGLContext = egl_ctx;
    x12_glamor_bind_context(&glamor_ctx);
}

// the contract glamor_init calls into: the ddx owns the egl
// environment and hands the context wiring over
void
glamor_egl_screen_init(ScreenPtr screen, struct glamor_context *ctx)
{
    (void)screen;
    *ctx = glamor_ctx;
}

// buffer export from the shared pool is the hardware wave, nothing to
// name today
int
glamor_egl_fd_name_from_pixmap(ScreenPtr screen, PixmapPtr pixmap,
                               CARD16 *stride, CARD32 *size)
{
    (void)screen;
    (void)pixmap;
    (void)stride;
    (void)size;
    return 0;
}

// the dmabuf fd export rides the same hardware wave
int
glamor_egl_fd_from_pixmap(ScreenPtr screen, PixmapPtr pixmap,
                          CARD16 *stride, CARD32 *size)
{
    (void)screen;
    (void)pixmap;
    (void)stride;
    (void)size;
    return -1;
}

int
glamor_egl_fds_from_pixmap(ScreenPtr screen, PixmapPtr pixmap, int *fds,
                           uint32_t *strides, uint32_t *offsets,
                           uint64_t *modifier)
{
    (void)screen;
    (void)pixmap;
    (void)fds;
    (void)strides;
    (void)offsets;
    (void)modifier;
    return -1;
}

// the display gpu is the card with a connected output, the same one
// the kms server scans out; X12_BRIDGE_CARD overrides the probe for
// multi gpu debugging. the first openable card is the fallback when
// no output reports connected, -1 when nothing opens at all
static int
x12_pick_card(void)
{
    const char *env = getenv("X12_BRIDGE_CARD");
    if (env && *env) {
        int fd = open(env, O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            snprintf(card_path, sizeof card_path, "%s", env);
        }
        return fd;
    }
    int fallback = -1;
    for (int i = 0; i < 8; i++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        int connected = 0;
        drmModeResPtr res = drmModeGetResources(fd);
        if (res) {
            for (int c = 0; c < res->count_connectors && !connected; c++) {
                drmModeConnectorPtr conn =
                    drmModeGetConnector(fd, res->connectors[c]);
                if (conn) {
                    connected = conn->connection == DRM_MODE_CONNECTED;
                    drmModeFreeConnector(conn);
                }
            }
            drmModeFreeResources(res);
        }
        if (connected) {
            if (fallback >= 0) {
                close(fallback);
            }
            snprintf(card_path, sizeof card_path, "%s", path);
            return fd;
        }
        if (fallback < 0) {
            fallback = fd;
            snprintf(card_path, sizeof card_path, "%s", path);
        } else {
            close(fd);
        }
    }
    return fallback;
}

// the gbm display on the display gpu first, the surfaceless default
// device when no card opens or the gbm init refuses; the display
// comes back initialized or not at all
static EGLDisplay
x12_make_display(EGLint *major, EGLint *minor)
{
    card_fd = x12_pick_card();
    if (card_fd >= 0) {
        gbm_dev = gbm_create_device(card_fd);
        if (gbm_dev) {
            EGLDisplay dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR,
                                                      gbm_dev, NULL);
            if (dpy != EGL_NO_DISPLAY &&
                eglInitialize(dpy, major, minor)) {
                return dpy;
            }
            if (dpy != EGL_NO_DISPLAY) {
                eglTerminate(dpy);
            }
        }
    }
    EGLDisplay dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA,
                                              EGL_DEFAULT_DISPLAY, NULL);
    if (dpy == EGL_NO_DISPLAY) {
        return EGL_NO_DISPLAY;
    }
    if (!eglInitialize(dpy, major, minor)) {
        eglTerminate(dpy);
        return EGL_NO_DISPLAY;
    }
    return dpy;
}

int
x12_glamor_init(ScreenPtr pScreen)
{
    const char *off = getenv("X12_BRIDGE_GLAMOR");
    EGLint major, minor;

    x12_glamor_active = 0;
    if (off && strcmp(off, "0") == 0) {
        return 0;
    }

    egl_dpy = x12_make_display(&major, &minor);
    if (egl_dpy == EGL_NO_DISPLAY) {
        ErrorF("Xx12: egl display refused, glamor off\n");
        return 0;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        ErrorF("Xx12: egl refused desktop gl, glamor off\n");
        return 0;
    }
    const EGLint ctx_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, GLAMOR_GL_CORE_VER_MAJOR,
        EGL_CONTEXT_MINOR_VERSION, GLAMOR_GL_CORE_VER_MINOR,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE,
    };
    egl_ctx = eglCreateContext(egl_dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT,
                               ctx_attrs);
    if (egl_ctx == EGL_NO_CONTEXT) {
        ErrorF("Xx12: gl core context refused, glamor off\n");
        return 0;
    }
    glamor_ctx.display = egl_dpy;
    glamor_ctx.ctx = egl_ctx;
    glamor_ctx.surface = EGL_NO_SURFACE;
    glamor_ctx.make_current = x12_glamor_bind_context;
    x12_glamor_make_current();

    const char *renderer = (const char *)glGetString(GL_RENDERER);
    if (!renderer) {
        ErrorF("Xx12: glGetString returned nothing, glamor off\n");
        return 0;
    }
    printf("X12-BRIDGE: glamor on %s\n", renderer);
    fflush(stdout);

    if (!glamor_init(pScreen, GLAMOR_USE_EGL_SCREEN)) {
        ErrorF("Xx12: glamor_init refused, glamor off\n");
        return 0;
    }
    GlxPushProvider(&glamor_provider);
    x12_glamor_active = 1;
    return 1;
}

// reads the damaged rows of the region out of the screen texture into
// the pool rows the caller commits; returns 0 when the pixmap is not
// a glamor texture and the fb copy path has to take over
int
x12_glamor_pump(PixmapPtr pix, RegionPtr rgn, unsigned char *pool)
{
    // the glamor run keeps the screen store as a gl texture; the
    // readback goes through the screen GetImage hook, the same path
    // xwd and the clients use, so the glamor internals (context,
    // framebuffer attachments, large pixmap tiles) stay behind one
    // call; zpixmap 24bpp rows land as bgrx, the pool format
    ScreenPtr pScreen = pix->drawable.pScreen;
    int w = pScreen->width;
    int h = pScreen->height;
    size_t need = (size_t)w * h * 4;

    if (need > staging_size) {
        free(staging);
        staging = malloc(need);
        staging_size = staging ? need : 0;
        if (!staging) {
            return 0;
        }
    }
    BoxPtr boxes = RegionRects(rgn);
    int n = RegionNumRects(rgn);
    for (int i = 0; i < n; i++) {
        int y0 = boxes[i].y1 < 0 ? 0 : boxes[i].y1;
        int y1 = boxes[i].y2 > h ? h : boxes[i].y2;
        int rows = y1 - y0;
        if (rows <= 0) {
            continue;
        }
        (*pScreen->GetImage) (&pix->drawable, 0, y0, w, rows, ZPixmap, ~0L, staging);
        for (int k = 0; k < rows; k++) {
            memcpy(pool + (size_t)(y0 + k) * w * 4,
                   staging + (size_t)k * w * 4, (size_t)w * 4);
        }
    }
    return 1;

}

void
x12_glamor_close(void)
{
    // the gl objects die with the context teardown, the staging buffer
    // is plain memory and survives nothing
    free(staging);
    staging = NULL;
    staging_size = 0;
}

// 'XR24' and 'AR24', the server accepts both for imports, a driver
// reports either for an opaque depth 24 texture
#define X12_DMABUF_FOURCC_XR24 0x34325258u
#define X12_DMABUF_FOURCC_AR24 0x34325241u

// flushes the gl pipeline before a commit hands the screen buffer to
// the scanout engine of another process, the dma buf reservations carry
// implicit fences on the same device but the skeleton stays conservative
void
x12_glamor_finish(void)
{
    if (!x12_glamor_active) {
        return;
    }
    x12_glamor_make_current();
    glFinish();
}

// the egl display the glamor context runs on, the dri3 glue needs it
// for its own imports
EGLDisplay
x12_glamor_display(void)
{
    return egl_dpy;
}

// exports any glamor texture pixmap as a linear dmabuf; a pixmap
// without a gpu backing answers zero and the caller stays on the cpu
// path
int
x12_glamor_export_pixmap(PixmapPtr pix, int *fd, uint32_t *stride,
                         uint64_t *modifier, uint32_t *w, uint32_t *h)
{
    if (!x12_glamor_active) {
        return 0;
    }
    glamor_pixmap_private *priv = glamor_get_pixmap_private(pix);
    if (!priv || priv->type != GLAMOR_TEXTURE_ONLY ||
        !priv->fbo || priv->fbo->tex == 0) {
        return 0;
    }
    x12_glamor_make_current();

    // the mesa dmabuf export entry points are resolved lazily, a driver
    // without the extension answers null and the pump path carries on
    PFNEGLCREATEIMAGEKHRPROC create_image =
        (PFNEGLCREATEIMAGEKHRPROC) eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC destroy_image =
        (PFNEGLDESTROYIMAGEKHRPROC) eglGetProcAddress("eglDestroyImageKHR");
    PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC export_query =
        (PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC)
        eglGetProcAddress("eglExportDMABUFImageQueryMESA");
    PFNEGLEXPORTDMABUFIMAGEMESAPROC export_image =
        (PFNEGLEXPORTDMABUFIMAGEMESAPROC)
        eglGetProcAddress("eglExportDMABUFImageMESA");
    if (!create_image || !destroy_image || !export_query || !export_image) {
        ErrorF("Xx12: dmabuf export, entry points missing\n");
        return 0;
    }
    EGLImageKHR img = create_image(egl_dpy, egl_ctx, EGL_GL_TEXTURE_2D_KHR,
                                   (EGLClientBuffer) (uintptr_t) priv->fbo->tex,
                                   NULL);
    if (img == EGL_NO_IMAGE_KHR) {
        ErrorF("Xx12: dmabuf export, egl image from the screen texture "
               "refused\n");
        return 0;
    }
    int fourcc = 0;
    EGLint n_planes = 0;
    EGLuint64KHR mod = 0;
    if (!export_query(egl_dpy, img, &fourcc, &n_planes, &mod) ||
        n_planes != 1 || (fourcc != (int) X12_DMABUF_FOURCC_XR24 &&
                          fourcc != (int) X12_DMABUF_FOURCC_AR24)) {
        ErrorF("Xx12: dmabuf export query refused, fourcc=%d planes=%d\n",
               fourcc, n_planes);
        destroy_image(egl_dpy, img);
        return 0;
    }
    int fds[4] = { -1, -1, -1, -1 };
    EGLint strides[4] = { 0, 0, 0, 0 };
    EGLint offsets[4] = { 0, 0, 0, 0 };
    if (!export_image(egl_dpy, img, fds, strides, offsets)) {
        ErrorF("Xx12: dmabuf export of the screen texture refused\n");
        destroy_image(egl_dpy, img);
        return 0;
    }
    destroy_image(egl_dpy, img);
    // single plane, the server owns the descriptor from here on
    *fd = fds[0];
    *stride = (uint32_t) strides[0];
    *modifier = (uint64_t) mod;
    *w = (uint32_t) pix->drawable.width;
    *h = (uint32_t) pix->drawable.height;
    return 1;
}

// exports the screen backing texture as a linear dmabuf, the server
// imports it as a scanout framebuffer and the pump stops copying rows.
// returns 0 when the driver refuses the export, the cpu pump keeps the
// session alive in that case
int
x12_glamor_export_screen(ScreenPtr pScreen, int *fd, uint32_t *stride,
                         uint64_t *modifier, uint32_t *w, uint32_t *h)
{
    if (!x12_glamor_active) {
        return 0;
    }
    PixmapPtr pix = pScreen->GetScreenPixmap(pScreen);
    if (!pix) {
        return 0;
    }
    return x12_glamor_export_pixmap(pix, fd, stride, modifier, w, h);
}

// the dmabuf targets of the scanout: two dumb allocations on the render
// node, imported as textures, exported back as the linear dmabufs the
// server page flips between. the pump always blits the screen into the
// buffer the crtc is not scanning out, a partial blit lands on top of
// the previous frame instead of on top of the live scanout
#define X12_SCANOUT_TARGETS 2
static struct {
    int dmabuf_fd;
    EGLImageKHR image;
    GLuint tex;
    GLuint fbo;
    int w, h;
} targets[X12_SCANOUT_TARGETS];
static int targets_live;
static int target_cur;
static GLuint screen_fbo;
static GLuint screen_tex;

#include <xf86drm.h>
#include "x12-dri3.h"

void
x12_glamor_target_destroy(void)
{
    for (int i = 0; i < X12_SCANOUT_TARGETS; i++) {
        if (targets[i].image != EGL_NO_IMAGE_KHR) {
            eglDestroyImageKHR(egl_dpy, targets[i].image);
            targets[i].image = EGL_NO_IMAGE_KHR;
        }
        if (targets[i].tex) {
            glDeleteTextures(1, &targets[i].tex);
            targets[i].tex = 0;
        }
        if (targets[i].fbo) {
            glDeleteFramebuffers(1, &targets[i].fbo);
            targets[i].fbo = 0;
        }
        if (targets[i].dmabuf_fd >= 0) {
            close(targets[i].dmabuf_fd);
            targets[i].dmabuf_fd = -1;
        }
        targets[i].w = targets[i].h = 0;
    }
    targets_live = 0;
    target_cur = 0;
    if (screen_fbo) {
        glDeleteFramebuffers(1, &screen_fbo);
        screen_fbo = 0;
    }
    screen_tex = 0;
}

// allocates a linear scanout buffer on the render node, imports it as
// the blit target texture and hands the dmabuf to the caller for the
// server import; zero on any refusal, the caller stays on the pump
int
x12_glamor_target_create(ScreenPtr pScreen, uint32_t w, uint32_t h,
                         int *fd, uint32_t *stride)
{
    if (!x12_glamor_active) {
        return 0;
    }
    if (targets_live == X12_SCANOUT_TARGETS) {
        return 0;
    }
    int slot = targets_live;
    int node = x12_dri3_render_node();
    if (node < 0) {
        ErrorF("Xx12: dmabuf target, no render node shared with the "
               "screen\n");
        return 0;
    }
    // dumb allocations live on the primary node only, the render node
    // denies every mode ioctl; the dmabuf keeps the memory alive after
    // the primary fd closes
    const char *card = drmGetPrimaryDeviceNameFromFd(node);
    int cfd = open(card, O_RDWR | O_CLOEXEC);
    if (cfd < 0) {
        ErrorF("Xx12: dmabuf target, primary node %s refused\n", card);
        return 0;
    }
    struct drm_mode_create_dumb cd = { .width = w, .height = h, .bpp = 32 };
    if (drmIoctl(cfd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) != 0) {
        ErrorF("Xx12: dmabuf target, dumb alloc refused\n");
        close(cfd);
        return 0;
    }
    int dfd;
    if (drmPrimeHandleToFD(cfd, cd.handle, DRM_CLOEXEC, &dfd) != 0) {
        struct drm_mode_destroy_dumb dd = { .handle = cd.handle };
        drmIoctl(cfd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
        close(cfd);
        ErrorF("Xx12: dmabuf target, prime handle refused\n");
        return 0;
    }
    close(cfd);
    EGLint attrs[] = {
        EGL_WIDTH, (EGLint) w,
        EGL_HEIGHT, (EGLint) h,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint) 0x34325258u,
        EGL_DMA_BUF_PLANE0_FD_EXT, dfd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint) cd.pitch,
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 0,
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0,
        EGL_NONE,
    };
    EGLImageKHR img = eglCreateImageKHR(egl_dpy, EGL_NO_CONTEXT,
                                        EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    if (img == EGL_NO_IMAGE_KHR) {
        ErrorF("Xx12: dmabuf target, egl import refused\n");
        close(dfd);
        return 0;
    }
    x12_glamor_make_current();
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
    glBindTexture(GL_TEXTURE_2D, 0);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        ErrorF("Xx12: dmabuf target, fbo incomplete %x\n", st);
        glDeleteTextures(1, &tex);
        glDeleteFramebuffers(1, &fbo);
        eglDestroyImageKHR(egl_dpy, img);
        close(dfd);
        return 0;
    }
    // the wire travels with a duplicate, the original stays for the
    // target lifetime and dies with it
    int wire = dup(dfd);
    if (wire < 0) {
        x12_glamor_target_destroy();
        close(dfd);
        return 0;
    }
    targets[slot].dmabuf_fd = dfd;
    targets[slot].image = img;
    targets[slot].tex = tex;
    targets[slot].fbo = fbo;
    targets[slot].w = (int) w;
    targets[slot].h = (int) h;
    targets_live++;
    *fd = wire;
    *stride = cd.pitch;
    return 1;
}

// the swap chain parks the freshly filled buffer on the surface and
// moves the blit destination to the one the crtc just stopped reading
void
x12_glamor_target_flip(void)
{
    if (targets_live == X12_SCANOUT_TARGETS) {
        target_cur ^= 1;
    }
}

int
x12_glamor_target_current(void)
{
    return target_cur;
}

// flips the damaged boxes of the screen texture into the scanout
// target, the gl bottom up row order of the screen memory never reaches
// the scanout which reads top down
void
x12_glamor_target_blit(ScreenPtr pScreen, RegionPtr rgn)
{
    PixmapPtr pix = pScreen->GetScreenPixmap(pScreen);
    if (!pix || !targets_live) {
        return;
    }
    glamor_pixmap_private *priv = glamor_get_pixmap_private(pix);
    if (!priv || !priv->fbo || priv->fbo->tex == 0) {
        return;
    }
    x12_glamor_make_current();
    if (screen_tex != priv->fbo->tex) {
        if (screen_fbo) {
            glDeleteFramebuffers(1, &screen_fbo);
        }
        glGenFramebuffers(1, &screen_fbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, screen_fbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, priv->fbo->tex, 0);
        screen_tex = priv->fbo->tex;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, screen_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, targets[target_cur].fbo);
    int h = pix->drawable.height;
    BoxPtr boxes = RegionRects(rgn);
    int n = RegionNumRects(rgn);
    for (int i = 0; i < n; i++) {
        // glamor renders the dix top row into the gl last row, so the
        // source rect mirrors the dix damage box into gl space while
        // the target fbo rows read top down like the scanout: dix row
        // r lives in the target row r, the flip lives in the source
        int sy1 = h - boxes[i].y1;
        int sy0 = h - boxes[i].y2;
        glBlitFramebuffer(boxes[i].x1, sy0, boxes[i].x2, sy1,
                          boxes[i].x1, boxes[i].y1,
                          boxes[i].x2, boxes[i].y2,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

// reads a rectangle of the screen texture into a caller buffer of
// w*h*4 bgrx rows in dix top down order; the gl readback rows come
// bottom up, so the copy flips them; answers zero when the pixmap is
// not a glamor texture and the caller falls back
int
x12_glamor_read_texture(PixmapPtr pix, int x, int y, int w, int h,
                        unsigned char *out)
{
    glamor_pixmap_private *priv = glamor_get_pixmap_private(pix);
    if (!x12_glamor_active || !priv || !priv->fbo || priv->fbo->tex == 0) {
        return 0;
    }
    x12_glamor_make_current();
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, priv->fbo->tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        return 0;
    }
    int pw = pix->drawable.width;
    int ph = pix->drawable.height;
    unsigned char *rows = malloc((size_t) w * h * 4);
    if (!rows) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        return 0;
    }
    glReadPixels(x, ph - y - h, w, h, GL_BGRA, GL_UNSIGNED_BYTE, rows);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    // bottom up gl rows arrive, write them top down into the buffer
    for (int r = 0; r < h; r++) {
        memcpy(out + (size_t) r * w * 4,
               rows + (size_t) (h - 1 - r) * w * 4, (size_t) w * 4);
    }
    free(rows);
    return 1;
}

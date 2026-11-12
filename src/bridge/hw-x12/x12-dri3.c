// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// dri3 plumbing of the bridge: clients allocate their buffers on the
// render node of the gpu the glamor context runs on and the buffers
// ride in as dmabufs, becoming glamor textures without any copy. the
// egl display rides the gbm device of the display gpu, so the import
// path goes through egl images directly and the device probe finds
// the render node whose primary device is that same card. a bare
// import test is not enough on multi gpu: some drivers accept
// foreign buffers and the first node in readdir order would win. a
// driver without export support (software renderers) fails the
// probe, clients fall back to drisw and nothing else changes

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <xf86drm.h>

#include "scrnintstr.h"
#include "pixmapstr.h"
#include "os.h"
#include "globals.h"
#include "glamor.h"
#include "glamor_priv.h"
#include "glamor_context.h"
#include "dri3.h"
#include "x12-glamor.h"

// the egl display handle lives in the glamor glue, the header stays
// free of egl types for the non egl build units
extern EGLDisplay x12_glamor_display(void);

// 'XR24' and 'AR24', the two formats the chain speaks, the fourcc
// follows the pixmap depth
#define X12_DRI3_FOURCC_XR24 0x34325258u
#define X12_DRI3_FOURCC_AR24 0x34325241u

// drm fourcc modifiers the chain accepts, linear keeps the server scan
// import and the cpu fallback honest
#define X12_DRI3_MOD_LINEAR 0ull

static Bool dri3_active;
static int device_fd = -1;

// the egl image backs the pixmap texture, both die with the pixmap
#define X12_DRI3_IMAGES 128
static struct {
    PixmapPtr pixmap;
    EGLImageKHR image;
} images[X12_DRI3_IMAGES];

static CloseScreenProcPtr saved_close_screen;
static DestroyPixmapProcPtr saved_destroy_pixmap;

static EGLImageKHR
x12Dri3ImageFromDmabuf(CARD8 depth, int fd, uint32_t w, uint32_t h,
                       uint32_t stride, uint32_t offset, uint64_t modifier)
{
    EGLint attrs[] = {
        EGL_WIDTH, (EGLint) w,
        EGL_HEIGHT, (EGLint) h,
        EGL_LINUX_DRM_FOURCC_EXT,
        (EGLint) (depth == 32 ? X12_DRI3_FOURCC_AR24 : X12_DRI3_FOURCC_XR24),
        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint) offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint) stride,
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint) (modifier & 0xffffffffull),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint) (modifier >> 32),
        EGL_NONE,
    };
    return eglCreateImageKHR(x12_glamor_display(), EGL_NO_CONTEXT,
                             EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
}

static void
x12Dri3ImageRelease(PixmapPtr pixmap)
{
    for (int i = 0; i < X12_DRI3_IMAGES; i++) {
        if (images[i].pixmap == pixmap) {
            eglDestroyImageKHR(x12_glamor_display(), images[i].image);
            images[i].pixmap = NULL;
            images[i].image = EGL_NO_IMAGE_KHR;
        }
    }
}

static Bool
x12Dri3DestroyPixmap(PixmapPtr pixmap)
{
    ScreenPtr pScreen = pixmap->drawable.pScreen;
    x12Dri3ImageRelease(pixmap);
    pScreen->DestroyPixmap = saved_destroy_pixmap;
    Bool ret = pScreen->DestroyPixmap(pixmap);
    pScreen->DestroyPixmap = x12Dri3DestroyPixmap;
    return ret;
}

// the texture creation mirrors the glamor egl glue: one gl texture
// bound to the egl image, wrapped as a glamor backing
static PixmapPtr
x12Dri3PixmapFromFds(ScreenPtr screen, CARD8 num_fds, const int *fds,
                     CARD16 width, CARD16 height, const CARD32 *strides,
                     const CARD32 *offsets, CARD8 depth, CARD8 bpp,
                     CARD64 modifier)
{
    if (!dri3_active || num_fds != 1 ||
        (depth != 24 && depth != 32) || width == 0 || height == 0 ||
        bpp != BitsPerPixel(depth) ||
        strides[0] < width * (CARD32)bpp / 8) {
        return NULL;
    }
    struct glamor_screen_private *glamor_priv =
        glamor_get_screen_private(screen);
    // the pixmap carries the real client geometry: the fbo the
    // texture attaches to sizes itself from the pixmap header and the
    // zero sized header every present copy used to run against turned
    // a whole client frame into a no op
    PixmapPtr pixmap = glamor_create_pixmap(screen, width, height, depth,
                                            GLAMOR_CREATE_PIXMAP_NO_TEXTURE);
    if (!pixmap) {
        return NULL;
    }
    x12_glamor_make_current();
    EGLImageKHR image = x12Dri3ImageFromDmabuf(depth, fds[0], width,
                                               height, strides[0],
                                               offsets[0],
                                               (uint64_t) modifier);
    if (image == EGL_NO_IMAGE_KHR) {
        screen->DestroyPixmap(pixmap);
        return NULL;
    }
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (!glamor_set_pixmap_texture(pixmap, texture)) {
        glDeleteTextures(1, &texture);
        eglDestroyImageKHR(x12_glamor_display(), image);
        screen->DestroyPixmap(pixmap);
        return NULL;
    }
    glamor_set_pixmap_type(pixmap, GLAMOR_TEXTURE_DRM);
    for (int i = 0; i < X12_DRI3_IMAGES; i++) {
        if (!images[i].pixmap) {
            images[i].pixmap = pixmap;
            images[i].image = image;
            break;
        }
    }
    (void) glamor_priv;
    return pixmap;
}

static int
x12Dri3FdsFromPixmap(ScreenPtr screen, PixmapPtr pixmap, int *fds,
                     uint32_t *strides, uint32_t *offsets, uint64_t *modifier)
{
    (void) screen;
    uint32_t w, h;
    if (!x12_glamor_export_pixmap(pixmap, &fds[0], &strides[0], modifier,
                                  &w, &h)) {
        return -1;
    }
    offsets[0] = 0;
    return 1;
}

// the extension frees both lists after the reply, the arrays leave
// this glue as heap allocations or the free corrupts the heap
static int
x12Dri3GetFormats(ScreenPtr screen, CARD32 *num_formats, CARD32 **formats)
{
    (void) screen;
    CARD32 *fmts = malloc(2 * sizeof(CARD32));
    if (!fmts) {
        return 0;
    }
    fmts[0] = X12_DRI3_FOURCC_XR24;
    fmts[1] = X12_DRI3_FOURCC_AR24;
    *num_formats = 2;
    *formats = fmts;
    return 1;
}

static int
x12Dri3GetModifiers(ScreenPtr screen, uint32_t format,
                    uint32_t *num_modifiers, uint64_t **modifiers)
{
    (void) screen;
    if (format != X12_DRI3_FOURCC_XR24 &&
        format != X12_DRI3_FOURCC_AR24) {
        *num_modifiers = 0;
        *modifiers = NULL;
        return 1;
    }
    uint64_t *mods = malloc(sizeof(uint64_t));
    if (!mods) {
        return 0;
    }
    mods[0] = X12_DRI3_MOD_LINEAR;
    *num_modifiers = 1;
    *modifiers = mods;
    return 1;
}

static int
x12Dri3GetDrawableModifiers(DrawablePtr draw, uint32_t format,
                            uint32_t *num_modifiers, uint64_t **modifiers)
{
    return x12Dri3GetModifiers(draw->pScreen, format, num_modifiers,
                               modifiers);
}

// finds the render node of the gpu that owns the screen texture: the
// screen dmabuf is imported into every render node and the one that
// accepts it shares the memory, so client rendering and the compositing
// read stay on the same device without a cross gpu pass
static int
x12Dri3OpenClient(ClientPtr client, ScreenPtr screen,
                  RRProviderPtr provider, int *fd)
{
    (void) client;
    (void) provider;
    if (device_fd >= 0) {
        *fd = dup(device_fd);
        return *fd >= 0 ? Success : BadMatch;
    }
    int dfd;
    uint32_t stride, w, h;
    uint64_t modifier;
    if (!x12_glamor_export_screen(screen, &dfd, &stride, &modifier, &w, &h)) {
        return BadMatch;
    }
    const char *card = x12_glamor_card();
    if (card) {
        // the render node of the glamor card: its primary device
        // name must be the card the egl display rides, the screen
        // buffer import is the sanity check on top
        DIR *cdir = opendir("/dev/dri");
        if (cdir) {
            struct dirent *cde;
            while ((cde = readdir(cdir)) != NULL) {
                if (strncmp(cde->d_name, "renderD", 7) != 0) {
                    continue;
                }
                char path[80];
                snprintf(path, sizeof path, "/dev/dri/%s", cde->d_name);
                int rfd = open(path, O_RDWR | O_CLOEXEC);
                if (rfd < 0) {
                    continue;
                }
                char *primary = drmGetPrimaryDeviceNameFromFd(rfd);
                int match = primary && strcmp(primary, card) == 0;
                free(primary);
                if (!match) {
                    close(rfd);
                    continue;
                }
                uint32_t handle;
                if (drmPrimeFDToHandle(rfd, dfd, &handle) != 0) {
                    close(rfd);
                    continue;
                }
                struct drm_gem_close gc = { .handle = handle };
                drmIoctl(rfd, DRM_IOCTL_GEM_CLOSE, &gc);
                device_fd = rfd;
                *fd = dup(device_fd);
                closedir(cdir);
                close(dfd);
                ErrorF("Xx12: dri3 device probed, clients render on %s\n",
                       path);
                return *fd >= 0 ? Success : BadMatch;
            }
            closedir(cdir);
        }
        close(dfd);
        return BadMatch;
    }
    DIR *dir = opendir("/dev/dri");
    if (!dir) {
        close(dfd);
        return BadMatch;
    }
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, "renderD", 7) != 0) {
            continue;
        }
        char path[80];
        snprintf(path, sizeof path, "/dev/dri/%s", de->d_name);
        int rfd = open(path, O_RDWR | O_CLOEXEC);
        if (rfd < 0) {
            continue;
        }
        uint32_t handle;
        if (drmPrimeFDToHandle(rfd, dfd, &handle) == 0) {
            // the gem handle lives in this process only, drop it and
            // hand the node to the client who maps its own buffers
            struct drm_gem_close gc = { .handle = handle };
            drmIoctl(rfd, DRM_IOCTL_GEM_CLOSE, &gc);
            device_fd = rfd;
            *fd = dup(device_fd);
            closedir(dir);
            close(dfd);
            ErrorF("Xx12: dri3 device probed, clients render on %s\n", path);
            return *fd >= 0 ? Success : BadMatch;
        }
        close(rfd);
    }
    closedir(dir);
    close(dfd);
    return BadMatch;
}

static const dri3_screen_info_rec x12Dri3Info = {
    .version = 2,
    .open_client = x12Dri3OpenClient,
    .pixmap_from_fds = x12Dri3PixmapFromFds,
    .fds_from_pixmap = x12Dri3FdsFromPixmap,
    .get_formats = x12Dri3GetFormats,
    .get_modifiers = x12Dri3GetModifiers,
    .get_drawable_modifiers = x12Dri3GetDrawableModifiers,
};

static Bool
x12Dri3CloseScreen(ScreenPtr screen)
{
    if (device_fd >= 0) {
        close(device_fd);
        device_fd = -1;
    }
    dri3_active = FALSE;
    screen->CloseScreen = saved_close_screen;
    return screen->CloseScreen(screen);
}

// registers the dri3 hooks after glamor is up; the opt out env keeps
// the drisw only setup for debugging
// the render node that shares the screen texture memory, the dmabuf
// target of the pump and the dri3 clients live on it; -1 when the
// probe found nothing
int
x12_dri3_render_node(void)
{
    if (device_fd >= 0) {
        return device_fd;
    }
    ScreenPtr screen = screenInfo.screens[0];
    if (!screen) {
        return -1;
    }
    int fd;
    if (x12Dri3OpenClient(NULL, screen, NULL, &fd) != Success) {
        return -1;
    }
    close(fd);
    return device_fd;
}

int
x12_dri3_init(ScreenPtr pScreen)
{
    const char *off = getenv("X12_BRIDGE_DRI3");
    if (off && strcmp(off, "0") == 0) {
        return 0;
    }
    // the imports and the probe need an egl display that talks dma
    // buffers, the glamor run provides it
    if (!x12_glamor_active) {
        return 0;
    }
    // the render node probe waits for the first target create or the
    // first dri3 client, both happen after the screen pixmap exists
    if (!dri3_screen_init(pScreen, &x12Dri3Info)) {
        ErrorF("Xx12: dri3 screen init refused, clients stay on drisw\n");
        return 0;
    }
    saved_close_screen = pScreen->CloseScreen;
    pScreen->CloseScreen = x12Dri3CloseScreen;
    saved_destroy_pixmap = pScreen->DestroyPixmap;
    pScreen->DestroyPixmap = x12Dri3DestroyPixmap;
    dri3_active = TRUE;
    ErrorF("Xx12: dri3 ready, buffers ride in as dmabufs\n");
    return 1;
}

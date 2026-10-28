// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// x12 bridge DDX: an X server whose frame store is an X12 surface.
// skeleton milestone: the server starts, serves its X socket and holds
// a live X12 session. the output wave commits this frame store to the
// X12 server as a buffer, the input wave feeds dix from X12 seat
// events. structure follows the shadow framebuffer DDX pattern

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <X11/X.h>
#include <X11/Xproto.h>
#include <X11/Xos.h>
#include "scrnintstr.h"
#include "servermd.h"
#include "fb.h"
#include "mi.h"
#include "micmap.h"
#include "mipointer.h"
#include "miline.h"
#include "globals.h"
#include "regionstr.h"
#include "shadow.h"
#include "os.h"
#include "windowstr.h"
#include "pixmapstr.h"
#include "randrstr.h"
#include "present.h"
#include "glx_extinit.h"

#include "x12-client.h"
#include "x12-dri3.h"
#include "x12-glamor.h"
#include "glamor.h"

#define X12_DEFAULT_W 800
#define X12_DEFAULT_H 480

static int x12_width, x12_height;

// the frame store dix renders into, the output wave hands this to the
// X12 server as a committed buffer
static unsigned char *fb_bits;
static int fb_padded_width;

// dirty rows the shadow layer reported while the x12 release gate was
// closed; the block handler retries them once the server freed the
// last frame
static RegionRec pending_damage;
static int pending_ready;
static ScreenPtr bridge_screen;

// an explicit env geometry owns the session size, the mode driven
// auto resize must not override the operator's choice
static int geometry_fixed;

// dix signatures for the main loop hooks, kept forward declared for
// the close path which unregisters them
static void x12BlockHandler(void *data, void *timeout);
static void x12WakeupHandler(void *data, int result);

// defined below the randr hooks, called from x12RRScreenSetSize
static Bool x12ResizeDix(ScreenPtr pScreen, int w, int h);
static GetImageProcPtr x12_saved_get_image;

// dix pixel reads of the root land on the glamor texture whose
// readback the drivers refuse through the mi path, the gl readback
// below keeps xwd and the screenshooter honest
static int
x12GetImageRoot(DrawablePtr pDraw, int x, int y, int w, int h,
                unsigned int format, unsigned long planeMask, char *dptrs)
{
    if (format != ZPixmap || (pDraw->type != DRAWABLE_WINDOW &&
                              pDraw->type != DRAWABLE_PIXMAP)) {
        return 0;
    }
    ScreenPtr pScreen = pDraw->pScreen;
    PixmapPtr pix;
    if (pDraw->type == DRAWABLE_PIXMAP) {
        pix = (PixmapPtr) pDraw;
        if (pix != pScreen->GetScreenPixmap(pScreen)) {
            return 0;
        }
    } else {
        WindowPtr win = (WindowPtr) pDraw;
        if (win != pScreen->root) {
            return 0;
        }
        pix = pScreen->GetScreenPixmap(pScreen);
    }
    if (w <= 0 || h <= 0 ||
        (uint32_t)(x + w) > (uint32_t) pScreen->width ||
        (uint32_t)(y + h) > (uint32_t) pScreen->height) {
        return 0;
    }
    // the wrapper only runs when the direct scanout mode holds, the
    // classic pump keeps the mi readback working otherwise
    if (!x12_bridge_dmabuf_active()) {
        return 0;
    }
    unsigned char *rows = malloc((size_t) w * h * 4);
    if (!rows) {
        return 0;
    }
    if (!x12_glamor_read_texture(pix, x, y, w, h, rows)) {
        free(rows);
        return 0;
    }
    unsigned char *d = (unsigned char *) dptrs;
    for (int r = 0; r < h; r++) {
        unsigned char *row = rows + (size_t) r * w * 4;
        for (int c = 0; c < w; c++) {
            unsigned char *px = row + (size_t) c * 4;
            px[0] &= planeMask & 0xff;
            px[1] &= (planeMask >> 8) & 0xff;
            px[2] &= (planeMask >> 16) & 0xff;
        }
        memcpy(d + (size_t) r * PixmapBytePad(w, 24), row, (size_t) w * 4);
    }
    free(rows);
    return 1;
}

static void
x12GetImage(DrawablePtr pDraw, int x, int y, int w, int h,
            unsigned int format, unsigned long planeMask, char *dptrs)
{
    if (x12GetImageRoot(pDraw, x, y, w, h, format, planeMask, dptrs)) {
        return;
    }
    x12_saved_get_image(pDraw, x, y, w, h, format, planeMask, dptrs);
}

// the single output of the rootful bridge, named after the backend
// output it mirrors; the mode tracks the session geometry
#define X12_RR_MAX_OUTS 8
static RRCrtcPtr x12_rr_crtc[X12_RR_MAX_OUTS];
static RROutputPtr x12_rr_output[X12_RR_MAX_OUTS];
static int x12_rr_nouts;
static uint32_t x12_rr_outs_gen;

// heads came or went: the crtc layout survives, only the connection
// state flips, a returning head reuses the same RR output
static void
x12RRUpdateConnections(void)
{
    for (int o = 0; o < x12_rr_nouts; o++) {
        const struct x12_bridge_out *out = x12_bridge_out_at((uint32_t)o);
        if (!out) {
            continue;
        }
        RROutputSetConnection(x12_rr_output[o],
                              out->live && out->have_mode ? RR_Connected
                                                          : RR_Disconnected);
    }
}
// geometry of the exported screen dmabuf, the resize re-export checks
// the new backing against it
static uint32_t dmabuf_w, dmabuf_h;

// rebuilds the modes for the current session geometry, every monitor
// of the session gets its own mode and position so xrandr reads the
// real layout; the xrandr clients see a resolution change as a fresh
// mode on the same outputs
static void
x12RRRefreshMode(ScreenPtr pScreen, int w, int h)
{
    (void)w;
    (void)h;
    if (x12_rr_nouts <= 0) {
        return;
    }
    int dpix = monitorResolution ? monitorResolution : 100;
    for (int o = 0; o < x12_rr_nouts; o++) {
        const struct x12_bridge_out *out = x12_bridge_out_at(o);
        if (!out || !out->have_mode || !x12_rr_crtc[o] || !x12_rr_output[o]) {
            continue;
        }
        char name[64];
        snprintf(name, sizeof name, "%ux%u", out->mode_w, out->mode_h);
        xRRModeInfo modeInfo;
        memset(&modeInfo, 0, sizeof modeInfo);
        modeInfo.width = out->mode_w;
        modeInfo.height = out->mode_h;
        modeInfo.nameLength = strlen(name);
        RRModePtr mode = RRModeGet(&modeInfo, name);
        if (!mode) {
            continue;
        }
        RROutputSetModes(x12_rr_output[o], &mode, 1, 0);
        RRCrtcNotify(x12_rr_crtc[o], mode, out->org_x, out->org_y,
                     RR_Rotate_0, NULL, 1, &x12_rr_output[o]);
        RROutputSetPhysicalSize(x12_rr_output[o],
                                (out->mode_w * 25.4) / dpix,
                                (out->mode_h * 25.4) / dpix);
    }
}

static Bool
x12RRCrtcSet(ScreenPtr pScreen, RRCrtcPtr crtc, RRModePtr mode, int x,
             int y, Rotation rotation, int numOutputs,
             RROutputPtr *outputs)
{
    (void)pScreen;
    // the bridge owns the geometry, client mode requests land in the
    // notification and nothing else; the session follows the output
    return RRCrtcNotify(crtc, mode, x, y, rotation, NULL, numOutputs,
                        outputs);
}

static Bool
x12RROutputValidateMode(ScreenPtr pScreen, RROutputPtr output,
                        RRModePtr mode)
{
    rrScrPriv(pScreen);
    (void)output;
    return pScrPriv->minWidth <= mode->mode.width &&
           pScrPriv->maxWidth >= mode->mode.width &&
           pScrPriv->minHeight <= mode->mode.height &&
           pScrPriv->maxHeight >= mode->mode.height;
}

// the xwayland rootful shape: one crtc, one output, one mode, every
// resize replaces the mode; enough for xrandr reads and for i3
// the present extension rides the session flip feedback: the queue
// vblank hook parks the request and the x12_output present event fires
// it with the scanout timestamp, so completes carry real vblank times.
// the server is damage driven and stops flipping on an idle screen,
// and a present on an idle screen waits for the flip its own copy
// would cause, so every parked request arms a one shot timer at the
// refresh interval; msc counts display cycles, a refresh without a new
// frame is a real cycle too
#define X12_PRESENT_QUEUE 64
struct x12_present_wait {
    uint64_t event_id;
    OsTimerPtr timer;
};
static present_screen_info_rec x12_present_info;
static uint64_t x12_present_ust;
static uint64_t x12_present_msc;
static struct x12_present_wait x12_present_pending[X12_PRESENT_QUEUE];
static int x12_present_np;

// the refresh period of the first live output, the park delay of the
// fallback timer; an unknown rate falls back to 60 hz
static int
x12_present_interval_ms(void)
{
    uint32_t nouts = x12_bridge_out_count();
    for (uint32_t o = 0; o < nouts; o++) {
        const struct x12_bridge_out *out = x12_bridge_out_at(o);
        if (out && out->have_mode && out->refresh_millihz) {
            int ms = (int)(1000000u / out->refresh_millihz);
            return ms < 1 ? 1 : (ms > 1000 ? 1000 : ms);
        }
    }
    return 16;
}

static RRCrtcPtr
x12PresentGetCrtc(WindowPtr window)
{
    uint32_t nouts = x12_bridge_out_count();
    int32_t cx = window->drawable.x + window->drawable.width / 2;
    int32_t cy = window->drawable.y + window->drawable.height / 2;
    for (uint32_t o = 0; o < nouts && (int)o < x12_rr_nouts; o++) {
        const struct x12_bridge_out *out = x12_bridge_out_at(o);
        if (!out || !out->have_mode) {
            continue;
        }
        if (cx >= out->org_x && cx < out->org_x + (int32_t)out->mode_w &&
            cy >= out->org_y && cy < out->org_y + (int32_t)out->mode_h) {
            return x12_rr_crtc[o];
        }
    }
    return x12_rr_nouts > 0 ? x12_rr_crtc[0] : NULL;
}

static int
x12PresentGetUstMsc(RRCrtcPtr crtc, uint64_t *ust, uint64_t *msc)
{
    (void)crtc;
    // before the first flip the ust is unknown, the wall clock is the
    // honest answer and msc zero marks the session as not yet scanned
    *ust = x12_present_ust ? x12_present_ust : GetTimeInMicros();
    *msc = x12_present_msc;
    return Success;
}

static void x12_present_complete_all(uint64_t ust, uint64_t msc);

// a refresh period passed without a session flip: the display kept
// scanning out the last frame, msc follows the refresh count and the
// parked requests complete with the wall clock
static CARD32
x12PresentTimer(OsTimerPtr timer, CARD32 now, void *arg)
{
    (void)now;
    (void)arg;
    fprintf(stderr, "PDBG timer fire msc=%llu\n",
            (unsigned long long)x12_present_msc);
    // returning zero frees the one shot timer, an explicit TimerFree
    // here would double free it
    x12_present_complete_all(GetTimeInMicros(), x12_present_msc + 1);
    return 0;
}

static void
x12_present_complete_all(uint64_t ust, uint64_t msc)
{
    x12_present_msc = msc;
    x12_present_ust = ust;
    for (int i = 0; i < x12_present_np; i++) {
        if (x12_present_pending[i].timer) {
            TimerCancel(x12_present_pending[i].timer);
            TimerFree(x12_present_pending[i].timer);
        }
        present_event_notify(x12_present_pending[i].event_id, ust, msc);
    }
    x12_present_np = 0;
}

static Bool
x12PresentQueueVblank(RRCrtcPtr crtc, uint64_t event_id, uint64_t msc)
{
    (void)crtc;
    (void)msc;
    // the request completes at the next session flip or at the next
    // refresh, whichever lands first; the callers compare against
    // Success, not TRUE
    if (x12_present_np >= X12_PRESENT_QUEUE) {
        return BadAlloc;
    }
    struct x12_present_wait *w = &x12_present_pending[x12_present_np++];
    fprintf(stderr, "PDBG queue id=%llu msc=%llu\n",
            (unsigned long long)event_id, (unsigned long long)msc);
    w->event_id = event_id;
    w->timer = TimerSet(NULL, 0, x12_present_interval_ms(),
                        x12PresentTimer, &w->event_id);
    return Success;
}

static void
x12PresentAbortVblank(RRCrtcPtr crtc, uint64_t event_id, uint64_t msc)
{
    (void)crtc;
    (void)msc;
    for (int i = 0; i < x12_present_np; i++) {
        if (x12_present_pending[i].event_id != event_id) {
            continue;
        }
        if (x12_present_pending[i].timer) {
            TimerCancel(x12_present_pending[i].timer);
            TimerFree(x12_present_pending[i].timer);
        }
        x12_present_pending[i] = x12_present_pending[--x12_present_np];
        break;
    }
}

static void x12_pump_pixmap(ScreenPtr pScreen, PixmapPtr pix, RegionPtr rgn);

static void
x12PresentFlush(WindowPtr window)
{
    // the present copy landed in the window pixmap; the screen pixmap
    // only catches up through the window repaint, which pixmap damage
    // alone never triggers, so a fullscreen frame would never reach
    // the server without an explicit pump from here
    ScreenPtr pScreen = window->drawable.pScreen;
    // a windowed frame repaints through the exposure path on its own;
    // only a fullscreen window needs the explicit pump, and it reads
    // the window pixmap directly so the screen copy never happens
    if (window->drawable.width != pScreen->width ||
        window->drawable.height != pScreen->height) {
        return;
    }
    if (!x12_bridge_gate_open()) {
        return;
    }
    RegionRec region;
    RegionInit(&region, NullBox, 0);
    RegionCopy(&region, &window->clipList);
    x12_pump_pixmap(pScreen, pScreen->GetWindowPixmap(window), &region);
    RegionUninit(&region);
}

// one flip cycle of the session landed: parked requests complete with
// the scanout timestamp, a copy kind only stamps the moment
void
x12_present_flip_seen(uint64_t ts_ns, int is_flip)
{
    if (is_flip && x12_present_np > 0) {
        fprintf(stderr, "PDBG flip complete\n");
        x12_present_complete_all(ts_ns / 1000, x12_present_msc + 1);
        return;
    }
    if (is_flip) {
        x12_present_msc++;
    }
    x12_present_ust = ts_ns / 1000;
}

static void
x12PresentInit(ScreenPtr pScreen)
{
    x12_present_info.version = PRESENT_SCREEN_INFO_VERSION;
    x12_present_info.get_crtc = x12PresentGetCrtc;
    x12_present_info.get_ust_msc = x12PresentGetUstMsc;
    x12_present_info.queue_vblank = x12PresentQueueVblank;
    x12_present_info.abort_vblank = x12PresentAbortVblank;
    x12_present_info.flush = x12PresentFlush;
    present_screen_init(pScreen, &x12_present_info);
}

static Bool
x12RandRInit(ScreenPtr pScreen)
{
    rrScrPriv(pScreen);
    uint32_t pool_w, pool_h;
    x12_bridge_max_size(&pool_w, &pool_h);
    pScrPriv->rrCrtcSet = x12RRCrtcSet;
    pScrPriv->rrOutputValidateMode = x12RROutputValidateMode;
    // the client visible range is what the session can legally take,
    // the pool ceiling, not the current resolution
    RRScreenSetSizeRange(pScreen, 1, 1, pool_w, pool_h);

    // one crtc plus one output per session monitor, the name rides
    // the session announcement
    uint32_t nouts = x12_bridge_out_count();
    if (!nouts || nouts > X12_RR_MAX_OUTS) {
        return FALSE;
    }
    for (uint32_t o = 0; o < nouts; o++) {
        const struct x12_bridge_out *out = x12_bridge_out_at(o);
        x12_rr_crtc[o] = RRCrtcCreate(pScreen, NULL);
        if (!x12_rr_crtc[o]) {
            return FALSE;
        }
        RRCrtcGammaSetSize(x12_rr_crtc[o], 256);

        const char *name = out && out->name[0] ? out->name : "xanother";
        x12_rr_output[o] = RROutputCreate(pScreen, name, strlen(name), NULL);
        if (!x12_rr_output[o]) {
            return FALSE;
        }
        if (!RROutputSetClones(x12_rr_output[o], NULL, 0) ||
            !RROutputSetCrtcs(x12_rr_output[o], &x12_rr_crtc[o], 1) ||
            !RROutputSetConnection(x12_rr_output[o], RR_Connected)) {
            return FALSE;
        }
    }
    x12_rr_nouts = (int)nouts;
    x12_rr_outs_gen = x12_bridge_outs_generation();
    x12RRRefreshMode(pScreen, pScreen->width, pScreen->height);
    return TRUE;
}

static Bool
x12CursorOffScreen(ScreenPtr *ppScreen, int *x, int *y)
{
    (void)ppScreen;
    (void)x;
    (void)y;
    return FALSE;
}

static void
x12CrossScreen(ScreenPtr pScreen, Bool entering)
{
    (void)pScreen;
    (void)entering;
}

// the software cursor layer every shadow framebuffer DDX provides to
// the mi pointer code
static miPointerScreenFuncRec x12PointerCursorFuncs = {
    x12CursorOffScreen,
    x12CrossScreen,
    miPointerWarpCursor,
};

static CloseScreenProcPtr x12_saved_close;

static Bool
x12CloseScreen(ScreenPtr pScreen)
{
    if (bridge_screen == pScreen) {
        bridge_screen = NULL;
    }
    if (pending_ready) {
        RegionUninit(&pending_damage);
        pending_ready = 0;
        RemoveNotifyFd(x12_bridge_fd());
        RemoveBlockAndWakeupHandlers(x12BlockHandler, x12WakeupHandler,
                                     NULL);
    }
    free(fb_bits);
    fb_bits = NULL;
    x12_glamor_close();
    pScreen->CloseScreen = x12_saved_close;
    return (*pScreen->CloseScreen)(pScreen);
}


// copies the damaged rows out of the screen pixmap and pushes them to
// the x12 server; dix renders into the screen pixmap, which after a
// session resize is a fresh backing store and not the fbScreenInit
// buffer, so the source of truth is the pixmap itself and not our
// frame store pointer
static void
x12_pump_pixmap(ScreenPtr pScreen, PixmapPtr pix, RegionPtr rgn)
{
    uint32_t w = x12_bridge_surface_width();
    uint32_t h = x12_bridge_surface_height();
    uint32_t pitch = w * 4;
    void *pool = NULL;
    x12_bridge_output_pixels(&pool);
    if (!pool) {
        return;
    }
    if (!pix) {
        return;
    }
    int n = RegionNumRects(rgn);
    BoxPtr boxes = RegionRects(rgn);
    if (x12_bridge_dmabuf_active()) {
        // the screen renders into a glamor texture whose gl row order
        // is bottom up, the scanout reads top down: the pump flips the
        // damaged boxes into the linear scanout buffer on the gpu, one
        // blit per box, no row ever crosses the wire
        x12_glamor_target_blit(pScreen, rgn);
        x12_glamor_finish();
        // the filled buffer goes on the surface, the commit inside the
        // row push shows it and the destination hops to the buffer the
        // server just stopped scanning out
        x12_bridge_dmabuf_pin(x12_glamor_target_current());
        for (int i = 0; i < n; i++) {
            int y0 = boxes[i].y1 < 0 ? 0 : boxes[i].y1;
            int y1 = boxes[i].y2 > (int)h ? (int)h : boxes[i].y2;
            if (y1 > y0) {
                x12_bridge_commit_rows(y0, y1);
            }
        }
        x12_glamor_target_flip();
        return;
    }
    if (x12_glamor_active && x12_glamor_pump(pix, rgn, pool)) {
        for (int i = 0; i < n; i++) {
            int y0 = boxes[i].y1 < 0 ? 0 : boxes[i].y1;
            int y1 = boxes[i].y2 > (int)h ? (int)h : boxes[i].y2;
            if (y1 > y0) {
                x12_bridge_commit_rows(y0, y1);
            }
        }
        return;
    }
    if (!pix->devPrivate.ptr) {
        return;
    }
    const unsigned char *bits = pix->devPrivate.ptr;
    int stride = pix->devKind;
    for (int i = 0; i < n; i++) {
        int y0 = boxes[i].y1;
        int y1 = boxes[i].y2;
        if (y0 < 0) {
            y0 = 0;
        }
        if (y1 > (int)h) {
            y1 = (int)h;
        }
        for (int y = y0; y < y1; y++) {
            memcpy((unsigned char *)pool + (size_t)y * pitch,
                   bits + (size_t)y * stride, (size_t)w * 4);
        }
        if (y1 > y0) {
            x12_bridge_commit_rows(y0, y1);
        }
    }
}

static void
x12_pump_boxes(ScreenPtr pScreen, RegionPtr rgn)
{
    x12_pump_pixmap(pScreen, pScreen->GetScreenPixmap(pScreen), rgn);
}

static void
x12ShadowUpdate(ScreenPtr pScreen, shadowBufPtr pBuf)
{
    RegionPtr rgn = DamageRegion(pBuf->pDamage);
    if (x12_bridge_gate_open()) {
        x12_pump_boxes(pScreen, rgn);
    } else {
        // the shadow layer empties its damage right after this call,
        // a gated push has to survive somewhere until the release
        RegionUnion(&pending_damage, &pending_damage, rgn);
    }
}

// the x12 socket rides the dix main loop through the os notify layer,
// dispatch keeps the release and configure state fresh
static void
x12NotifyFd(int fd, int ready, void *data)
{
    (void)fd;
    (void)ready;
    (void)data;
    if (x12_bridge_dispatch() != 0) {
        FatalError("Xx12: the X12 server hung up");
    }
    // a configure offer moves the root to the geometry the server
    // actually gave, the fullscreen offer rides here before any mode
    // event and wins over an explicit env geometry too: the rootful
    // contract is the whole output, the env only picks the start size
    if (bridge_screen &&
        x12_rr_outs_gen != x12_bridge_outs_generation()) {
        x12_rr_outs_gen = x12_bridge_outs_generation();
        x12RRUpdateConnections();
    }
    if (bridge_screen) {
        uint32_t cw, ch;
        if (x12_bridge_take_configure(&cw, &ch) &&
            ((int)cw != bridge_screen->width ||
             (int)ch != bridge_screen->height)) {
            x12ResizeDix(bridge_screen, (int)cw, (int)ch);
            if (!x12_bridge_dmabuf_active()) {
                x12_bridge_resize(cw, ch);
            }
        }
    }
    // the mode event landed in dispatch, a changed output size moves
    // the dix geometry before the next block handler beat; an explicit
    // env geometry stays untouched
    if (bridge_screen && !geometry_fixed) {
        uint32_t w, h;
        if (x12_bridge_preferred_size(&w, &h) &&
            ((int)w != bridge_screen->width ||
             (int)h != bridge_screen->height)) {
            x12ResizeDix(bridge_screen, (int)w, (int)h);
            if (!x12_bridge_dmabuf_active()) {
                x12_bridge_resize(w, h);
            }
        }
    }
    // a release event just reopened the gate: a queued resize goes
    // out first, parked damage would copy at the wrong geometry
    // otherwise and the resize would wait forever behind the pump
    if (bridge_screen && x12_bridge_gate_open()) {
        x12_bridge_commit_queued();
    }
    // parked damage goes out right here too, waiting for the block
    // handler beat would queue whole repaints behind a busy dix main
    // loop
    if (bridge_screen && pending_ready &&
        RegionNotEmpty(&pending_damage) && x12_bridge_gate_open()) {
        x12_pump_boxes(bridge_screen, &pending_damage);
        RegionEmpty(&pending_damage);
    }
}

static void
x12BlockHandler(void *data, void *timeout)
{
    // the heartbeat flush: requests queued between readable moments of
    // the x12 socket would otherwise wait for data that never comes
    x12_bridge_flush();
    // a resize the gate refused goes out on the first open beat
    x12_bridge_commit_queued();
    if (bridge_screen && pending_ready &&
        RegionNotEmpty(&pending_damage) && x12_bridge_gate_open()) {
        x12_pump_boxes(bridge_screen, &pending_damage);
        RegionEmpty(&pending_damage);
    }
}

static void
x12WakeupHandler(void *data, int result)
{
    (void)data;
    (void)result;
}

// the dix half of a session resize: the same backing pixmap retargets
// onto fresh bits, resized root, randr notification. the x12 commit
// half rides in the client session. a fresh pixmap and a destroy of
// the old one would orphan the sprite cursor damage on a freed
// drawable and segfault the first GetImage in DamageUnregister
// windows still pointing at the retired screen pixmap reattach to
// the new one; the retired pixmap itself is never destroyed, the
// sprite cursor damage and client references keep it alive
static Bool
x12_set_pixmap_visit(WindowPtr window, void *data)
{
    ScreenPtr pScreen = window->drawable.pScreen;

    if (pScreen->GetWindowPixmap(window) == data) {
        pScreen->SetWindowPixmap(window, pScreen->GetScreenPixmap(pScreen));
        return WT_WALKCHILDREN;
    }
    return WT_DONTWALKCHILDREN;
}

// rebuilds the scanout swap chain after a resize or at session start:
// two fresh linear targets, the first one pinned and committed so the
// server holds a complete frame while the pump fills the second one
// beside the live scanout
static Bool
x12_swapchain_rebuild(ScreenPtr pScreen, uint32_t w, uint32_t h)
{
    // the old pair dies with the old geometry: the create call
    // refuses a full table, so a rebuild without the destroy would
    // always fail and the session would walk back to the pool pump
    x12_glamor_target_destroy();
    int fds[2];
    uint32_t strides[2];
    for (int i = 0; i < 2; i++) {
        if (!x12_glamor_target_create(pScreen, w, h, &fds[i], &strides[i])) {
            for (int j = 0; j < i; j++) {
                close(fds[j]);
            }
            return FALSE;
        }
    }
    x12_bridge_dmabuf_reset();
    int s0 = x12_bridge_dmabuf_attach(fds[0], strides[0], 0, w, h);
    int s1 = s0 >= 0 ? x12_bridge_dmabuf_attach(fds[1], strides[1], 0, w, h)
                     : -1;
    if (s0 < 0 || s1 < 0 || !x12_bridge_dmabuf_show(0)) {
        // a half built chain walks back to the pool pump, the wire
        // import closed the shown fd and the other one dies here
        for (int i = 0; i < 2; i++) {
            if (s1 >= 0 && i == 0) {
                continue;
            }
            close(fds[i]);
        }
        return FALSE;
    }
    // the blit destination hops to the second buffer, the first pump
    // frame lands beside the live scanout instead of on top of it
    x12_glamor_target_flip();
    dmabuf_w = w;
    dmabuf_h = h;
    return TRUE;
}

static Bool
x12ResizeDix(ScreenPtr pScreen, int w, int h)
{
    uint32_t max_w, max_h;

    if (w == pScreen->width && h == pScreen->height) {
        return TRUE;
    }
    x12_bridge_max_size(&max_w, &max_h);
    if (w < 1 || h < 1 || w > (int)max_w || h > (int)max_h) {
        return FALSE;
    }

    SetRootClip(pScreen, ROOT_CLIP_NONE);
    PixmapPtr pix = pScreen->GetScreenPixmap(pScreen);
    shadowRemove(pScreen, pix);

    if (x12_glamor_active) {
        // the screen store is a texture, it cannot retarget in place:
        // a fresh texture takes the frame, the old one retires with
        // every dix reference intact - a destroy would orphan the
        // sprite cursor damage on a freed drawable
        PixmapPtr fresh = pScreen->CreatePixmap(pScreen, w, h,
                                                pScreen->rootDepth,
                                                CREATE_PIXMAP_USAGE_BACKING_PIXMAP);
        if (!fresh) {
            FatalError("Xx12: glamor backing pixmap for %dx%d refused", w, h);
        }
        pScreen->SetScreenPixmap(fresh);
        TraverseTree(pScreen->root, x12_set_pixmap_visit, pix);
        pScreen->ResizeWindow(pScreen->root, 0, 0, w, h, NULL);
        shadowAdd(pScreen, fresh, x12ShadowUpdate, NULL, 0, NULL);
    } else {
        int bytes_pad = PixmapBytePad(w, 24);
        free(fb_bits);
        fb_padded_width = bytes_pad / 4;
        fb_bits = calloc((size_t)bytes_pad * h, 1);
        if (!fb_bits) {
            FatalError("Xx12: frame store for %dx%d refused", w, h);
        }
        // the same pixmap object retargets onto the fresh bits: the
        // sprite damage, the window pixmaps and every dix reference
        // stay valid
        pScreen->ModifyPixmapHeader(pix, w, h, pScreen->rootDepth,
                                    BitsPerPixel(pScreen->rootDepth),
                                    bytes_pad, fb_bits);
        pScreen->ResizeWindow(pScreen->root, 0, 0, w, h, NULL);
        shadowAdd(pScreen, pix, x12ShadowUpdate, NULL, 0, NULL);
    }

    pScreen->width = w;
    pScreen->height = h;
    int dpix = monitorResolution ? monitorResolution : 100;
    pScreen->mmWidth = (w * 25.4) / dpix;
    pScreen->mmHeight = (h * 25.4) / dpix;
    BoxRec box = { 0, 0, w, h };
    pScreen->root->drawable.width = w;
    pScreen->root->drawable.height = h;
    RegionReset(&pScreen->root->winSize, &box);

    SetRootClip(pScreen, ROOT_CLIP_FULL);
    RRScreenSizeNotify(pScreen);
    update_desktop_dimensions();
    // the mode follows the session, xrandr clients re-read the output
    x12RRRefreshMode(pScreen, w, h);
    // damage rects of the old geometry mean nothing on the new one,
    // the fresh root paint reopens the whole frame
    RegionEmpty(&pending_damage);
    if (x12_bridge_dmabuf_active()) {
        // the old scanout buffers die with the old geometry, a fresh
        // pair keeps the direct mode alive and a refusal walks back to
        // the pool pump through the session resize
        if (x12_swapchain_rebuild(pScreen, w, h)) {
            return TRUE;
        }
        x12_bridge_resize((uint32_t)w, (uint32_t)h);
    }
    return TRUE;
}

static Bool
x12RRScreenSetSize(ScreenPtr pScreen, CARD16 width, CARD16 height,
                   CARD32 mmWidth, CARD32 mmHeight)
{
    (void)mmWidth;
    (void)mmHeight;
    if (!x12ResizeDix(pScreen, width, height)) {
        return FALSE;
    }
    // the session surface follows the dix geometry, the release gate
    // queues the commit when the server still reads the old frame; the
    // dmabuf mode re-exports inside the dix resize instead
    if (!x12_bridge_dmabuf_active()) {
        x12_bridge_resize((uint32_t)width, (uint32_t)height);
    }
    return TRUE;
}
static Bool
x12RRGetInfo(ScreenPtr pScreen, Rotation *rotations)
{
    if (rotations) {
        *rotations = RR_Rotate_0;
    }
    (void)pScreen;
    return TRUE;
}

static CreateScreenResourcesProcPtr x12_saved_create_res;

static Bool
x12CreateScreenResources(ScreenPtr pScreen)
{
    Bool ret = (*x12_saved_create_res)(pScreen);
    if (!ret) {
        return ret;
    }
    if (x12_glamor_active) {
        // mi just retargeted the screen pixmap onto the cpu store;
        // the glamor run keeps the screen on the gpu instead, the
        // shadow damage then drives the gl readback pump and the cpu
        // bits stay behind as the mi starting pixmap nobody reads
        PixmapPtr tex = pScreen->CreatePixmap(pScreen, pScreen->width,
                                              pScreen->height,
                                              pScreen->rootDepth,
                                              CREATE_PIXMAP_USAGE_BACKING_PIXMAP);
        if (!tex) {
            return FALSE;
        }
        pScreen->SetScreenPixmap(tex);
        // the scanout pair are fresh linear dumb allocations, the
        // server flips between them on the vblank; the live texture
        // export is unreliable, the drivers snapshot or tile it
        if (!x12_swapchain_rebuild(pScreen, pScreen->width,
                                   pScreen->height)) {
            ErrorF("Xx12: dmabuf target refused, the cpu pump carries the "
                   "screen\n");
        }
    }
    // the screen pixmap exists only after dix ran its resource hook,
    // the shadow layer tracks damage on that pixmap from here on
    if (!shadowAdd(pScreen, pScreen->GetScreenPixmap(pScreen),
                   x12ShadowUpdate, NULL, 0, NULL)) {
        return FALSE;
    }
    return ret;
}

static Bool
x12ScreenInit(ScreenPtr pScreen, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    int dpix = monitorResolution ? monitorResolution : 100;
    int bytes_pad = PixmapBytePad(x12_width, 24);
    fb_padded_width = bytes_pad / 4;
    fb_bits = calloc((size_t)bytes_pad * x12_height, 1);
    if (!fb_bits) {
        return FALSE;
    }

    miSetVisualTypesAndMasks(24, (1 << TrueColor), 8, TrueColor,
                             0xff0000, 0x00ff00, 0x0000ff);
    miSetPixmapDepths();

    Bool ret = fbScreenInit(pScreen, fb_bits, x12_width, x12_height,
                            dpix, dpix, fb_padded_width, 32);
    if (!ret) {
        return FALSE;
    }
    fbPictureInit(pScreen, 0, 0);

    // the shadow private key and the screen damage machinery must be
    // alive before shadowAdd runs in the resource hook
    //
    // the order against glamor is load bearing: damage wraps whatever
    // create gc the screen carries at DamageSetup time, so the egl
    // engine must take over the screen first, a glamor install after
    // shadowSetup bypasses every damage report and the frame never
    // pumps
    x12_glamor_init(pScreen);
    if (!shadowSetup(pScreen)) {
        return FALSE;
    }

    // the dri3 hooks ride on the glamor context, clients render on the
    // gpu as soon as the extension answers an open
    x12_dri3_init(pScreen);

    // pixel reads ride the gl readback while the direct scanout holds
    x12_saved_get_image = pScreen->GetImage;
    pScreen->GetImage = x12GetImage;

    // randr carries the session resize, both the xrandr client path
    // and the mode driven path land in the same hook
    if (RRScreenInit(pScreen)) {
        rrScrPriv(pScreen);
        uint32_t max_w, max_h;
        x12_bridge_max_size(&max_w, &max_h);
        pScrPriv->rrScreenSetSize = x12RRScreenSetSize;
        pScrPriv->rrGetInfo = x12RRGetInfo;
        pScrPriv->minWidth = 1;
        pScrPriv->minHeight = 1;
        pScrPriv->maxWidth = (CARD16)max_w;
        pScrPriv->maxHeight = (CARD16)max_h;
        if (!x12RandRInit(pScreen)) {
            return FALSE;
        }
        x12PresentInit(pScreen);
    }

    miDCInitialize(pScreen, &x12PointerCursorFuncs);

    if (x12_bridge_output_setup((uint32_t)x12_width,
                                (uint32_t)x12_height, NULL, NULL) != 0) {
        return FALSE;
    }
    RegionInit(&pending_damage, NullBox, 1);
    pending_ready = 1;
    SetNotifyFd(x12_bridge_fd(), x12NotifyFd, X_NOTIFY_READ, NULL);
    RegisterBlockAndWakeupHandlers(x12BlockHandler, x12WakeupHandler,
                                   NULL);
    x12_saved_create_res = pScreen->CreateScreenResources;
    pScreen->CreateScreenResources = x12CreateScreenResources;
    bridge_screen = pScreen;

    pScreen->blackPixel = 0;
    pScreen->whitePixel = 0xffffff;
    ret = fbCreateDefColormap(pScreen);
    miSetZeroLineBias(pScreen, 0);

    x12_saved_close = pScreen->CloseScreen;
    pScreen->CloseScreen = x12CloseScreen;
    return ret;
}

void
InitOutput(ScreenInfo *screen_info, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    // the bridge session must exist before any X client can talk to
    // us, a missing X12 server is a fatal misconfiguration, not a
    // degraded mode
    const char *sock = getenv("X12_BRIDGE_SOCKET");
    if (!sock || !*sock) {
        FatalError("Xx12: X12_BRIDGE_SOCKET is not set");
    }
    if (!x12_bridge_start(sock)) {
        FatalError("Xx12: the X12 server at %s refused the handshake",
                   sock);
    }

    const char *w = getenv("X12_BRIDGE_WIDTH");
    const char *h = getenv("X12_BRIDGE_HEIGHT");
    uint32_t pw, ph;
    if (w && h) {
        x12_width = atoi(w);
        x12_height = atoi(h);
        geometry_fixed = 1;
    } else if (x12_bridge_preferred_size(&pw, &ph)) {
        // no explicit geometry, a rootful session fills the output it
        // learned from the mode event
        x12_width = (int)pw;
        x12_height = (int)ph;
    } else {
        x12_width = X12_DEFAULT_W;
        x12_height = X12_DEFAULT_H;
    }
    if (x12_width <= 0 || x12_height <= 0 ||
        x12_width > 8192 || x12_height > 8192) {
        x12_width = X12_DEFAULT_W;
        x12_height = X12_DEFAULT_H;
    }

    // depth 1 is not optional: dix mints the default stipple as a
    // depth one pixmap, a missing format makes fbValidateGC spin on a
    // zero shift pad forever
    // depth 1 is not optional: dix mints the default stipple as a
    // depth one pixmap, a missing format makes fbValidateGC spin on a
    // zero shift pad forever. the render depths (4, 8, 16) are the
    // cairo alpha masks and 16-bit colors, clients like i3 need them
    // for their text masks, a missing one turns into BadValue on
    // CreatePixmap and tears whole client toolkits down
    int n_formats = 0;
    static const struct {
        int depth;
        int bpp;
    } fmts[] = {
        { 1, 1 }, { 4, 8 }, { 8, 8 }, { 16, 16 }, { 24, 32 }, { 32, 32 },
    };
    for (size_t k = 0; k < sizeof fmts / sizeof *fmts; k++) {
        screen_info->formats[n_formats].depth = fmts[k].depth;
        screen_info->formats[n_formats].bitsPerPixel = fmts[k].bpp;
        screen_info->formats[n_formats].scanlinePad = BITMAP_SCANLINE_PAD;
        n_formats++;
    }

    screen_info->imageByteOrder = IMAGE_BYTE_ORDER;
    screen_info->bitmapScanlineUnit = BITMAP_SCANLINE_UNIT;
    screen_info->bitmapScanlinePad = BITMAP_SCANLINE_PAD;
    screen_info->bitmapBitOrder = BITMAP_BIT_ORDER;
    screen_info->numPixmapFormats = n_formats;

    if (-1 == AddScreen(x12ScreenInit, argc, argv)) {
        FatalError("Xx12: couldn't add the screen");
    }
    // the glvnd glx vendor registers through this callback, no call
    // here means the extension comes up with zero fbconfigs and every
    // glx client dies on "no RGB visual"
    xorgGlxCreateVendor();
    printf("X12-BRIDGE: screen ready\n");
    fflush(stdout);
}

void
ddxGiveUp(enum ExitCode error)
{
    (void)error;
}

void
OsVendorInit(void)
{
}

void
OsVendorFatalError(const char *f, va_list args)
{
    (void)f;
    (void)args;
}

void
ddxUseMsg(void)
{
}

void
ddxInputThreadInit(void)
{
    // armed before dix knows our devices, stays empty until the input
    // wave routes seat events through the thread
}

int
ddxProcessArgument(int argc, char *argv[], int i)
{
    (void)argc;
    (void)argv;
    (void)i;
    // everything is env driven for now, the os layer owns the standard
    // display and auth arguments
    return 0;
}

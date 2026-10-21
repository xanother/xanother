// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// the glamor side of the bridge ddx, declared apart from x12-init.c so
// the fb engine build stays free of egl includes

#ifndef X12_GLAMOR_H
#define X12_GLAMOR_H

#include <regionstr.h>
#include <screenint.h>

extern int x12_glamor_active;

int x12_glamor_init(ScreenPtr pScreen);
// the primary node path of the card the egl display rides, NULL when
// the surfaceless fallback carries the display; the dri3 probe
// matches render nodes against it
const char *x12_glamor_card(void);
int x12_glamor_pump(PixmapPtr pix, RegionPtr rgn, unsigned char *pool);
void x12_glamor_close(void);
void x12_glamor_make_current(void);

#endif
// dmabuf export of the screen backing: the fd belongs to the caller
// unless the export failed, then nothing was opened
int x12_glamor_export_screen(ScreenPtr pScreen, int *fd, uint32_t *stride,
                             uint64_t *modifier, uint32_t *w, uint32_t *h);
// gl pipeline flush before a scanout handoff
void x12_glamor_finish(void);
// arbitrary pixmap export for the dri3 glue
int x12_glamor_export_pixmap(PixmapPtr pixmap, int *fd, uint32_t *stride,
                             uint64_t *modifier, uint32_t *w, uint32_t *h);
// the scanout target of the pump: a linear dumb allocation on the
// render node imported as a texture, the pump blits the screen into it
// flipped; the dmabuf fd travels to the server, the target dies with
// the destroy call
int x12_glamor_target_create(ScreenPtr pScreen, uint32_t w, uint32_t h,
                             int *fd, uint32_t *stride);
void x12_glamor_target_destroy(void);
void x12_glamor_target_blit(ScreenPtr pScreen, RegionPtr rgn);
// the blit destination hops to the buffer the server stopped scanning
void x12_glamor_target_flip(void);
int x12_glamor_target_current(void);
// cpu readback of a glamor texture pixmap, dix top down bgrx rows
int x12_glamor_read_texture(PixmapPtr pixmap, int x, int y, int w, int h,
                            unsigned char *out);


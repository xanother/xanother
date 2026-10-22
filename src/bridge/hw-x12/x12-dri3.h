// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#ifndef X12_DRI3_H
#define X12_DRI3_H

#include "screenint.h"

int x12_dri3_init(ScreenPtr pScreen);
// the render node sharing the screen texture memory, -1 when unknown
int x12_dri3_render_node(void);

#endif

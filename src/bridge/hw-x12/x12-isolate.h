// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// the x11 window isolation of the bridge, see x12-isolate.c

#ifndef X12_BRIDGE_ISOLATE_H
#define X12_BRIDGE_ISOLATE_H

// installs the access hooks and request filters that keep every x
// client inside its own window tree, call once from ddx init before
// the first client connects
void x12_isolate_init(void);

#endif

// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#ifndef X12_LIMITS_H
#define X12_LIMITS_H

// named limits, every one of them refuses loudly instead of growing
#define X12_MAX_CLIENTS 8
#define X12_MAX_OBJECTS 32
#define X12_MAX_FDS 8
#define X12_MAX_ARGS 16
#define X12_IO_BUF 65536

// well known ids for the singletons the server creates on connect, client
// allocated ids start elsewhere and may not touch these
#define X12_OBJ_ID_CONNECTION 1u
#define X12_OBJ_ID_REGISTRY 2u

#endif

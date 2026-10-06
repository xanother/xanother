// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#ifndef X12_OUTPUT_H
#define X12_OUTPUT_H

#include <stdint.h>

#include "conn.h"

// the output global: one screen description and its presentation
// feedback. every client that binds the global hears every present, the
// commit token tells it which of its own frames reached the screen

// startup description of the one screen the server drives
void x12_output_info(uint32_t w, uint32_t h, uint32_t refresh_millihz,
                     const char *name);

void x12_output_describe(uint32_t *w, uint32_t *h, uint32_t *refresh_millihz,
                         const char **name);

// the registry bind and the teardown path
void x12_output_bind(struct x12_conn *c, uint32_t id);
void x12_output_unbind(struct x12_conn *c);

// presentation feedback: the token names the commit that reached the
// screen, kind is the x12_output present_kind wire value and ns is the
// scanout moment the display picked the frame up
void x12_output_present(uint64_t token, uint32_t kind, uint64_t ns);

#endif

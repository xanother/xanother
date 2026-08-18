// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#ifndef X12_WIRE_H
#define X12_WIRE_H

#include <stddef.h>
#include <stdint.h>

#define X12_HEADER_SIZE 12u
#define X12_TIMESTAMP_SIZE 8u

// arg types as emitted into generated tables
enum x12_arg_type {
    X12_ARG_UINT32,
    X12_ARG_INT32,
    X12_ARG_UINT64,
    X12_ARG_INT64,
    X12_ARG_FIXED24P8,
    X12_ARG_STRING,
    X12_ARG_FD,
    X12_ARG_OBJECT,
    X12_ARG_NEW_ID,
    X12_ARG_ARRAY_U32,
    X12_ARG_ENUM,
};

enum x12_msg_kind {
    X12_REQUEST = 0,
    X12_EVENT = 1,
};

// enum_id indexes the x12_enums table, 255 means the arg carries no enum
typedef struct {
    const char *name;
    uint8_t type;
    uint8_t enum_id;
} x12_arg_desc;

typedef struct {
    const char *name;
    uint16_t opcode;
    uint8_t kind;
    uint8_t arg_count;
    const x12_arg_desc *args;
} x12_msg_desc;

typedef struct {
    const char *name;
    uint16_t version;
    uint16_t msg_count;
    const x12_msg_desc *msgs;
} x12_iface_desc;

typedef struct {
    const char *name;
    uint8_t symbol_count;
    const char *const *symbols;
} x12_enum_desc;

// generated tables, see x12gen
extern const x12_enum_desc x12_enums[];
extern const size_t x12_enums_count;
extern const x12_iface_desc x12_interfaces[];
extern const size_t x12_interfaces_count;

const x12_iface_desc *x12_iface_find(const char *name);
const x12_msg_desc *x12_msg_find(const x12_iface_desc *iface, uint8_t kind, uint16_t opcode);
// name lookup for handlers that name their message the way the idl does
const x12_msg_desc *x12_msg_find_name(const x12_iface_desc *iface, uint8_t kind, const char *name);

// header fields of a frame the caller already located by size, cheap peek
// for dispatch before the full decode
void x12_header_peek(const uint8_t *buf, uint32_t *object_out, uint16_t *opcode_out);

// a decoded or to be encoded argument, the active field follows the table
// arg type: u64 for unsigned numerics, enums, object ids and fd slots,
// i64 for signed numerics and fixed24p8 raw values, str and array point
// into the frame buffer on decode and stay caller owned on encode
typedef struct {
    uint64_t u64;
    int64_t i64;
    const char *str;
    uint32_t str_len;
    const uint8_t *array;
    uint32_t array_len;
} x12_arg;

enum x12_rc {
    X12_OK = 0,
    X12_ERR_CAP = -1,    // output buffer too small
    X12_ERR_ARG = -2,    // argument missing, out of enum range or holds a nul byte
    X12_ERR_FRAME = -3,  // size mismatch, nonzero reserved, wrong opcode, truncation, bad padding
};

// encodes one frame, events carry the implicit leading timestamp, requests
// ignore it. args must hold exactly msg->arg_count entries in table order
int x12_encode(const x12_iface_desc *iface, const x12_msg_desc *msg,
               uint32_t object, uint64_t timestamp_ns,
               const x12_arg *args, size_t nargs,
               uint8_t *buf, size_t cap, size_t *out_len);

// decodes one frame previously located by opcode, every read is bounds
// checked and every byte is accounted for, trailing data is rejected
int x12_decode(const x12_iface_desc *iface, const x12_msg_desc *msg,
               const uint8_t *buf, size_t len,
               uint32_t *object_out, uint64_t *timestamp_ns,
               x12_arg *args_out, size_t max_args);

#endif

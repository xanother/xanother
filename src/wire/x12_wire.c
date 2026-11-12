// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#include "x12_wire.h"

// the engine is a fixed interpreter over generated tables, protocol growth
// never touches this file. every read is bounds checked before it happens
// and every accepted frame is canonical, decode followed by encode must
// reproduce the input byte for byte

#include <string.h>

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd_u64(const uint8_t *p) {
    return (uint64_t)rd_u32(p) | ((uint64_t)rd_u32(p + 4) << 32);
}

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void wr_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wr_u64(uint8_t *p, uint64_t v) {
    wr_u32(p, (uint32_t)v);
    wr_u32(p + 4, (uint32_t)(v >> 32));
}

static void wr_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static size_t align4(size_t n) {
    return (n + 3u) & ~(size_t)3u;
}

// strict utf8: no overlongs, no surrogates, nothing beyond the unicode
// range. python reference model decodes utf8 strictly, so the engine must
// accept exactly the same set or the two codecs would disagree
static int utf8_valid(const char *s, uint32_t len) {
    uint32_t i = 0;
    while (i < len) {
        uint8_t c = (uint8_t)s[i];
        uint8_t cont;
        uint32_t cp;
        if (c < 0x80) {
            i++;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            cont = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            cont = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            cont = 3;
            cp = c & 0x07;
        } else {
            return 0;
        }
        if (i + cont >= len) {
            return 0;
        }
        for (uint8_t k = 1; k <= cont; k++) {
            uint8_t cc = (uint8_t)s[i + k];
            if ((cc & 0xC0) != 0x80) {
                return 0;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((cont == 1 && cp < 0x80) || (cont == 2 && cp < 0x800) ||
            (cont == 3 && cp < 0x10000)) {
            return 0;
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return 0;
        }
        if (cp > 0x10FFFF) {
            return 0;
        }
        i += cont + 1;
    }
    return 1;
}

const x12_iface_desc *x12_iface_find(const char *name) {
    for (size_t i = 0; i < x12_interfaces_count; i++) {
        if (strcmp(x12_interfaces[i].name, name) == 0) {
            return &x12_interfaces[i];
        }
    }
    return 0;
}

const x12_msg_desc *x12_msg_find(const x12_iface_desc *iface, uint8_t kind, uint16_t opcode) {
    if (!iface) {
        return 0;
    }
    for (uint16_t i = 0; i < iface->msg_count; i++) {
        const x12_msg_desc *m = &iface->msgs[i];
        if (m->kind == kind && m->opcode == opcode) {
            return m;
        }
    }
    return 0;
}

const x12_msg_desc *x12_msg_find_name(const x12_iface_desc *iface, uint8_t kind, const char *name) {
    if (!iface) {
        return 0;
    }
    for (uint16_t i = 0; i < iface->msg_count; i++) {
        const x12_msg_desc *m = &iface->msgs[i];
        if (m->kind == kind && strcmp(m->name, name) == 0) {
            return m;
        }
    }
    return 0;
}

void x12_header_peek(const uint8_t *buf, uint32_t *object_out, uint16_t *opcode_out) {
    *object_out = rd_u32(buf + 4);
    *opcode_out = rd_u16(buf + 8);
}

// validates one argument and returns its payload size, validation happens
// here so the write pass can stay blind to value errors
static int arg_size(const x12_arg_desc *ad, const x12_arg *a, size_t *out) {
    switch (ad->type) {
    case X12_ARG_UINT32:
    case X12_ARG_INT32:
    case X12_ARG_FIXED24P8:
    case X12_ARG_ENUM:
    case X12_ARG_FD:
    case X12_ARG_OBJECT:
    case X12_ARG_NEW_ID:
        if (ad->type == X12_ARG_ENUM && a->u64 >= x12_enums[ad->enum_id].symbol_count) {
            return X12_ERR_ARG;
        }
        *out = 4;
        return X12_OK;
    case X12_ARG_UINT64:
    case X12_ARG_INT64:
        *out = 8;
        return X12_OK;
    case X12_ARG_STRING:
        if (!a->str) {
            return X12_ERR_ARG;
        }
        // interior nul bytes are rejected, the length prefix is the exact
        // utf8 size so a nul would smuggle a shorter string past consumers
        for (uint32_t i = 0; i < a->str_len; i++) {
            if (a->str[i] == '\0') {
                return X12_ERR_ARG;
            }
        }
        if (!utf8_valid(a->str, a->str_len)) {
            return X12_ERR_ARG;
        }
        // fall through
    case X12_ARG_BYTES:
        if (!a->str) {
            return X12_ERR_ARG;
        }
        *out = 4 + align4(a->str_len);
        return X12_OK;
    case X12_ARG_ARRAY_U32:
        if (a->array_len && !a->array) {
            return X12_ERR_ARG;
        }
        *out = 4 + 4u * (size_t)a->array_len;
        return X12_OK;
    default:
        return X12_ERR_ARG;
    }
}

static size_t arg_write(const x12_arg_desc *ad, const x12_arg *a, uint8_t *p) {
    switch (ad->type) {
    case X12_ARG_UINT32:
    case X12_ARG_ENUM:
    case X12_ARG_FD:
    case X12_ARG_OBJECT:
    case X12_ARG_NEW_ID:
        wr_u32(p, (uint32_t)a->u64);
        return 4;
    case X12_ARG_INT32:
    case X12_ARG_FIXED24P8:
        wr_u32(p, (uint32_t)(int32_t)a->i64);
        return 4;
    case X12_ARG_UINT64:
        wr_u64(p, a->u64);
        return 8;
    case X12_ARG_INT64:
        wr_u64(p, (uint64_t)a->i64);
        return 8;
    case X12_ARG_STRING:
    case X12_ARG_BYTES: {
        wr_u32(p, a->str_len);
        memcpy(p + 4, a->str, a->str_len);
        memset(p + 4 + a->str_len, 0, align4(a->str_len) - a->str_len);
        return 4 + align4(a->str_len);
    }
    case X12_ARG_ARRAY_U32: {
        wr_u32(p, a->array_len);
        for (uint32_t i = 0; i < a->array_len; i++) {
            wr_u32(p + 4 + 4u * i, rd_u32(a->array + 4u * i));
        }
        return 4 + 4u * (size_t)a->array_len;
    }
    default:
        return 0;
    }
}

int x12_encode(const x12_iface_desc *iface, const x12_msg_desc *msg,
               uint32_t object, uint64_t timestamp_ns,
               const x12_arg *args, size_t nargs,
               uint8_t *buf, size_t cap, size_t *out_len) {
    (void)iface;
    if (nargs != msg->arg_count) {
        return X12_ERR_ARG;
    }
    size_t payload = msg->kind == X12_EVENT ? X12_TIMESTAMP_SIZE : 0;
    for (size_t i = 0; i < nargs; i++) {
        size_t sz = 0;
        int rc = arg_size(&msg->args[i], &args[i], &sz);
        if (rc != X12_OK) {
            return rc;
        }
        payload += sz;
    }
    size_t total = X12_HEADER_SIZE + payload;
    if (total > cap) {
        return X12_ERR_CAP;
    }
    wr_u32(buf, (uint32_t)total);
    wr_u32(buf + 4, object);
    wr_u16(buf + 8, msg->opcode);
    wr_u16(buf + 10, 0);
    uint8_t *p = buf + X12_HEADER_SIZE;
    if (msg->kind == X12_EVENT) {
        wr_u64(p, timestamp_ns);
        p += X12_TIMESTAMP_SIZE;
    }
    for (size_t i = 0; i < nargs; i++) {
        p += arg_write(&msg->args[i], &args[i], p);
    }
    *out_len = total;
    return X12_OK;
}

// bounds helper, off never exceeds len so the sum cannot overflow
static int need(const uint8_t **off, size_t *left, size_t n) {
    if (*left < n) {
        return X12_ERR_FRAME;
    }
    *off += n;
    *left -= n;
    return X12_OK;
}

int x12_decode(const x12_iface_desc *iface, const x12_msg_desc *msg,
               const uint8_t *buf, size_t len,
               uint32_t *object_out, uint64_t *timestamp_ns,
               x12_arg *args_out, size_t max_args) {
    (void)iface;
    if (len < X12_HEADER_SIZE) {
        return X12_ERR_FRAME;
    }
    uint32_t size = rd_u32(buf);
    uint32_t object = rd_u32(buf + 4);
    uint16_t opcode = rd_u16(buf + 8);
    uint16_t reserved = rd_u16(buf + 10);
    if (size != len || reserved != 0 || opcode != msg->opcode) {
        return X12_ERR_FRAME;
    }
    const uint8_t *off = buf + X12_HEADER_SIZE;
    size_t left = len - X12_HEADER_SIZE;
    uint64_t ts = 0;
    if (msg->kind == X12_EVENT) {
        int rc = need(&off, &left, X12_TIMESTAMP_SIZE);
        if (rc != X12_OK) {
            return rc;
        }
        ts = rd_u64(off - X12_TIMESTAMP_SIZE);
    }
    if (msg->arg_count > max_args) {
        return X12_ERR_FRAME;
    }
    for (uint8_t i = 0; i < msg->arg_count; i++) {
        const x12_arg_desc *ad = &msg->args[i];
        x12_arg *a = &args_out[i];
        memset(a, 0, sizeof(*a));
        switch (ad->type) {
        case X12_ARG_UINT32:
        case X12_ARG_FD:
        case X12_ARG_OBJECT:
        case X12_ARG_NEW_ID: {
            int rc = need(&off, &left, 4);
            if (rc != X12_OK) {
                return rc;
            }
            a->u64 = rd_u32(off - 4);
            break;
        }
        case X12_ARG_ENUM: {
            int rc = need(&off, &left, 4);
            if (rc != X12_OK) {
                return rc;
            }
            uint32_t v = rd_u32(off - 4);
            // enum values outside the declared range are wire garbage,
            // rejecting keeps every decoded symbol meaningful
            if (v >= x12_enums[ad->enum_id].symbol_count) {
                return X12_ERR_FRAME;
            }
            a->u64 = v;
            break;
        }
        case X12_ARG_INT32:
        case X12_ARG_FIXED24P8: {
            int rc = need(&off, &left, 4);
            if (rc != X12_OK) {
                return rc;
            }
            uint32_t raw = rd_u32(off - 4);
            a->u64 = raw;
            a->i64 = (int32_t)raw;
            break;
        }
        case X12_ARG_UINT64: {
            int rc = need(&off, &left, 8);
            if (rc != X12_OK) {
                return rc;
            }
            a->u64 = rd_u64(off - 8);
            break;
        }
        case X12_ARG_INT64: {
            int rc = need(&off, &left, 8);
            if (rc != X12_OK) {
                return rc;
            }
            a->i64 = (int64_t)rd_u64(off - 8);
            break;
        }
        case X12_ARG_STRING:
        case X12_ARG_BYTES: {
            int rc = need(&off, &left, 4);
            if (rc != X12_OK) {
                return rc;
            }
            uint32_t slen = rd_u32(off - 4);
            size_t pad = align4(slen) - slen;
            rc = need(&off, &left, slen);
            if (rc != X12_OK) {
                return rc;
            }
            const char *s = (const char *)(off - slen);
            if (ad->type == X12_ARG_STRING) {
                // raw bytes take anything, strings stay printable text
                for (uint32_t k = 0; k < slen; k++) {
                    if (s[k] == '\0') {
                        return X12_ERR_FRAME;
                    }
                }
                if (!utf8_valid(s, slen)) {
                    return X12_ERR_FRAME;
                }
            }
            // padding sits right after the payload, checked before consuming
            for (size_t k = 0; k < pad; k++) {
                if (off[k] != 0) {
                    return X12_ERR_FRAME;
                }
            }
            rc = need(&off, &left, pad);
            if (rc != X12_OK) {
                return rc;
            }
            a->str = s;
            a->str_len = slen;
            break;
        }
        case X12_ARG_ARRAY_U32: {
            int rc = need(&off, &left, 4);
            if (rc != X12_OK) {
                return rc;
            }
            uint32_t count = rd_u32(off - 4);
            if (count > left / 4) {
                return X12_ERR_FRAME;
            }
            rc = need(&off, &left, 4u * (size_t)count);
            if (rc != X12_OK) {
                return rc;
            }
            a->array = off - 4u * (size_t)count;
            a->array_len = count;
            break;
        }
        default:
            return X12_ERR_FRAME;
        }
    }
    if (left != 0) {
        return X12_ERR_FRAME;
    }
    *object_out = object;
    *timestamp_ns = ts;
    return X12_OK;
}

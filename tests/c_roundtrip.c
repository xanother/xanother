// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// c roundtrip runner, consumes golden vectors produced by gen_vectors.py.
// for every vector the engine must decode the frame, re-encode it and match
// the golden bytes exactly. failures exit nonzero, this binary is the phase
// gate for the generated codec tables

#define _POSIX_C_SOURCE 200809L

#include "x12_wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t hex_parse(const char *hex, uint8_t *out, size_t cap) {
    size_t n = strlen(hex) / 2;
    if (n > cap) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        int hi = hexval(hex[2 * i]);
        int lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return 0;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

static const x12_msg_desc *msg_by_name(const x12_iface_desc *iface, uint8_t kind, const char *name) {
    for (uint16_t i = 0; i < iface->msg_count; i++) {
        const x12_msg_desc *m = &iface->msgs[i];
        if (m->kind == kind && strcmp(m->name, name) == 0) {
            return m;
        }
    }
    return 0;
}

static int roundtrip(const uint8_t *golden, size_t len,
                     const x12_iface_desc *iface, const x12_msg_desc *msg) {
    x12_arg args[16];
    uint64_t ts = 0;
    uint32_t object = 0;
    int rc = x12_decode(iface, msg, golden, len, &object, &ts, args, 16);
    if (rc != X12_OK) {
        printf("decode failed rc=%d on %s.%s\n", rc, iface->name, msg->name);
        return 0;
    }
    uint8_t out[8192];
    size_t out_len = 0;
    rc = x12_encode(iface, msg, object, ts, args, msg->arg_count, out, sizeof out, &out_len);
    if (rc != X12_OK) {
        printf("encode failed rc=%d on %s.%s\n", rc, iface->name, msg->name);
        return 0;
    }
    if (out_len != len || memcmp(out, golden, len) != 0) {
        printf("canonicality mismatch on %s.%s (%zu vs %zu bytes)\n",
               iface->name, msg->name, out_len, len);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "build/gen/vectors.hex";
    FILE *f = fopen(path, "r");
    if (!f) {
        perror("vectors");
        return 1;
    }
    char line[8192];
    size_t total = 0, ok = 0;
    // negatives run on every vector, a rejected corruption is a pass
    size_t negatives = 0, negatives_ok = 0;
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *save = NULL;
        char *iface_name = strtok_r(line, "\t", &save);
        char *msg_name = strtok_r(NULL, "\t", &save);
        char *kind_str = strtok_r(NULL, "\t", &save);
        char *hex = strtok_r(NULL, "\t", &save);
        if (!iface_name || !msg_name || !kind_str || !hex) {
            printf("bad vector line\n");
            return 1;
        }
        const x12_iface_desc *iface = x12_iface_find(iface_name);
        if (!iface) {
            printf("unknown interface %s\n", iface_name);
            return 1;
        }
        uint8_t kind = strcmp(kind_str, "event") == 0 ? (uint8_t)X12_EVENT : (uint8_t)X12_REQUEST;
        const x12_msg_desc *msg = msg_by_name(iface, kind, msg_name);
        if (!msg) {
            printf("unknown message %s.%s\n", iface_name, msg_name);
            return 1;
        }
        uint8_t golden[8192];
        size_t len = hex_parse(hex, golden, sizeof golden);
        if (len < X12_HEADER_SIZE) {
            printf("bad hex for %s.%s\n", iface_name, msg_name);
            return 1;
        }
        total++;
        if (roundtrip(golden, len, iface, msg)) {
            ok++;
        }
        // truncated frame must be rejected
        if (x12_decode(iface, msg, golden, len - 1, &(uint32_t){0}, &(uint64_t){0}, NULL, 0) == X12_ERR_FRAME) {
            negatives_ok++;
        }
        negatives++;
        // nonzero reserved must be rejected
        uint8_t bad[8192];
        memcpy(bad, golden, len);
        bad[11] = 1;
        x12_arg args[16];
        if (x12_decode(iface, msg, bad, len, &(uint32_t){0}, &(uint64_t){0}, args, 16) == X12_ERR_FRAME) {
            negatives_ok++;
        }
        negatives++;
    }
    fclose(f);
    printf("c roundtrip: %zu/%zu vectors, negatives %zu/%zu\n",
           ok, total, negatives_ok, negatives);
    return (ok == total && negatives_ok == negatives) ? 0 : 1;
}

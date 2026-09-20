// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// the c welcome client on libx12: maps a gradient window and prints the
// seat event stream. --once exits right after the map so e2e runs can
// assert the painted pixels without babysitting the process


#include "libx12.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define XR24 0x34325258u

static void print_args(const x12_msg_desc *m, const x12_arg *args,
                       size_t nargs) {
    for (size_t i = 0; i < nargs; i++) {
        const x12_arg_desc *ad = &m->args[i];
        if (ad->type == X12_ARG_FIXED24P8) {
            printf(" %s=%lld", ad->name, (long long)(args[i].i64 / 256));
        } else if (ad->type == X12_ARG_ENUM) {
            const x12_enum_desc *e = &x12_enums[ad->enum_id];
            printf(" %s=%s", ad->name,
                   args[i].u64 < e->symbol_count ? e->symbols[args[i].u64]
                                                 : "?");
        } else if (ad->type == X12_ARG_ARRAY_U32) {
            printf(" %s=[%u]", ad->name, (unsigned)args[i].array_len);
        } else if (ad->type == X12_ARG_FD) {
            printf(" %s=fd%llu", ad->name, (unsigned long long)args[i].u64);
        } else {
            printf(" %s=%llu", ad->name, (unsigned long long)args[i].u64);
        }
    }
}

static void on_event(x12c *c, const x12c_proxy *p, const x12_msg_desc *desc,
                     const x12_arg *args, size_t nargs) {
    (void)c;
    if (strcmp(desc->name, "keymap") == 0) {
        // the lib remapped the wire slot into a real fd, one peek is
        // enough to prove the keymap text arrived
        char magic[12] = { 0 };
        int fd = (int)args[1].u64;
        if (fd >= 0) {
            ssize_t got = read(fd, magic, sizeof magic - 1);
            if (got > 0) {
                printf("WELCOME-EVENT keymap size=%u magic=%s\n",
                       (unsigned)args[2].u64, magic);
                fflush(stdout);
            }
        }
        return;
    }
    if (strcmp(p->iface->name, "x12_pointer") == 0 ||
        strcmp(p->iface->name, "x12_keyboard") == 0) {
        printf("WELCOME-EVENT %s %s", p->iface->name, desc->name);
        print_args(desc, args, nargs);
        printf("\n");
        fflush(stdout);
        return;
    }
    if (strcmp(desc->name, "error") == 0) {
        printf("WELCOME-EVENT SERVER-ERROR");
        print_args(desc, args, nargs);
        printf("\n");
        fflush(stdout);
    }
}

static void paint(uint32_t *pix, uint32_t w, uint32_t h) {
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t ring = x < y ? x : y;
            uint32_t r2 = w - 1u - x < h - 1u - y ? w - 1u - x : h - 1u - y;
            if (r2 < ring) {
                ring = r2;
            }
            uint32_t v;
            if (ring < 3) {
                v = 0xFFE6E6EBu;
            } else {
                uint32_t r = 40u + (x - 3u) * 180u / (w - 7u);
                uint32_t g = 40u + (y - 3u) * 180u / (h - 7u);
                v = 0xFF000000u | (r << 16) | (g << 8) | 200u;
            }
            pix[y * w + x] = v;
        }
    }
}

static int bound(x12c *c, uint32_t id) {
    return x12c_proxy_iface(c, id) != 0;
}

int main(int argc, char **argv) {
    int once = 0;
    const char *sock = 0;
    uint32_t w = 192, h = 128;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--once") == 0) {
            once = 1;
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            sscanf(argv[++i], "%ux%u", &w, &h);
        } else if (!sock) {
            sock = argv[i];
        }
    }
    if (!sock) {
        fprintf(stderr, "usage: welcome <socket> [--once] [--size WxH]\n");
        return 1;
    }
    x12c *c = x12c_connect(sock, on_event);
    if (!c) {
        fprintf(stderr, "welcome: cannot connect to %s, is the server up?\n",
                sock);
        return 1;
    }
    uint32_t comp = x12c_bind(c, "x12_compositor", 0);
    uint32_t bufman = x12c_bind(c, "x12_buffer_manager", 0);
    uint32_t seat = x12c_bind(c, "x12_seat", 0);
    if (!comp || !bufman || !seat) {
        fprintf(stderr, "welcome: proxy table exhausted\n");
        return 1;
    }
    while (!bound(c, comp) || !bound(c, bufman) || !bound(c, seat)) {
        if (x12c_dispatch(c, 2000) < 0) {
            fprintf(stderr, "welcome: server hung up before globals\n");
            return 1;
        }
    }

    // the seat device objects carry the input stream
    uint32_t ptr = x12c_reserve(c, "x12_pointer", 0);
    uint32_t kbd = x12c_reserve(c, "x12_keyboard", 0);
    x12_arg pa[1] = { { .u64 = ptr } };
    x12_arg ka[1] = { { .u64 = kbd } };
    if (!ptr || !kbd ||
        x12c_call(c, "x12_seat", "get_pointer", seat, pa, 1) != 0 ||
        x12c_call(c, "x12_seat", "get_keyboard", seat, ka, 1) != 0) {
        fprintf(stderr, "welcome: seat devices refused\n");
        return 1;
    }

    uint32_t surf = x12c_reserve(c, "x12_surface", 0);
    x12_arg cr[1] = { { .u64 = surf } };
    if (x12c_call(c, "x12_compositor", "create_surface", comp, cr, 1) != 0) {
        fprintf(stderr, "welcome: create_surface refused\n");
        return 1;
    }
    size_t pool_size = (size_t)w * h * 4;
    uint32_t pool;
    int memfd = x12c_pool_create(c, bufman, pool_size, &pool, 0);
    if (memfd < 0) {
        fprintf(stderr, "welcome: pool refused\n");
        return 1;
    }
    uint32_t *pix = mmap(0, pool_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                         memfd, 0);
    if (pix == MAP_FAILED) {
        fprintf(stderr, "welcome: pool mmap failed (errno=%s)\n",
                strerror(errno));
        return 1;
    }
    paint(pix, w, h);
    close(memfd);

    uint32_t buf;
    if (x12c_buffer_create(c, pool, 0, w, h, XR24, &buf, 0) != 0) {
        fprintf(stderr, "welcome: buffer refused\n");
        return 1;
    }
    x12_arg at[1] = { { .u64 = buf } };
    if (x12c_call(c, "x12_surface", "set_buffer", surf, at, 1) != 0 ||
        x12c_call(c, "x12_surface", "commit", surf, 0, 0) != 0 ||
        x12c_toplevel_map(c, comp, surf) < 0) {
        fprintf(stderr, "welcome: map handshake failed\n");
        return 1;
    }
    printf("WELCOME: mapped, event stream follows\n");
    fflush(stdout);
    if (once) {
        x12c_close(c);
        return 0;
    }
    // a hangup is the clean end of a session, the server closed the
    // window or died and there is nothing left to say
    for (;;) {
        if (x12c_dispatch(c, 5000) < 0) {
            return 0;
        }
    }
}

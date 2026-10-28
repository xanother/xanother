// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// headless backend, no host at all: the canvas geometry comes from the
// command line and every present is written out as a ppm dump. it also
// carries the test bench affordances: one to four heads tile the
// canvas and host commands drive mode changes and hotplug at runtime

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend.h"
#include "state.h"

// the per head mode, heads tile left to right on top of it
static uint32_t canvas_w = 640, canvas_h = 480;
static const char *ppm_path = "build/framebuffer.ppm";
static int heads = 1;
// hotplug state for the test bench, one head can go dark and come back
// on the same instance
static int head_live[4] = { 1, 1, 1, 1 };
// output events for the core poll, one slot per head is enough
static struct x12_output_event out_evq[8];
static int out_evq_n;

static void push_out_event(int kind, int out) {
    if (out_evq_n < (int)(sizeof out_evq / sizeof *out_evq)) {
        out_evq[out_evq_n].kind = kind;
        out_evq[out_evq_n].out = out;
        out_evq_n++;
    }
}

static const char *flag_value(int argc, char **argv, const char *flag) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], flag) == 0 && i + 1 < argc) {
            return argv[i + 1];
        }
    }
    return 0;
}

static int init(int argc, char **argv) {
    const char *w = flag_value(argc, argv, "--width");
    const char *h = flag_value(argc, argv, "--height");
    const char *p = flag_value(argc, argv, "--ppm");
    const char *n = flag_value(argc, argv, "--heads");
    if (w) {
        canvas_w = (uint32_t)strtoul(w, 0, 10);
    }
    if (h) {
        canvas_h = (uint32_t)strtoul(h, 0, 10);
    }
    if (p) {
        ppm_path = p;
    }
    if (n) {
        heads = (int)strtol(n, 0, 10);
    }
    if (canvas_w == 0 || canvas_h == 0 || canvas_w > 16384 ||
        canvas_h > 16384 || heads < 1 || heads > 4) {
        return -1;
    }
    return 0;
}

// the canvas is the union of the live heads, a hole does not shift the
// survivors and a fully dark canvas reports a degenerate size
static void get_size(uint32_t *w, uint32_t *h) {
    int first = -1, last = -1;
    for (int i = 0; i < heads; i++) {
        if (head_live[i]) {
            if (first < 0) {
                first = i;
            }
            last = i;
        }
    }
    if (first < 0) {
        *w = 0;
        *h = 0;
        return;
    }
    *w = (uint32_t)(last - first + 1) * canvas_w;
    *h = canvas_h;
}

static void present(const uint32_t *fb, const struct x12_rect *rects,
                    int n_rects) {
    // the ppm dump is a test artifact, not a scanout, it always carries
    // the whole frame so asserts stay independent of damage regions
    (void)rects;
    (void)n_rects;
    uint32_t fw, fh;
    get_size(&fw, &fh);
    if (fw == 0) {
        return;
    }
    FILE *f = fopen(ppm_path, "wb");
    if (!f) {
        return;
    }
    fprintf(f, "P6\n%u %u\n255\n", fw, fh);
    for (uint32_t y = 0; y < fh; y++) {
        for (uint32_t x = 0; x < fw; x++) {
            uint32_t v = fb[(size_t)y * fw + x];
            uint8_t rgb[3] = {
                (uint8_t)((v >> 16) & 255),
                (uint8_t)((v >> 8) & 255),
                (uint8_t)(v & 255),
            };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

static int output_count(void) {
    return heads;
}

// heads tile left to right, every head runs the same mode; a down head
// refuses to describe at all, its instance stays reserved for return
static int output_info(int out, struct x12_out_info *info) {
    static const char *names[4] = { "X12-1", "X12-2", "X12-3", "X12-4" };
    if (out < 0 || out >= heads || !head_live[out]) {
        return 0;
    }
    info->x = out * (int32_t)canvas_w;
    info->y = 0;
    info->width = canvas_w;
    info->height = canvas_h;
    info->mm_width = 0;
    info->mm_height = 0;
    info->refresh_millihz = 60000;
    info->name = names[out];
    info->live = 1;
    return 1;
}

static int output_vrr(int out, uint32_t *min_millihz, uint32_t *max_millihz) {
    (void)out;
    (void)min_millihz;
    (void)max_millihz;
    return 0;
}

static int set_vrr(int out, int enable) {
    (void)out;
    (void)enable;
    return -1;
}

static int output_poll(struct x12_output_event *ev) {
    if (out_evq_n == 0) {
        return 0;
    }
    *ev = out_evq[0];
    for (int i = 1; i < out_evq_n; i++) {
        out_evq[i - 1] = out_evq[i];
    }
    out_evq_n--;
    return 1;
}

// test bench: host lines the input stand did not consume. one line per
// wish: "resize W H" re-modes every live head, "heads N" grows or
// shrinks the ring, "head N up" and "head N down" hotplug one head
static int host_command(const char *line) {
    char word[16] = { 0 }, state[8] = { 0 };
    unsigned int w = 0, h = 0;
    int n = -1;
    if (sscanf(line, "%15s %u %u", word, &w, &h) == 3 &&
        strcmp(word, "resize") == 0 && w && h && w <= 16384 && h <= 16384) {
        canvas_w = w;
        canvas_h = h;
        for (int i = 0; i < heads; i++) {
            if (head_live[i]) {
                push_out_event(X12_OUTPUT_ADDED, i);
            }
        }
        return 1;
    }
    if (sscanf(line, "%15s %d", word, &n) == 2 &&
        strcmp(word, "heads") == 0 && n >= 1 && n <= 4) {
        for (int i = n; i < heads; i++) {
            if (head_live[i]) {
                head_live[i] = 0;
                push_out_event(X12_OUTPUT_REMOVED, i);
            }
        }
        for (int i = heads; i < n; i++) {
            head_live[i] = 1;
            push_out_event(X12_OUTPUT_ADDED, i);
        }
        heads = n;
        return 1;
    }
    if (sscanf(line, "%15s %d %7s", word, &n, state) == 3 &&
        strcmp(word, "head") == 0 && n >= 0 && n < 4) {
        if (strcmp(state, "up") == 0 && !head_live[n] && n < heads) {
            head_live[n] = 1;
            push_out_event(X12_OUTPUT_ADDED, n);
        } else if (strcmp(state, "down") == 0 && head_live[n] && n < heads) {
            head_live[n] = 0;
            push_out_event(X12_OUTPUT_REMOVED, n);
        }
        return 1;
    }
    return 0;
}

static void pump(void) {
}

static int halt_requested(void) {
    return 0;
}

static void shutdown(void) {
}

const struct x12_backend_ops x12_backend_headless = {
    .init = init,
    .get_size = get_size,
    .present = present,
    .output_count = output_count,
    .output_info = output_info,
    .output_vrr = output_vrr,
    .set_vrr = set_vrr,
    .output_poll = output_poll,
    .pump = pump,
    .host_command = host_command,
    .halt_requested = halt_requested,
    .shutdown = shutdown,
    .host_cursor = 0,
};

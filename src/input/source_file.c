// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#include "source.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "backend.h"
#include "input.h"

static int file_fd = -1;
static char file_path[256];
// a read may split lines anywhere, the tail waits here for the rest
static char acc[1024];
static size_t acc_len;

int x12_input_file_open(const char *path) {
    if (strlen(path) >= sizeof file_path) {
        return -1;
    }
    strcpy(file_path, path);
    file_fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    acc_len = 0;
    return file_fd < 0 ? -1 : 0;
}

int x12_input_file_fill(struct pollfd *pfds, int max) {
    if (file_fd < 0 || max < 1) {
        return 0;
    }
    pfds[0].fd = file_fd;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    return 1;
}

static void push_line(char *line) {
    int a = 0, b = 0;
    char w1[16], w2[16];
    struct x12_input_event e;
    memset(&e, 0, sizeof e);
    if (sscanf(line, "motion %d %d", &a, &b) == 2) {
        e.kind = IN_MOTION;
        e.x = a;
        e.y = b;
    } else if (sscanf(line, "button %d %15s", &a, w1) == 2) {
        e.kind = IN_BUTTON;
        e.code = (uint32_t)a;
        e.pressed = strcmp(w1, "press") == 0;
    } else if (sscanf(line, "key %d %15s", &a, w1) == 2) {
        e.kind = IN_KEY;
        e.code = (uint32_t)a;
        e.pressed = strcmp(w1, "press") == 0;
    } else if (sscanf(line, "axis %15s %d", w2, &a) == 2) {
        uint32_t axis = 0;
        const char *name = strcmp(w2, "horizontal") == 0
                               ? "horizontal-scroll"
                               : "vertical-scroll";
        if (x12_input_enum("axis_kind", name, &axis) != 0) {
            return;
        }
        e.kind = IN_AXIS;
        e.axis = axis;
        e.value = a;
    } else {
        // not an input line: the backend test bench gets its shot
        // (resize, heads, head up and down), one wrong line must not
        // poison the rest of the stream either way
        if (x12_backend->host_command) {
            x12_backend->host_command(line);
        }
        return;
    }
    x12_input_push(&e);
}

static void parse_buffer(void) {
    size_t start = 0;
    for (size_t i = 0; i < acc_len; i++) {
        if (acc[i] != '\n') {
            continue;
        }
        acc[i] = 0;
        if (start < i && acc[start] != '#') {
            push_line(acc + start);
        }
        start = i + 1;
    }
    acc_len -= start;
    memmove(acc, acc + start, acc_len);
}

void x12_input_file_pump(const struct pollfd *pfds, int count) {
    for (int i = 0; i < count; i++) {
        if (pfds[i].fd != file_fd || !(pfds[i].revents & (POLLIN | POLLHUP))) {
            continue;
        }
        while (acc_len < sizeof acc - 1) {
            ssize_t n = read(file_fd, acc + acc_len, sizeof acc - 1 - acc_len);
            if (n > 0) {
                acc_len += (size_t)n;
                continue;
            }
            if (n == 0) {
                // the writer went away, keep listening for the next one
                close(file_fd);
                file_fd = open(file_path,
                               O_RDONLY | O_NONBLOCK | O_CLOEXEC);
                acc_len = 0;
            }
            break;
        }
        acc[acc_len] = 0;
        parse_buffer();
    }
}

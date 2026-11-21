// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#include "buffer.h"

#include <fcntl.h>
#include <stdio.h>
#include <signal.h>
#include <setjmp.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdlib.h>

#include "x12_protocol.h"
#include "frame.h"

uint32_t x12_format_bpp(uint32_t format) {
    if (format == X12_FORMAT_XRGB8888 || format == X12_FORMAT_ARGB8888) {
        return 4;
    }
    return 0;
}

int x12_pool_create(struct x12_conn *c, uint32_t id, int fd, uint32_t size) {
    if (size == 0) {
        close(fd);
        return X12_ERROR_CODE_BAD_VALUE;
    }
    // the memory budget refuses new pools past the limit, the client
    // learns with out-of-memory instead of a silent swap death
    if (c->pool_bytes + size > X12_POOL_LIMIT) {
        close(fd);
        return X12_ERROR_CODE_OUT_OF_MEMORY;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || (uint64_t)st.st_size < size) {
        close(fd);
        return X12_ERROR_CODE_BAD_VALUE;
    }
    // a pool the client can still truncate is a crash waiting to
    // happen: shrink the file under the mapping and the compositor
    // takes a sigbus reading pixels. sealed memfds cannot do that,
    // everything else is refused
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) != 0) {
        close(fd);
        return X12_ERROR_CODE_BAD_VALUE;
    }
    // the size check and the seal are two reads, not one atomic step:
    // the client can truncate between them. under the seal the size can
    // never move again, so a second look here is authoritative
    struct stat sealed;
    if (fstat(fd, &sealed) != 0 || (uint64_t)sealed.st_size < size) {
        close(fd);
        return X12_ERROR_CODE_BAD_VALUE;
    }
    uint8_t *map = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        return X12_ERROR_CODE_OUT_OF_MEMORY;
    }
    if (x12_objects_alloc(&c->objects, id, OBJ_SHM_POOL,
                          x12_iface_find("x12_shm_pool")) != 0) {
        munmap(map, size);
        return X12_ERROR_CODE_INVALID_ID;
    }
    struct x12_object *o = x12_objects_get(&c->objects, id);
    o->as.pool.map = map;
    o->as.pool.size = size;
    c->pool_bytes += size;
    return 0;
}

int x12_pool_create_net(struct x12_conn *c, uint32_t id, uint32_t size) {
    if (size == 0) {
        return X12_ERROR_CODE_BAD_VALUE;
    }
    if (c->pool_bytes + size > X12_POOL_LIMIT) {
        return X12_ERROR_CODE_OUT_OF_MEMORY;
    }
    // calloc, not malloc: the client may write only part of the pool
    // before the compositor blits it, and heap garbage must never reach
    // the screen
    uint8_t *map = calloc(1, size);
    if (!map) {
        return X12_ERROR_CODE_OUT_OF_MEMORY;
    }
    if (x12_objects_alloc(&c->objects, id, OBJ_SHM_POOL,
                          x12_iface_find("x12_shm_pool")) != 0) {
        free(map);
        return X12_ERROR_CODE_INVALID_ID;
    }
    struct x12_object *o = x12_objects_get(&c->objects, id);
    o->as.pool.map = map;
    o->as.pool.size = size;
    o->as.pool.net = 1;
    c->pool_bytes += size;
    return 0;
}

int x12_pool_write(struct x12_conn *c, uint32_t id, uint32_t offset,
                   const uint8_t *data, uint32_t len) {
    struct x12_object *o = x12_objects_get(&c->objects, id);
    // a shm client owns its memory locally, writes feed net pools only
    if (!o || o->kind != OBJ_SHM_POOL || !o->as.pool.net) {
        return X12_ERROR_CODE_BAD_VALUE;
    }
    if (offset > o->as.pool.size || len > o->as.pool.size - offset) {
        return X12_ERROR_CODE_BAD_VALUE;
    }
    memcpy(o->as.pool.map + offset, data, len);
    return 0;
}

void x12_pool_destroy(struct x12_conn *c, uint32_t id) {
    struct x12_object *o = x12_objects_get(&c->objects, id);
    if (!o || o->kind != OBJ_SHM_POOL) {
        return;
    }
    // buffers carved out of this pool die with it, their pixels lived in
    // the mapping that is going away
    for (uint32_t bid = 1; bid < X12_MAX_OBJECTS; bid++) {
        struct x12_object *b = x12_objects_get(&c->objects, bid);
        if (b && b->kind == OBJ_BUFFER && b->as.buffer.pool_id == id) {
            x12_buffer_destroy(c, bid);
        }
    }
    if (o->as.pool.net) {
        free(o->as.pool.map);
    }
    else {
        munmap(o->as.pool.map, o->as.pool.size);
    }
    c->pool_bytes -= o->as.pool.size;
    x12_objects_free(&c->objects, id);
}

int x12_buffer_create(struct x12_conn *c, uint32_t pool_id, uint32_t id,
                      uint32_t offset, uint32_t width, uint32_t height,
                      uint32_t stride, uint32_t format) {
    struct x12_object *p = x12_objects_get(&c->objects, pool_id);
    if (!p || p->kind != OBJ_SHM_POOL) {
        return X12_ERROR_CODE_INVALID_ID;
    }
    uint32_t bpp = x12_format_bpp(format);
    // the carve must sit inside the pool and the stride must cover a row,
    // every product is widened so a hostile size cannot wrap past the check
    if (bpp == 0 || width == 0 || height == 0 ||
        stride < (uint64_t)width * bpp ||
        (uint64_t)stride * height + offset > p->as.pool.size) {
        return X12_ERROR_CODE_BAD_VALUE;
    }
    if (x12_objects_alloc(&c->objects, id, OBJ_BUFFER,
                          x12_iface_find("x12_buffer")) != 0) {
        return X12_ERROR_CODE_INVALID_ID;
    }
    struct x12_object *o = x12_objects_get(&c->objects, id);
    o->as.buffer.pixels = p->as.pool.map + offset;
    o->as.buffer.width = width;
    o->as.buffer.height = height;
    o->as.buffer.stride = stride;
    o->as.buffer.format = format;
    o->as.buffer.pool_id = pool_id;
    o->as.buffer.dmabuf_fd = -1;
    return 0;
}

void x12_buffer_destroy(struct x12_conn *c, uint32_t id) {
    struct x12_object *o = x12_objects_get(&c->objects, id);
    if (!o || o->kind != OBJ_BUFFER) {
        return;
    }
    // surfaces referencing the buffer drop the reference, the retire
    // bookkeeping keeps the credit until its normal leave
    for (uint32_t sid = 1; sid < X12_MAX_OBJECTS; sid++) {
        struct x12_object *s = x12_objects_get(&c->objects, sid);
        if (s && s->kind == OBJ_SURFACE) {
            x12_frame_drop_buffer(c, &s->as.surface, id);
        }
    }
    if (o->as.buffer.pixels && o->as.buffer.map_size) {
        munmap((void *)(uintptr_t)o->as.buffer.pixels, o->as.buffer.map_size);
    }
    if (o->as.buffer.dmabuf_fd >= 0) {
        close(o->as.buffer.dmabuf_fd);
    }
    x12_objects_free(&c->objects, id);
}

// a real dma-buf exposes an exporter name in its fdinfo, anything else
// on the wire claiming to be one gets turned away before it becomes a
// buffer the compositor would happily scan out blind. when the kernel
// exports a size line too it is returned as the authoritative byte
// count, 0 means the kernel does not report one
static uint64_t dmabuf_fd_size(int fd, int *is_dmabuf) {
    char path[64];
    snprintf(path, sizeof path, "/proc/self/fdinfo/%d", fd);
    FILE *f = fopen(path, "r");
    if (!f) {
        *is_dmabuf = 0;
        return 0;
    }
    char line[256];
    uint64_t size = 0;
    *is_dmabuf = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "exp_name:", 9) == 0) {
            *is_dmabuf = 1;
        }
        else if (strncmp(line, "size:", 5) == 0) {
            size = strtoull(line + 5, NULL, 10);
        }
    }
    fclose(f);
    return size;
}

// a truncated dma-buf maps fine and explodes on the first pixel read:
// the probe touches the last byte of the mapping under a sigbus catcher
// so a lying client cannot kill the compositor later. single threaded,
// the signal can only come from the probe's own access
static sigjmp_buf probe_env;
static volatile sig_atomic_t probe_active;

static void probe_sigbus(int sig) {
    (void)sig;
    if (probe_active) {
        probe_active = 0;
        siglongjmp(probe_env, 1);
    }
}

static int mapping_readable(uint8_t *map, size_t size) {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = probe_sigbus;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGBUS, &sa, &old) != 0) {
        return 0;
    }
    int ok = 1;
    if (sigsetjmp(probe_env, 1) == 0) {
        probe_active = 1;
        // the last page is where a truncated export bites, the first
        // one never does
        volatile uint8_t *p = map + size - 1;
        (void)*p;
    }
    else {
        ok = 0;
    }
    probe_active = 0;
    sigaction(SIGBUS, &old, NULL);
    return ok;
}

int x12_buffer_import(struct x12_conn *c, uint32_t id, int fd, uint32_t width,
                      uint32_t height, uint32_t format, uint64_t modifier,
                      uint32_t stride) {
    uint32_t bpp = x12_format_bpp(format);
    if (fd < 0 || bpp == 0 || width == 0 || height == 0 ||
        stride < (uint64_t)width * bpp) {
        close(fd);
        return X12_ERROR_CODE_BAD_VALUE;
    }
    // the fdinfo size is the kernel's own word on the export, when it
    // is present a short buffer dies here without any probing. the
    // sigbus probe below stays as the fallback for kernels that do not
    // report the size
    int is_dmabuf = 0;
    uint64_t fdinfo_size = dmabuf_fd_size(fd, &is_dmabuf);
    if (!is_dmabuf) {
        close(fd);
        return X12_ERROR_CODE_BAD_VALUE;
    }
    // the mapping is a convenience for the composite fallback, a gpu
    // only buffer stays unmapped and reaches the screen through scanout
    uint64_t full = (uint64_t)stride * height;
    if (full > SIZE_MAX) {
        close(fd);
        return X12_ERROR_CODE_BAD_VALUE;
    }
    size_t size = (size_t)full;
    if (fdinfo_size > 0 && fdinfo_size < full) {
        close(fd);
        return X12_ERROR_CODE_BAD_VALUE;
    }
    // an export that reports its size gets the cheap fstat answer, the
    // rest proves the mapping by touching it before the buffer ever
    // reaches the compositor
    struct stat dst;
    if (fstat(fd, &dst) == 0 && dst.st_size > 0 &&
        (uint64_t)dst.st_size < full) {
        close(fd);
        return X12_ERROR_CODE_BAD_VALUE;
    }
    uint8_t *map = mmap(0, size, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        map = NULL;
    }
    else if (!mapping_readable(map, size)) {
        munmap(map, size);
        close(fd);
        return X12_ERROR_CODE_BAD_VALUE;
    }
    if (x12_objects_alloc(&c->objects, id, OBJ_BUFFER,
                          x12_iface_find("x12_buffer")) != 0) {
        if (map) {
            munmap(map, size);
        }
        close(fd);
        return X12_ERROR_CODE_INVALID_ID;
    }
    struct x12_object *o = x12_objects_get(&c->objects, id);
    o->as.buffer.pixels = map;
    o->as.buffer.map_size = map ? size : 0;
    o->as.buffer.width = width;
    o->as.buffer.height = height;
    o->as.buffer.stride = stride;
    o->as.buffer.format = format;
    o->as.buffer.pool_id = 0;
    o->as.buffer.dmabuf_fd = fd;
    o->as.buffer.modifier = modifier;
    return 0;
}

// teardown hook for the reap path, the mapping goes away with the client
void x12_buffers_teardown(struct x12_conn *c) {
    // imported dma-bufs close with the client too, they live outside
    // the pools the loop below unmaps
    for (uint32_t id = 1; id < X12_MAX_OBJECTS; id++) {
        struct x12_object *o = x12_objects_get(&c->objects, id);
        if (o && o->kind == OBJ_BUFFER) {
            x12_buffer_destroy(c, id);
        }
    }
    for (uint32_t id = 1; id < X12_MAX_OBJECTS; id++) {
        struct x12_object *o = x12_objects_get(&c->objects, id);
        if (o && o->kind == OBJ_SHM_POOL) {
            x12_pool_destroy(c, id);
        }
    }
}

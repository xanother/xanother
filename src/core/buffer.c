// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#include "buffer.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

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

int x12_pool_resize(struct x12_conn *c, uint32_t id, uint32_t size) {
    struct x12_object *o = x12_objects_get(&c->objects, id);
    if (!o || o->kind != OBJ_SHM_POOL) {
        return X12_ERROR_CODE_INVALID_ID;
    }
    if (size == 0) {
        return X12_ERROR_CODE_BAD_VALUE;
    }
    uint64_t grown = c->pool_bytes - o->as.pool.size + size;
    if (grown > X12_POOL_LIMIT) {
        return X12_ERROR_CODE_OUT_OF_MEMORY;
    }
    uint8_t *map = mremap(o->as.pool.map, o->as.pool.size, size, MREMAP_MAYMOVE);
    if (map == MAP_FAILED) {
        return X12_ERROR_CODE_OUT_OF_MEMORY;
    }
    o->as.pool.map = map;
    o->as.pool.size = size;
    c->pool_bytes = grown;
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
    munmap(o->as.pool.map, o->as.pool.size);
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
    x12_objects_free(&c->objects, id);
}

// teardown hook for the reap path, the mapping goes away with the client
void x12_buffers_teardown(struct x12_conn *c) {
    for (uint32_t id = 1; id < X12_MAX_OBJECTS; id++) {
        struct x12_object *o = x12_objects_get(&c->objects, id);
        if (o && o->kind == OBJ_SHM_POOL) {
            x12_pool_destroy(c, id);
        }
    }
}

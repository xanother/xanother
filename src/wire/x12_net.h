// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// transport layer: one wire over unix sockets, tcp and tls alike. the
// address forms:
//   /path or unix:/path   local socket, the only transport that can
//                         pass file descriptors
//   tcp:HOST:PORT         plain tcp
//   tls:HOST:PORT         tls over tcp. the server names its
//                         certificate in X12_TLS_CERT and the key in
//                         X12_TLS_KEY, the client verifies against
//                         X12_TLS_CA or, with X12_TLS_INSECURE=1,
//                         accepts any certificate
// a bare path stays unix for the old positional socket argument, the
// protocol above the transport never learns which one it got

#ifndef X12_NET_H
#define X12_NET_H

#include <stddef.h>
#include <stdint.h>

#include <stddef.h>
#include <sys/types.h>

#define X12_NET_UNIX 0
#define X12_NET_TCP 1
#define X12_NET_TLS 2

struct x12_net_addr {
    int kind;
    char host[128];         // tcp and tls
    int port;
    char path[256];         // unix
};

// parses every address form, returns -1 on garbage
int x12_net_parse(const char *spec, struct x12_net_addr *out);

// listener side: the listening fd plus, for tls, the server context
// every accepted connection wraps around. -1 on error, the reason
// goes to stderr
int x12_net_listen(const struct x12_net_addr *a, void **tls_ctx_out);

// wraps an accepted fd: for tls runs the handshake and returns the
// connection object, otherwise returns NULL and the fd stands as is.
// a NULL result on a tls address means the handshake failed
int x12_net_accept(void *tls_ctx, const struct x12_net_addr *a, int fd,
                   void **tls_out);

// dialer side: a connected fd, plus the tls connection object through
// tls_out on tls addresses. -1 on error
int x12_net_connect(const struct x12_net_addr *a, void **tls_out);

// byte io on a connection, tls aware. plain fds keep errno semantics,
// a tls stall reports EAGAIN like a nonblocking socket would
ssize_t x12_net_read(void *tls, int fd, void *buf, size_t n);
ssize_t x12_net_write(void *tls, int fd, const void *buf, size_t n);

// frame envelope for the network transports: one complete wire frame
// goes out as [u32 hdr][u32 raw_len][data]. the high bit of hdr marks
// raw data, clear means a zlib stream of hdr & 0x7fffffff bytes that
// inflates to raw_len. small frames ride raw, compression pays off on
// the big remote buffer payloads
#define X12_NET_RAW_FLAG 0x80000000u
#define X12_NET_COMPRESS_MIN 512u
// the biggest frame the wire allows, mirrors the connection io buffer
#define X12_NET_MAX_FRAME 65536u
// envelope staging room: one envelope header past a biggest frame
#define X12_NET_STAGING (X12_NET_MAX_FRAME + 16u)

// wraps one complete frame for a network transport, raw frames pass
// through with just the envelope header. returns the bytes written to
// dst or 0 when dst cannot hold them
size_t x12_net_wrap(int net, uint8_t *dst, size_t dstcap,
                    const uint8_t *frame, size_t len);

// peels every complete envelope from src into dst and slides the
// partial remainder down. a raw_len outside the frame limits is a
// framing violation and returns -1
int x12_net_peel(uint8_t *src, size_t *src_len,
                 uint8_t *dst, size_t *dst_len, size_t dstcap);

// flips a socket to nonblocking io. the tls handshake runs before
// this, afterwards the poll loop owns all the pacing
void x12_net_set_nonblock(int fd);

// releases a tls connection object, NULL is fine
void x12_net_tls_free(void *tls);

#endif

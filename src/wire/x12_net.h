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

#endif

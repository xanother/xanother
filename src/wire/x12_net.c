// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

#include "x12_net.h"
#include "x12_wire.h"

#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <openssl/x509_vfy.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <openssl/err.h>
#include <zlib.h>
#include <openssl/ssl.h>

static int
parse_hostport(const char *s, struct x12_net_addr *out)
{
    const char *colon = strrchr(s, ':');
    size_t hlen;

    if (!colon || colon == s || !colon[1]) {
        return -1;
    }
    hlen = (size_t)(colon - s);
    if (hlen >= sizeof(out->host)) {
        return -1;
    }
    memcpy(out->host, s, hlen);
    out->host[hlen] = 0;
    out->port = atoi(colon + 1);
    if (out->port <= 0 || out->port > 65535) {
        return -1;
    }
    return 0;
}

int
x12_net_parse(const char *spec, struct x12_net_addr *out)
{
    memset(out, 0, sizeof *out);
    if (!spec || !*spec) {
        return -1;
    }
    if (strncmp(spec, "unix:", 5) == 0) {
        out->kind = X12_NET_UNIX;
        snprintf(out->path, sizeof(out->path), "%s", spec + 5);
        return out->path[0] ? 0 : -1;
    }
    if (strncmp(spec, "tcp:", 4) == 0) {
        out->kind = X12_NET_TCP;
        return parse_hostport(spec + 4, out);
    }
    if (strncmp(spec, "tls:", 4) == 0) {
        out->kind = X12_NET_TLS;
        return parse_hostport(spec + 4, out);
    }
    // a bare path stays unix, the positional socket argument of old
    out->kind = X12_NET_UNIX;
    snprintf(out->path, sizeof(out->path), "%s", spec);
    return 0;
}

static void
tls_log(const char *what)
{
    fprintf(stderr, "x12-net: %s\n", what);
    ERR_print_errors_fp(stderr);
}

static SSL_CTX *
tls_server_ctx(void)
{
    const char *cert = getenv("X12_TLS_CERT");
    const char *key = getenv("X12_TLS_KEY");
    SSL_CTX *ctx;

    if (!cert || !*cert || !key || !*key) {
        tls_log("tls listen needs X12_TLS_CERT and X12_TLS_KEY");
        return NULL;
    }
    ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        tls_log("tls context refused");
        return NULL;
    }
    if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        tls_log("tls certificate refused");
        SSL_CTX_free(ctx);
        return NULL;
    }
    return ctx;
}

static int
tcp_listen(const char *host, int port)
{
    struct addrinfo hints, *ai, *it;
    char service[16];
    int s = -1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    snprintf(service, sizeof service, "%d", port);
    if (getaddrinfo(host && *host ? host : NULL, service, &hints, &ai) != 0) {
        return -1;
    }
    for (it = ai; it; it = it->ai_next) {
        s = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (s < 0) {
            continue;
        }
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(s, it->ai_addr, it->ai_addrlen) == 0 &&
            listen(s, 16) == 0) {
            break;
        }
        close(s);
        s = -1;
    }
    freeaddrinfo(ai);
    return s;
}

int
x12_net_listen(const struct x12_net_addr *a, void **tls_ctx_out)
{
    *tls_ctx_out = NULL;

    if (a->kind == X12_NET_UNIX) {
        struct sockaddr_un addr;
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        if (s < 0) {
            perror("x12-net: unix socket");
            return -1;
        }
        memset(&addr, 0, sizeof addr);
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof addr.sun_path, "%.*s",
                 (int)sizeof addr.sun_path - 1, a->path);
        // the socket file stays behind otherwise and the next listen
        // fails on the stale one
        unlink(a->path);
        if (bind(s, (struct sockaddr *)&addr, sizeof addr) != 0 ||
            listen(s, 8) != 0) {
            perror("x12-net: unix bind");
            close(s);
            return -1;
        }
        return s;
    }

    int s = tcp_listen(a->host, a->port);
    if (s < 0) {
        perror("x12-net: tcp bind");
        return -1;
    }
    if (a->kind == X12_NET_TLS) {
        SSL_CTX *ctx = tls_server_ctx();
        if (!ctx) {
            close(s);
            return -1;
        }
        *tls_ctx_out = ctx;
    }
    return s;
}

int
x12_net_accept(void *tls_ctx, const struct x12_net_addr *a, int fd,
               void **tls_out)
{
    *tls_out = NULL;
    if (a->kind != X12_NET_TLS) {
        x12_net_set_nonblock(fd);
        return 0;
    }
    if (!tls_ctx) {
        return -1;
    }
    SSL *ssl = SSL_new((SSL_CTX *)tls_ctx);
    if (!ssl) {
        tls_log("tls connection refused");
        return -1;
    }
    SSL_set_fd(ssl, fd);
    // the handshake runs while the fd is still blocking, the poll loop
    // owns the pacing after the nonblocking flip
    if (SSL_accept(ssl) != 1) {
        tls_log("tls accept handshake failed");
        SSL_free(ssl);
        return -1;
    }
    x12_net_set_nonblock(fd);
    *tls_out = ssl;
    return 0;
}

int
x12_net_connect(const struct x12_net_addr *a, void **tls_out)
{
    *tls_out = NULL;

    if (a->kind == X12_NET_UNIX) {
        struct sockaddr_un addr;
        int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (s < 0) {
            return -1;
        }
        memset(&addr, 0, sizeof addr);
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof addr.sun_path, "%.*s",
                 (int)sizeof addr.sun_path - 1, a->path);
        if (connect(s, (struct sockaddr *)&addr, sizeof addr) != 0) {
            close(s);
            return -1;
        }
        return s;
    }

    struct addrinfo hints, *ai, *it;
    char service[16];
    int s = -1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof service, "%d", a->port);
    if (getaddrinfo(a->host, service, &hints, &ai) != 0) {
        return -1;
    }
    for (it = ai; it; it = it->ai_next) {
        s = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (s < 0) {
            continue;
        }
        if (connect(s, it->ai_addr, it->ai_addrlen) == 0) {
            break;
        }
        close(s);
        s = -1;
    }
    freeaddrinfo(ai);
    if (s < 0) {
        return -1;
    }

    if (a->kind == X12_NET_TLS) {
        SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
        SSL *ssl;
        int verify = 1;

        if (!ctx) {
            tls_log("tls context refused");
            close(s);
            return -1;
        }
        const char *ca = getenv("X12_TLS_CA");
        const char *insecure = getenv("X12_TLS_INSECURE");
        if (insecure && *insecure && strcmp(insecure, "0") != 0) {
            verify = 0;
        }
        if (verify) {
            if (ca && *ca) {
                if (SSL_CTX_load_verify_locations(ctx, ca, NULL) != 1) {
                    tls_log("tls ca refused");
                    SSL_CTX_free(ctx);
                    close(s);
                    return -1;
                }
            }
            else {
                SSL_CTX_set_default_verify_paths(ctx);
            }
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
        }
        else {
            SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
        }
        ssl = SSL_new(ctx);
        // the context dies with the connection, the ssl keeps its own
        // reference to what it needs
        SSL_CTX_free(ctx);
        if (!ssl) {
            tls_log("tls connection refused");
            close(s);
            return -1;
        }
        SSL_set_fd(ssl, s);
        SSL_set_tlsext_host_name(ssl, a->host);
        if (verify) {
            // an ip address verifies against the certificate ip san,
            // a name against its host name
            struct in6_addr probe;
            if (inet_pton(AF_INET, a->host, &probe) == 1 ||
                inet_pton(AF_INET6, a->host, &probe) == 1) {
                X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), a->host);
            }
            else {
                SSL_set1_host(ssl, a->host);
            }
        }
        if (SSL_connect(ssl) != 1) {
            tls_log("tls connect handshake failed");
            SSL_free(ssl);
            close(s);
            return -1;
        }
        *tls_out = ssl;
    }
    return s;
}

ssize_t
x12_net_read(void *tls, int fd, void *buf, size_t n)
{
    if (!tls) {
        return recv(fd, buf, n, 0);
    }
    int r = SSL_read((SSL *)tls, buf, (int)n);
    if (r > 0) {
        return r;
    }
    int err = SSL_get_error((SSL *)tls, r);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;
    }
    else if (err == SSL_ERROR_ZERO_RETURN) {
        return 0;
    }
    else {
        errno = EIO;
    }
    return -1;
}

ssize_t
x12_net_write(void *tls, int fd, const void *buf, size_t n)
{
    if (!tls) {
        return send(fd, buf, n, MSG_NOSIGNAL);
    }
    int r = SSL_write((SSL *)tls, buf, (int)n);
    if (r > 0) {
        return r;
    }
    int err = SSL_get_error((SSL *)tls, r);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;
    }
    else if (err == SSL_ERROR_ZERO_RETURN) {
        return 0;
    }
    else {
        errno = EIO;
    }
    return -1;
}

void
x12_net_tls_free(void *tls)
{
    if (tls) {
        SSL_free((SSL *)tls);
    }
}

void
x12_net_set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) {
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
}

size_t
x12_net_wrap(int net, uint8_t *dst, size_t dstcap,
             const uint8_t *frame, size_t len)
{
    if (!net) {
        if (dstcap < len) {
            return 0;
        }
        memcpy(dst, frame, len);
        return len;
    }
    uint32_t raw_len = (uint32_t)len;
    uint8_t comp[X12_NET_MAX_FRAME];
    uint32_t comp_len = 0;
    uint32_t hdr = X12_NET_RAW_FLAG | raw_len;
    const uint8_t *data = frame;
    if (len >= X12_NET_COMPRESS_MIN) {
        uLongf zl = (uLongf)sizeof comp;
        // compress2 keeps every frame self contained, a lost byte on
        // one frame can never poison the next
        if (compress2(comp, &zl, frame, (uLong)len, 6) == Z_OK &&
            (uint32_t)zl < raw_len) {
            comp_len = (uint32_t)zl;
            hdr = comp_len;
            data = comp;
        }
    }
    size_t total = 8u + (hdr & ~X12_NET_RAW_FLAG);
    if (dstcap < total) {
        return 0;
    }
    dst[0] = (uint8_t)(hdr & 0xff);
    dst[1] = (uint8_t)((hdr >> 8) & 0xff);
    dst[2] = (uint8_t)((hdr >> 16) & 0xff);
    dst[3] = (uint8_t)((hdr >> 24) & 0xff);
    dst[4] = (uint8_t)(raw_len & 0xff);
    dst[5] = (uint8_t)((raw_len >> 8) & 0xff);
    dst[6] = (uint8_t)((raw_len >> 16) & 0xff);
    dst[7] = (uint8_t)((raw_len >> 24) & 0xff);
    memcpy(dst + 8, data, hdr & ~X12_NET_RAW_FLAG);
    return total;
}

int
x12_net_peel(uint8_t *src, size_t *src_len,
             uint8_t *dst, size_t *dst_len, size_t dstcap)
{
    size_t off = 0;
    while (*src_len - off >= 8) {
        const uint8_t *h = src + off;
        uint32_t hdr = (uint32_t)h[0] | ((uint32_t)h[1] << 8) |
                       ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);
        uint32_t raw_len = (uint32_t)h[4] | ((uint32_t)h[5] << 8) |
                           ((uint32_t)h[6] << 16) | ((uint32_t)h[7] << 24);
        uint32_t data_len = hdr & ~X12_NET_RAW_FLAG;
        // a frame is at least a header and never bigger than the io
        // buffer, anything else is a framing violation
        if (raw_len < X12_HEADER_SIZE || raw_len > X12_NET_MAX_FRAME ||
            data_len > X12_NET_MAX_FRAME) {
            return -1;
        }
        if (8u + data_len > *src_len - off) {
            break;
        }
        if (*dst_len + raw_len > dstcap) {
            // the caller drains complete frames first, a full frame
            // buffer on top of that means a peer flooding past the
            // pacing rules
            return -1;
        }
        if (hdr & X12_NET_RAW_FLAG) {
            if (data_len != raw_len) {
                return -1;
            }
            memcpy(dst + *dst_len, h + 8, raw_len);
        }
        else {
            uLongf zl = raw_len;
            if (uncompress(dst + *dst_len, &zl, h + 8, data_len) != Z_OK ||
                zl != raw_len) {
                return -1;
            }
        }
        *dst_len += raw_len;
        off += 8u + data_len;
    }
    memmove(src, src + off, *src_len - off);
    *src_len -= off;
    return 0;
}

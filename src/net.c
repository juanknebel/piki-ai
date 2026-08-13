#include "net.h"
#include "certs.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

volatile sig_atomic_t net_interrupt = 0;

struct net_conn {
    int fd;
    SSL_CTX *ctx;
    SSL *ssl;
    int read_timeout_ms;   /* -1 = no limit */
};

enum {
    CONNECT_TIMEOUT_MS = 15000,
    POLL_TICK_MS = 200,
    CONNECT_ATTEMPTS = 3,      /* the target machines have flaky wifi */
    RETRY_BASE_MS = 1000       /* backoff: 1 s, then 2 s */
};

static void seterr(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;

    if (!err || !errlen)
        return;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static const char *ssl_reason(void)
{
    unsigned long e = ERR_peek_last_error();
    const char *s = e ? ERR_reason_error_string(e) : NULL;

    return s ? s : "unknown TLS error";
}

static int fd_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);

    if (fl < 0)
        return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Waits for fd to be ready for events. timeout_ms < 0 waits without limit.
 * Returns 1 ready, 0 timeout, -1 interrupted by net_interrupt. Polls in
 * short ticks so the SIGINT flag is serviced right away. */
static int wait_fd(int fd, short events, int timeout_ms)
{
    int waited = 0;

    for (;;) {
        struct pollfd p;
        int rc;

        if (net_interrupt)
            return -1;
        if (timeout_ms >= 0 && waited >= timeout_ms)
            return 0;
        p.fd = fd;
        p.events = events;
        p.revents = 0;
        rc = poll(&p, 1, POLL_TICK_MS);
        if (rc > 0)
            return 1;
        if (rc < 0 && errno != EINTR)
            return 0;
        waited += POLL_TICK_MS;
    }
}

/* Sleeps ms in short ticks so a Ctrl-C is noticed right away.
 * Returns -1 if interrupted. */
static int sleep_interruptible(int ms)
{
    int waited = 0;

    while (waited < ms) {
        if (net_interrupt)
            return -1;
        poll(NULL, 0, POLL_TICK_MS);
        waited += POLL_TICK_MS;
    }
    return net_interrupt ? -1 : 0;
}

static int tcp_connect(const char *host, int port, char *err, size_t errlen)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];
    int fd = -1;
    int rc;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof portstr, "%d", port);

    rc = getaddrinfo(host, portstr, &hints, &res);
    if (rc != 0) {
        seterr(err, errlen, "could not resolve %s: %s",
               host, gai_strerror(rc));
        return -1;
    }

    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (fd_nonblock(fd) < 0) {
            close(fd);
            fd = -1;
            continue;
        }
        rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0)
            break;
        if (errno == EINPROGRESS &&
            wait_fd(fd, POLLOUT, CONNECT_TIMEOUT_MS) == 1) {
            int soerr = 0;
            socklen_t sl = sizeof soerr;

            getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
            if (soerr == 0)
                break;
            errno = soerr;
        } else if (errno == EINPROGRESS) {
            errno = ETIMEDOUT;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0)
        seterr(err, errlen, "could not connect to %s:%d: %s",
               host, port, strerror(errno));
    return fd;
}

static int load_roots(SSL_CTX *ctx)
{
    X509_STORE *store = SSL_CTX_get_cert_store(ctx);
    size_t i;

    for (i = 0; i < PIKI_CA_ROOTS_COUNT; i++) {
        BIO *bio = BIO_new_mem_buf(piki_ca_roots[i], -1);
        X509 *x;
        int ok;

        if (!bio)
            return -1;
        x = PEM_read_bio_X509(bio, NULL, NULL, NULL);
        BIO_free(bio);
        if (!x)
            return -1;
        ok = X509_STORE_add_cert(store, x);
        X509_free(x);
        if (ok != 1)
            return -1;
    }
    return 0;
}

net_conn *net_connect(const char *host, int port, int use_tls,
                      char *err, size_t errlen)
{
    net_conn *c = calloc(1, sizeof *c);

    if (!c) {
        seterr(err, errlen, "out of memory");
        return NULL;
    }
    c->read_timeout_ms = -1;

    /* Retry the connect only: nothing has been sent yet, so a retry cannot
     * duplicate a request or a charge. TLS/certificate failures below are
     * never retried -- they are not transient and retrying hides them. */
    {
        int attempt;

        for (attempt = 0; ; attempt++) {
            c->fd = tcp_connect(host, port, err, errlen);
            if (c->fd >= 0 || attempt >= CONNECT_ATTEMPTS - 1)
                break;
            if (sleep_interruptible(RETRY_BASE_MS << attempt) < 0)
                break;
        }
    }
    if (c->fd < 0) {
        free(c);
        return NULL;
    }
    if (!use_tls)
        return c;

    c->ctx = SSL_CTX_new(TLS_client_method());
    if (!c->ctx ||
        SSL_CTX_set_min_proto_version(c->ctx, TLS1_2_VERSION) != 1 ||
        load_roots(c->ctx) < 0) {
        seterr(err, errlen, "could not initialize TLS: %s", ssl_reason());
        goto fail;
    }
    SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER, NULL);

    c->ssl = SSL_new(c->ctx);
    if (!c->ssl) {
        seterr(err, errlen, "could not initialize TLS: %s", ssl_reason());
        goto fail;
    }
    if (SSL_set_fd(c->ssl, c->fd) != 1 ||
        SSL_set_tlsext_host_name(c->ssl, host) != 1 || /* SNI */
        SSL_set1_host(c->ssl, host) != 1) {
        seterr(err, errlen, "could not initialize TLS: %s", ssl_reason());
        goto fail;
    }

    for (;;) {
        int rc = SSL_connect(c->ssl);
        int e;

        if (rc == 1)
            break;
        e = SSL_get_error(c->ssl, rc);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            int w = wait_fd(c->fd,
                            e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT,
                            CONNECT_TIMEOUT_MS);

            if (w == 1)
                continue;
            if (w < 0)
                seterr(err, errlen, "TLS handshake interrupted");
            else
                seterr(err, errlen, "timeout in TLS handshake with %s", host);
            goto fail;
        }
        {
            long vr = SSL_get_verify_result(c->ssl);

            if (vr != X509_V_OK)
                seterr(err, errlen, "certificate from %s rejected: %s "
                       "(try regenerating src/certs.h with tools/mkcerts.sh)",
                       host, X509_verify_cert_error_string(vr));
            else
                seterr(err, errlen, "TLS handshake with %s failed: %s "
                       "(check network and try tools/mkcerts.sh if root is missing)",
                       host, ssl_reason());
        }
        goto fail;
    }
    return c;

fail:
    net_close(c);
    return NULL;
}

/* Waits for read respecting the inactivity timeout. Returns 0 ready,
 * or NET_EINTR / NET_TIMEOUT to propagate. */
static int read_wait(net_conn *c, short events)
{
    int w = wait_fd(c->fd, events, c->read_timeout_ms);

    if (w == 1)
        return 0;
    if (w < 0)
        return NET_EINTR;
    return NET_TIMEOUT;
}

ssize_t net_read(net_conn *c, void *p, size_t n)
{
    for (;;) {
        int w;

        if (net_interrupt)
            return NET_EINTR;
        if (c->ssl) {
            int rc = SSL_read(c->ssl, p,
                              n > INT_MAX ? INT_MAX : (int)n);
            int e;

            if (rc > 0)
                return rc;
            e = SSL_get_error(c->ssl, rc);
            if (e == SSL_ERROR_ZERO_RETURN)
                return NET_EOF;
            if (e == SSL_ERROR_WANT_READ) {
                if ((w = read_wait(c, POLLIN)))
                    return w;
                continue;
            }
            if (e == SSL_ERROR_WANT_WRITE) {
                if ((w = read_wait(c, POLLOUT)))
                    return w;
                continue;
            }
            /* EOF without close_notify: many servers do this when cutting off */
            if (e == SSL_ERROR_SYSCALL && rc == 0 &&
                ERR_peek_error() == 0)
                return NET_EOF;
            return NET_ERR;
        } else {
            ssize_t rc = read(c->fd, p, n);

            if (rc > 0)
                return rc;
            if (rc == 0)
                return NET_EOF;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if ((w = read_wait(c, POLLIN)))
                    return w;
                continue;
            }
            if (errno != EINTR)
                return NET_ERR;
        }
    }
}

ssize_t net_write(net_conn *c, const void *p, size_t n)
{
    const char *q = p;
    size_t off = 0;

    while (off < n) {
        if (net_interrupt)
            return NET_EINTR;
        if (c->ssl) {
            size_t chunk = n - off;
            int rc, e;

            if (chunk > INT_MAX)
                chunk = INT_MAX;
            rc = SSL_write(c->ssl, q + off, (int)chunk);
            if (rc > 0) {
                off += (size_t)rc;
                continue;
            }
            e = SSL_get_error(c->ssl, rc);
            if (e == SSL_ERROR_WANT_READ) {
                if (wait_fd(c->fd, POLLIN, -1) < 0)
                    return NET_EINTR;
                continue;
            }
            if (e == SSL_ERROR_WANT_WRITE) {
                if (wait_fd(c->fd, POLLOUT, -1) < 0)
                    return NET_EINTR;
                continue;
            }
            return NET_ERR;
        } else {
            ssize_t rc = write(c->fd, q + off, n - off);

            if (rc >= 0) {
                off += (size_t)rc;
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (wait_fd(c->fd, POLLOUT, -1) < 0)
                    return NET_EINTR;
                continue;
            }
            if (errno != EINTR)
                return NET_ERR;
        }
    }
    return (ssize_t)off;
}

void net_close(net_conn *c)
{
    if (!c)
        return;
    if (c->ssl) {
        SSL_shutdown(c->ssl); /* close_notify best-effort */
        SSL_free(c->ssl);
    }
    if (c->ctx)
        SSL_CTX_free(c->ctx);
    if (c->fd >= 0)
        close(c->fd);
    free(c);
}

void net_set_read_timeout(net_conn *c, int seconds)
{
    c->read_timeout_ms = seconds > 0 ? seconds * 1000 : -1;
}

const char *net_tls_version(net_conn *c)
{
    return c->ssl ? SSL_get_version(c->ssl) : NULL;
}

const char *net_tls_cipher(net_conn *c)
{
    return c->ssl ? SSL_get_cipher_name(c->ssl) : NULL;
}

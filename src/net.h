#ifndef PIKI_NET_H
#define PIKI_NET_H

#include <signal.h>
#include <stddef.h>
#include <sys/types.h>

/* TCP connection with or without TLS. The higher-level modules (http, sse, api)
 * operate on this interface without knowing whether there is TLS underneath —
 * local providers (Ollama, llama-server) use use_tls == 0. */
typedef struct net_conn net_conn;

/* The program's SIGINT handler sets this flag to 1; operations in
 * progress return NET_EINTR as soon as they see it (cancels a streaming without
 * killing the process). The caller clears it before retrying. */
extern volatile sig_atomic_t net_interrupt;

enum {
    NET_EOF     = 0,
    NET_ERR     = -1,
    NET_EINTR   = -2,
    NET_TIMEOUT = -3
};

/* Connects (15 s timeout) and, with use_tls, negotiates TLS >= 1.2 with SNI and
 * certificate verification against the embedded roots (certs.h).
 * Returns NULL with the failure description in err. */
net_conn *net_connect(const char *host, int port, int use_tls,
                      char *err, size_t errlen);

/* Reads up to n bytes (n > 0): returns the amount read, or NET_EOF/NET_ERR/
 * NET_EINTR. Blocks without timeout: between streaming tokens an arbitrary
 * amount of time may pass; NET_EINTR is the escape route. */
ssize_t net_read(net_conn *c, void *p, size_t n);

/* Writes all n bytes: returns n, or NET_ERR/NET_EINTR. */
ssize_t net_write(net_conn *c, const void *p, size_t n);

/* Inactivity timeout for net_read (seconds; <= 0 disables it).
 * On expiry, net_read returns NET_TIMEOUT. */
void net_set_read_timeout(net_conn *c, int seconds);

void net_close(net_conn *c);

/* NULL if the connection does not use TLS. */
const char *net_tls_version(net_conn *c);
const char *net_tls_cipher(net_conn *c);

#endif /* PIKI_NET_H */

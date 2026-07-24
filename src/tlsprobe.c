/* Phase 0: validates the TCP connection + TLS handshake with certificate
 * verification against the embedded roots, and cancellation via Ctrl+C.
 *
 * Usage: tlsprobe [host [port]]        (default: openrouter.ai 443)
 */
#include "net.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void on_sigint(int sig)
{
    (void)sig;
    net_interrupt = 1;
}

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "openrouter.ai";
    int port = argc > 2 ? atoi(argv[2]) : 443;
    char err[256];
    struct sigaction sa;
    net_conn *c;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);

    c = net_connect(host, port, 1, err, sizeof err);
    if (!c) {
        fprintf(stderr, "FAIL: %s\n", err);
        return 1;
    }
    printf("OK: %s:%d  %s  %s\n", host, port,
           net_tls_version(c), net_tls_cipher(c));
    net_close(c);
    return 0;
}

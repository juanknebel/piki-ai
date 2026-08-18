#ifndef PIKI_HTTP_H
#define PIKI_HTTP_H

#include <stddef.h>
#include <sys/types.h>

#include "buf.h"
#include "net.h"

/* --- pure parts (testable without network) ------------------------- */

typedef struct {
    int status;
    long content_length;   /* -1 if it did not come */
    int chunked;
    int conn_close;        /* the server sent Connection: close */
} http_meta;

/* http_read_response: the server closed the connection before sending a
 * single byte. On a reused keep-alive connection that is the classic
 * idle-close race (RFC 9112 9.4): nothing of a response was consumed,
 * so the caller may safely retry the request once on a fresh one. */
#define HTTP_EARLY_EOF (-4)

/* Parses status line + headers from hdr (NUL-terminated text, at least up to
 * the empty line). 0 ok, -1 malformed. */
int http_parse_meta(const char *hdr, http_meta *m);

/* Incremental decoder for Transfer-Encoding: chunked. */
typedef struct {
    int state;
    long left;             /* bytes remaining of the current chunk */
    int done;
    char line[64];
    size_t linelen;
} chunk_dec;

void chunk_init(chunk_dec *d);

/* Consumes up to n bytes from in and appends the decoded data to out.
 * Returns bytes consumed (may be < n only when finishing), or -1 if the
 * framing is invalid. d->done is set to 1 upon seeing the final chunk. */
ssize_t chunk_feed(chunk_dec *d, const char *in, size_t n, buf_t *out);

/* --- over a connection --------------------------------------------- */

typedef struct {
    http_meta meta;
    net_conn *conn;
    chunk_dec cd;
    long body_left;        /* for Content-Length; -1 = until EOF */
    int body_done;
    char rbuf[8192];
    size_t rlen, rpos;
} http_resp;

/* Send the complete request. 0 ok, NET_ERR or NET_EINTR.
 * bearer NULL = no Authorization header (local providers).
 * keep_alive nonzero asks the server to keep the connection open. */
int http_post(net_conn *c, const char *host, const char *path,
              const char *bearer, const char *body, size_t bodylen,
              int keep_alive);
int http_get(net_conn *c, const char *host, const char *path,
             const char *bearer, int keep_alive);

/* Reads status + headers and leaves r ready to read the body. 0 ok;
 * NET_ERR/NET_EINTR with description in err. */
int http_read_response(net_conn *c, http_resp *r, char *err, size_t errlen);

/* Reads one more piece of the body and appends it DECODED to out.
 * >0 bytes added, 0 body complete, NET_ERR or NET_EINTR. */
ssize_t http_read_body(http_resp *r, buf_t *out);

/* Reads the whole body into out (cap 8 MB). 0 ok, NET_ERR or NET_EINTR. */
int http_read_all(http_resp *r, buf_t *out);

/* 1 iff the connection may carry another request: body fully read and
 * framed (chunked or Content-Length, never identity-until-EOF), no
 * unconsumed surplus bytes, and no Connection: close from the server. */
int http_resp_reusable(const http_resp *r);

#endif /* PIKI_HTTP_H */

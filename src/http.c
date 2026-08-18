#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "version.h"

#define HDR_MAX  (64 * 1024)
#define BODY_MAX (8 * 1024 * 1024)

/* --- header parsing ------------------------------------------------- */

static int value_has(const char *v, size_t vlen, const char *tok)
{
    size_t tlen = strlen(tok), i;

    for (i = 0; i + tlen <= vlen; i++)
        if (strncasecmp(v + i, tok, tlen) == 0)
            return 1;
    return 0;
}

int http_parse_meta(const char *hdr, http_meta *m)
{
    const char *p;

    m->status = 0;
    m->content_length = -1;
    m->chunked = 0;
    m->conn_close = 0;

    if (strncmp(hdr, "HTTP/1.", 7) != 0)
        return -1;
    p = strchr(hdr, ' ');
    if (!p)
        return -1;
    m->status = atoi(p + 1);
    if (m->status < 100 || m->status > 599)
        return -1;

    p = strchr(hdr, '\n');
    while (p) {
        const char *nl;
        size_t len;
        const char *colon;

        p++;
        nl = strchr(p, '\n');
        len = nl ? (size_t)(nl - p) : strlen(p);
        if (len && p[len - 1] == '\r')
            len--;
        if (!len)
            break; /* empty line: end of headers */
        colon = memchr(p, ':', len);
        if (colon) {
            size_t nlen = (size_t)(colon - p);
            const char *v = colon + 1;

            while (v < p + len && (*v == ' ' || *v == '\t'))
                v++;
            if (nlen == 14 &&
                strncasecmp(p, "Content-Length", 14) == 0)
                m->content_length = atol(v);
            else if (nlen == 17 &&
                     strncasecmp(p, "Transfer-Encoding", 17) == 0 &&
                     value_has(v, (size_t)(p + len - v), "chunked"))
                m->chunked = 1;
            else if (nlen == 10 &&
                     strncasecmp(p, "Connection", 10) == 0 &&
                     value_has(v, (size_t)(p + len - v), "close"))
                m->conn_close = 1;
        }
        p = nl;
    }
    return 0;
}

/* --- chunked decoder -------------------------------------------------- */

enum { CH_SIZE, CH_DATA, CH_DATA_END, CH_TRAILER, CH_DONE };

void chunk_init(chunk_dec *d)
{
    memset(d, 0, sizeof *d);
    d->state = CH_SIZE;
}

ssize_t chunk_feed(chunk_dec *d, const char *in, size_t n, buf_t *out)
{
    size_t i = 0;

    while (i < n) {
        char c;

        switch (d->state) {
        case CH_SIZE:
            c = in[i++];
            if (c != '\n') {
                if (d->linelen + 1 >= sizeof d->line) {
                    /* only an extension can be this long;
                     * without a preceding ';' it is garbage */
                    if (!memchr(d->line, ';', d->linelen))
                        return -1;
                } else {
                    d->line[d->linelen++] = c;
                }
                break;
            }
            {
                char *end;
                long sz;

                d->line[d->linelen] = '\0';
                sz = strtol(d->line, &end, 16);
                if (end == d->line || sz < 0)
                    return -1;
                if (*end && *end != ';' && *end != '\r')
                    return -1;
                d->linelen = 0;
                if (sz == 0) {
                    d->state = CH_TRAILER;
                } else {
                    d->left = sz;
                    d->state = CH_DATA;
                }
            }
            break;

        case CH_DATA: {
            size_t take = n - i;

            if ((long)take > d->left)
                take = (size_t)d->left;
            buf_append(out, in + i, take);
            i += take;
            d->left -= (long)take;
            if (d->left == 0)
                d->state = CH_DATA_END;
            break;
        }

        case CH_DATA_END: /* CRLF that closes the chunk */
            c = in[i++];
            if (c == '\r')
                break;
            if (c == '\n') {
                d->state = CH_SIZE;
                break;
            }
            return -1;

        case CH_TRAILER:
            c = in[i++];
            if (c == '\n') {
                int empty = d->linelen == 0 ||
                            (d->linelen == 1 && d->line[0] == '\r');

                d->linelen = 0;
                if (empty) {
                    d->done = 1;
                    d->state = CH_DONE;
                    return (ssize_t)i;
                }
            } else if (d->linelen + 1 < sizeof d->line) {
                /* trailers: only detecting the empty line matters */
                d->line[d->linelen++] = c;
            } else if (d->linelen == 0) {
                d->line[d->linelen++] = c;
            }
            break;

        case CH_DONE:
            return (ssize_t)i;
        }
    }
    return (ssize_t)i;
}

/* --- requests --------------------------------------------------------- */

static int send_request(net_conn *c, const char *method, const char *host,
                        const char *path, const char *bearer,
                        const char *body, size_t bodylen, int keep_alive)
{
    buf_t req;
    ssize_t rc;

    buf_init(&req);
    buf_printf(&req,
               "%s %s HTTP/1.1\r\n"
               "Host: %s\r\n"
               "User-Agent: piki/" PIKI_VERSION "\r\n"
               "Accept: application/json\r\n",
               method, path, host);
    if (bearer)
        buf_printf(&req, "Authorization: Bearer %s\r\n", bearer);
    if (body) {
        buf_puts(&req, "Content-Type: application/json\r\n");
        buf_printf(&req, "Content-Length: %lu\r\n",
                   (unsigned long)bodylen);
    }
    buf_printf(&req, "Connection: %s\r\n\r\n",
               keep_alive ? "keep-alive" : "close");
    if (body)
        buf_append(&req, body, bodylen);

    rc = net_write(c, req.data, req.len);
    buf_free(&req);
    if (rc < 0)
        return (int)rc;
    return 0;
}

int http_post(net_conn *c, const char *host, const char *path,
              const char *bearer, const char *body, size_t bodylen,
              int keep_alive)
{
    return send_request(c, "POST", host, path, bearer, body, bodylen,
                        keep_alive);
}

int http_get(net_conn *c, const char *host, const char *path,
             const char *bearer, int keep_alive)
{
    return send_request(c, "GET", host, path, bearer, NULL, 0, keep_alive);
}

/* --- response ----------------------------------------------------------- */

static void seterr(char *err, size_t errlen, const char *msg)
{
    if (err && errlen)
        snprintf(err, errlen, "%s", msg);
}

int http_read_response(net_conn *c, http_resp *r, char *err, size_t errlen)
{
    buf_t hdr;
    char tmp[4096];
    const char *term;
    size_t hdrlen, rest;

    memset(r, 0, sizeof *r);
    r->conn = c;
    r->meta.content_length = -1;
    chunk_init(&r->cd);
    buf_init(&hdr);

    for (;;) {
        ssize_t rc;

        term = hdr.data ? strstr(hdr.data, "\r\n\r\n") : NULL;
        if (term)
            break;
        if (hdr.len > HDR_MAX) {
            seterr(err, errlen, "headers too large");
            goto fail;
        }
        rc = net_read(c, tmp, sizeof tmp);
        if (rc == NET_EOF) {
            if (hdr.len == 0) {
                /* not one byte came: safe-to-retry on a reused conn */
                buf_free(&hdr);
                return HTTP_EARLY_EOF;
            }
            seterr(err, errlen,
                   "the server closed the connection without responding");
            goto fail;
        }
        if (rc == NET_EINTR) {
            buf_free(&hdr);
            return NET_EINTR;
        }
        if (rc < 0) {
            seterr(err, errlen, "network error reading the response");
            goto fail;
        }
        buf_append(&hdr, tmp, (size_t)rc);
    }

    /* what was read past the headers goes to the body buffer */
    hdrlen = (size_t)(term - hdr.data) + 4;
    rest = hdr.len - hdrlen;
    if (rest > sizeof r->rbuf) {
        seterr(err, errlen, "malformed HTTP response");
        goto fail;
    }
    memcpy(r->rbuf, hdr.data + hdrlen, rest);
    r->rlen = rest;
    hdr.data[hdrlen] = '\0';

    if (http_parse_meta(hdr.data, &r->meta) < 0) {
        seterr(err, errlen, "malformed HTTP response");
        goto fail;
    }
    if (!r->meta.chunked) {
        r->body_left = r->meta.content_length;
        if (r->meta.content_length == 0)
            r->body_done = 1;
    }
    buf_free(&hdr);
    return 0;

fail:
    buf_free(&hdr);
    return NET_ERR;
}

ssize_t http_read_body(http_resp *r, buf_t *out)
{
    if (r->body_done)
        return 0;

    for (;;) {
        size_t avail;

        if (r->rpos == r->rlen) {
            ssize_t rc = net_read(r->conn, r->rbuf, sizeof r->rbuf);

            if (rc == NET_EOF) {
                if (r->meta.chunked && !r->cd.done)
                    return NET_ERR;      /* truncated body */
                if (r->body_left > 0)
                    return NET_ERR;
                r->body_done = 1;
                return 0;                /* identity until EOF */
            }
            if (rc < 0)
                return rc;
            r->rlen = (size_t)rc;
            r->rpos = 0;
        }
        avail = r->rlen - r->rpos;

        if (r->meta.chunked) {
            size_t before = out->len;
            ssize_t used = chunk_feed(&r->cd, r->rbuf + r->rpos,
                                      avail, out);

            if (used < 0)
                return NET_ERR;
            r->rpos += (size_t)used;
            if (r->cd.done)
                r->body_done = 1;
            if (out->len > before)
                return (ssize_t)(out->len - before);
            if (r->body_done)
                return 0;
            /* consumed only framing: keep reading */
        } else if (r->body_left >= 0) {
            size_t take = avail;

            if ((long)take > r->body_left)
                take = (size_t)r->body_left;
            buf_append(out, r->rbuf + r->rpos, take);
            r->rpos += take;
            r->body_left -= (long)take;
            if (r->body_left == 0)
                r->body_done = 1;
            return (ssize_t)take;
        } else {
            buf_append(out, r->rbuf + r->rpos, avail);
            r->rpos = r->rlen;
            return (ssize_t)avail;
        }
    }
}

int http_resp_reusable(const http_resp *r)
{
    return r->body_done &&
           (r->meta.chunked || r->meta.content_length >= 0) &&
           r->rpos == r->rlen &&
           !r->meta.conn_close;
}

int http_read_all(http_resp *r, buf_t *out)
{
    for (;;) {
        ssize_t rc = http_read_body(r, out);

        if (rc == 0)
            return 0;
        if (rc < 0)
            return (int)rc;
        if (out->len > BODY_MAX)
            return NET_ERR;
    }
}

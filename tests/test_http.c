#include "http.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fails++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* Feeds the decoder `step` bytes at a time and validates output and total consumption. */
static void run_chunked(const char *in, size_t step,
                        const char *want, int want_done)
{
    chunk_dec d;
    buf_t out;
    size_t off = 0, len = strlen(in);
    int error = 0;

    chunk_init(&d);
    buf_init(&out);
    while (off < len && !d.done) {
        size_t n = len - off < step ? len - off : step;
        ssize_t used = chunk_feed(&d, in + off, n, &out);

        if (used < 0) {
            error = 1;
            break;
        }
        if (used == 0 && d.done)
            break;
        off += (size_t)used;
    }
    CHECK(!error);
    CHECK(d.done == want_done);
    CHECK(out.len == strlen(want) && memcmp(out.data, want, out.len) == 0);
    buf_free(&out);
}

int main(void)
{
    http_meta m;

    /* --- header parsing --- */
    CHECK(http_parse_meta(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 123\r\n"
        "\r\n", &m) == 0);
    CHECK(m.status == 200 && m.content_length == 123 && !m.chunked);

    CHECK(http_parse_meta(
        "HTTP/1.1 404 Not Found\r\n"
        "transfer-encoding: CHUNKED\r\n"
        "\r\n", &m) == 0);
    CHECK(m.status == 404 && m.content_length == -1 && m.chunked);

    CHECK(http_parse_meta(
        "HTTP/1.0 500 Oops\r\n\r\n", &m) == 0);
    CHECK(m.status == 500 && m.content_length == -1 && !m.chunked);

    /* headers after the empty line do not count */
    CHECK(http_parse_meta(
        "HTTP/1.1 200 OK\r\n"
        "\r\n"
        "Content-Length: 99\r\n", &m) == 0);
    CHECK(m.content_length == -1);

    /* Connection header, case-insensitive */
    CHECK(http_parse_meta(
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "\r\n", &m) == 0);
    CHECK(m.conn_close);
    CHECK(http_parse_meta(
        "HTTP/1.1 200 OK\r\n"
        "connection: CLOSE\r\n"
        "\r\n", &m) == 0);
    CHECK(m.conn_close);
    CHECK(http_parse_meta(
        "HTTP/1.1 200 OK\r\n"
        "Connection: keep-alive\r\n"
        "\r\n", &m) == 0);
    CHECK(!m.conn_close);
    CHECK(http_parse_meta(
        "HTTP/1.1 200 OK\r\n\r\n", &m) == 0);
    CHECK(!m.conn_close);

    CHECK(http_parse_meta("FTP/1.1 200 OK\r\n\r\n", &m) < 0);
    CHECK(http_parse_meta("HTTP/1.1 999 X\r\n\r\n", &m) < 0);
    CHECK(http_parse_meta("HTTP/1.1\r\n\r\n", &m) < 0);

    /* --- decoder chunked --- */

    /* all at once */
    run_chunked("5\r\nhola!\r\n3\r\nabc\r\n0\r\n\r\n", 9999, "hola!abc", 1);

    /* byte by byte: the case that breaks non-incremental decoders */
    run_chunked("5\r\nhola!\r\n3\r\nabc\r\n0\r\n\r\n", 1, "hola!abc", 1);

    /* 3 bytes at a time */
    run_chunked("5\r\nhola!\r\n3\r\nabc\r\n0\r\n\r\n", 3, "hola!abc", 1);

    /* uppercase hex + chunk extension */
    run_chunked("A;ext=\"x\"\r\n0123456789\r\n0\r\n\r\n", 4,
                "0123456789", 1);

    /* trailers after the final chunk */
    run_chunked("3\r\nabc\r\n0\r\nX-Trailer: v\r\n\r\n", 2, "abc", 1);

    /* invalid framing */
    {
        chunk_dec d;
        buf_t out;

        chunk_init(&d);
        buf_init(&out);
        CHECK(chunk_feed(&d, "zz\r\n", 4, &out) < 0);
        buf_free(&out);

        chunk_init(&d);
        buf_init(&out);
        CHECK(chunk_feed(&d, "3\r\nabcXX", 8, &out) < 0);
        buf_free(&out);
    }

    /* after done it consumes no more */
    {
        chunk_dec d;
        buf_t out;
        ssize_t used;

        chunk_init(&d);
        buf_init(&out);
        used = chunk_feed(&d, "0\r\n\r\nEXTRA", 10, &out);
        CHECK(used == 5 && d.done);
        used = chunk_feed(&d, "EXTRA", 5, &out);
        CHECK(used == 0 && out.len == 0);
        buf_free(&out);
    }

    /* --- connection reuse predicate --- */
    {
        http_resp r;

        /* chunked body read to the end: reusable */
        memset(&r, 0, sizeof r);
        r.body_done = 1;
        r.meta.chunked = 1;
        r.meta.content_length = -1;
        CHECK(http_resp_reusable(&r));

        /* Content-Length body read to the end: reusable */
        memset(&r, 0, sizeof r);
        r.body_done = 1;
        r.meta.content_length = 42;
        CHECK(http_resp_reusable(&r));

        /* identity-until-EOF framing: never reusable */
        memset(&r, 0, sizeof r);
        r.body_done = 1;
        r.meta.content_length = -1;
        CHECK(!http_resp_reusable(&r));

        /* body not fully read */
        memset(&r, 0, sizeof r);
        r.meta.chunked = 1;
        r.meta.content_length = -1;
        CHECK(!http_resp_reusable(&r));

        /* unconsumed surplus bytes poison the connection */
        memset(&r, 0, sizeof r);
        r.body_done = 1;
        r.meta.chunked = 1;
        r.meta.content_length = -1;
        r.rlen = 5;
        r.rpos = 0;
        CHECK(!http_resp_reusable(&r));

        /* the server said Connection: close */
        memset(&r, 0, sizeof r);
        r.body_done = 1;
        r.meta.content_length = 42;
        r.meta.conn_close = 1;
        CHECK(!http_resp_reusable(&r));
    }

    if (fails) {
        fprintf(stderr, "test_http: %d failures\n", fails);
        return 1;
    }
    puts("test_http: OK");
    return 0;
}

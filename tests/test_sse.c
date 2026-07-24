#include "sse.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fails++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* joins the received events separated by '|' */
typedef struct {
    buf_t got;
    int count;
    int abort_at; /* stops upon reaching this event (0 = never) */
} collect_t;

static int collect(const char *data, void *user)
{
    collect_t *c = user;

    if (c->count)
        buf_putc(&c->got, '|');
    buf_puts(&c->got, data);
    c->count++;
    return c->abort_at && c->count >= c->abort_at;
}

/* feeds `step` bytes at a time and compares the result */
static void run(const char *in, size_t step, const char *want, int events)
{
    sse_parser s;
    collect_t c;
    size_t off = 0, len = strlen(in);

    sse_init(&s);
    buf_init(&c.got);
    c.count = 0;
    c.abort_at = 0;
    while (off < len) {
        size_t n = len - off < step ? len - off : step;

        if (sse_feed(&s, in + off, n, collect, &c))
            break;
        off += n;
    }
    CHECK(c.count == events);
    CHECK((c.got.data ? strcmp(c.got.data, want) : want[0] != '\0' ? -1 : 0) == 0);
    sse_free(&s);
    buf_free(&c.got);
}

int main(void)
{
    /* simple event */
    run("data: hola\n\n", 9999, "hola", 1);

    /* CRLF */
    run("data: hola\r\n\r\n", 9999, "hola", 1);

    /* no space after the colon */
    run("data:x\n\n", 9999, "x", 1);

    /* multiple data lines are joined with \n */
    run("data: a\ndata: b\n\n", 9999, "a\nb", 1);

    /* comments and ignored fields */
    run(": keepalive\nevent: delta\nid: 7\ndata: y\nretry: 100\n\n",
        9999, "y", 1);

    /* several events in one feed; [DONE] is ordinary data */
    run("data: uno\n\ndata: dos\n\ndata: [DONE]\n\n",
        9999, "uno|dos|[DONE]", 3);

    /* byte by byte: the case that breaks non-incremental parsers */
    run("data: uno\r\n\r\ndata: dos\r\n\r\n", 1, "uno|dos", 2);

    /* empty lines without data generate no events */
    run("\n\n\n: nada\n\n", 9999, "", 0);

    /* JSON payload with ':' inside is not split */
    run("data: {\"a\":\"b:c\"}\n\n", 3, "{\"a\":\"b:c\"}", 1);

    /* the callback can abort the stream */
    {
        sse_parser s;
        collect_t c;
        int rc;

        sse_init(&s);
        buf_init(&c.got);
        c.count = 0;
        c.abort_at = 1;
        rc = sse_feed(&s, "data: a\n\ndata: b\n\n", 18, collect, &c);
        CHECK(rc != 0);
        CHECK(c.count == 1 && strcmp(c.got.data, "a") == 0);
        sse_free(&s);
        buf_free(&c.got);
    }

    if (fails) {
        fprintf(stderr, "test_sse: %d failures\n", fails);
        return 1;
    }
    puts("test_sse: OK");
    return 0;
}

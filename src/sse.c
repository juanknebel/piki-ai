#include "sse.h"

#include <string.h>

void sse_init(sse_parser *s)
{
    buf_init(&s->line);
    buf_init(&s->data);
    s->have_data = 0;
}

void sse_free(sse_parser *s)
{
    buf_free(&s->line);
    buf_free(&s->data);
}

/* Processes a complete line (already without the '\n'). */
static int flush_line(sse_parser *s, sse_cb cb, void *user)
{
    char *line = s->line.data ? s->line.data : "";
    size_t len = s->line.len;

    if (len && line[len - 1] == '\r') {
        len--;
        line[len] = '\0';
    }

    if (len == 0) {
        /* empty line: end of event */
        int rc = 0;

        if (s->have_data) {
            rc = cb(s->data.data ? s->data.data : "", user);
            buf_reset(&s->data);
            s->have_data = 0;
        }
        return rc;
    }
    if (line[0] == ':')
        return 0; /* comment (keepalive) */
    if (len >= 5 && memcmp(line, "data:", 5) == 0) {
        const char *val = line + 5;

        if (*val == ' ')
            val++;
        if (s->have_data)
            buf_putc(&s->data, '\n');
        buf_puts(&s->data, val);
        s->have_data = 1;
    }
    /* event:/id:/retry: and unknown fields are ignored */
    return 0;
}

int sse_feed(sse_parser *s, const char *in, size_t n,
             sse_cb cb, void *user)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (in[i] == '\n') {
            int rc = flush_line(s, cb, user);

            buf_reset(&s->line);
            if (rc)
                return rc;
        } else {
            buf_putc(&s->line, in[i]);
        }
    }
    return 0;
}

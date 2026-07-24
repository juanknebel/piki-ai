#ifndef PIKI_SSE_H
#define PIKI_SSE_H

#include <stddef.h>

#include "buf.h"

/* Incremental Server-Sent Events parser: accepts bytes split at
 * any point (arbitrary chunk/TLS record boundaries). It joins the
 * "data:" lines of each event (multiple ones are joined with '\n') and calls the
 * callback with the complete payload upon seeing the empty line. Ignores
 * comments (":...") and event:/id:/retry: fields. */

/* Returns 0 to continue; nonzero cuts the feed (and sse_feed
 * propagates it as its return value). data is NUL-terminated. */
typedef int (*sse_cb)(const char *data, void *user);

typedef struct {
    buf_t line;     /* partial line in progress */
    buf_t data;     /* accumulated payload of the event in progress */
    int have_data;
} sse_parser;

void sse_init(sse_parser *s);
void sse_free(sse_parser *s);

int sse_feed(sse_parser *s, const char *in, size_t n,
             sse_cb cb, void *user);

#endif /* PIKI_SSE_H */

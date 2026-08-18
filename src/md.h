#ifndef PIKI_MD_H
#define PIKI_MD_H

#include <stddef.h>
#include "buf.h"

/* Incremental markdown-to-ANSI renderer for streamed model output:
 * ``` fenced blocks (dim, only at line start), inline `code` (dim),
 * **bold**, and dimmed single * / _ markers. Like the other parsers it
 * must tolerate input split at arbitrary byte boundaries, so trailing
 * marker runs are held back until the next feed decides them. */
typedef struct {
    int color;          /* 0 = exact passthrough */
    int bold, code, codeblock;
    int at_line_start;
    int pend_ls;        /* was the pending run at line start? */
    char pend[4];       /* held-back run of '`' or '*' */
    size_t npend;
} md_state;

void md_init(md_state *st, int color);

/* Renders n bytes of s, appending the (possibly colored) output to out. */
void md_feed(md_state *st, const char *s, size_t n, buf_t *out);

/* Flushes any held-back marker bytes and closes any open ANSI mode. */
void md_finish(md_state *st, buf_t *out);

#endif /* PIKI_MD_H */

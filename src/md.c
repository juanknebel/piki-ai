#include <string.h>

#include "md.h"

#define MD_DIM   "\033[2m"
#define MD_BOLD  "\033[1m"
#define MD_RESET "\033[0m"

void md_init(md_state *st, int color)
{
    memset(st, 0, sizeof *st);
    st->color = color;
    st->at_line_start = 1;
}

/* An ANSI reset wipes every attribute, so re-arm the modes still open. */
static void restore_modes(const md_state *st, buf_t *out)
{
    if (st->bold)
        buf_puts(out, MD_BOLD);
    if (st->code || st->codeblock)
        buf_puts(out, MD_DIM);
}

/* Decides a completed run of '`' or '*' now that its length is known. */
static void flush_run(md_state *st, buf_t *out)
{
    size_t n = st->npend;
    char ch = st->pend[0];

    st->npend = 0;
    if (n == 0)
        return;

    if (ch == '`') {
        if (n == 3 && st->pend_ls && !st->code) {
            if (!st->codeblock) {
                buf_puts(out, MD_DIM);
                buf_puts(out, "```");
            } else {
                buf_puts(out, "```");
                buf_puts(out, MD_RESET);
                st->codeblock = 0;
                restore_modes(st, out);
                return;
            }
            st->codeblock = 1;
            return;
        }
        if (st->codeblock || n != 1) {
            /* literal backticks: inside a block, or an odd-length run */
            while (n--)
                buf_putc(out, '`');
            return;
        }
        if (!st->code) {
            buf_puts(out, MD_DIM);
            buf_putc(out, '`');
            st->code = 1;
        } else {
            buf_putc(out, '`');
            buf_puts(out, MD_RESET);
            st->code = 0;
            restore_modes(st, out);
        }
        return;
    }

    /* '*' run */
    if (st->codeblock || st->code) {
        while (n--)
            buf_putc(out, '*');
        return;
    }
    if (n == 2) {
        if (!st->bold) {
            buf_puts(out, MD_BOLD);
            buf_puts(out, "**");
            st->bold = 1;
        } else {
            buf_puts(out, "**");
            buf_puts(out, MD_RESET);
            st->bold = 0;
            restore_modes(st, out);
        }
    } else {
        buf_puts(out, MD_DIM);
        buf_putc(out, '*');
        buf_puts(out, MD_RESET);
        restore_modes(st, out);
    }
}

void md_feed(md_state *st, const char *s, size_t n, buf_t *out)
{
    size_t i;

    if (!st->color) {
        buf_append(out, s, n);
        return;
    }
    for (i = 0; i < n; i++) {
        char c = s[i];

        if (st->npend) {
            size_t max = st->pend[0] == '`' ? 3 : 2;

            if (c == st->pend[0] && st->npend < max) {
                st->pend[st->npend++] = c;
                if (st->npend == max)
                    flush_run(st, out);
                continue;
            }
            flush_run(st, out);
        }
        if (c == '`' || c == '*') {
            st->pend[0] = c;
            st->npend = 1;
            st->pend_ls = st->at_line_start;
            st->at_line_start = 0;
            if (c == '*' && (st->codeblock || st->code)) {
                /* no bold inside code: emit right away */
                st->npend = 0;
                buf_putc(out, '*');
            }
            continue;
        }
        if (c == '_' && !st->codeblock && !st->code) {
            buf_puts(out, MD_DIM);
            buf_putc(out, '_');
            buf_puts(out, MD_RESET);
            restore_modes(st, out);
            st->at_line_start = 0;
            continue;
        }
        buf_putc(out, c);
        st->at_line_start = c == '\n';
    }
}

void md_finish(md_state *st, buf_t *out)
{
    if (!st->color)
        return;
    flush_run(st, out);
    if (st->bold || st->code || st->codeblock)
        buf_puts(out, MD_RESET);
    st->bold = st->code = st->codeblock = 0;
    st->at_line_start = 1;
}

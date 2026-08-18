#include "buf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die_oom(void)
{
    fputs("piki: out of memory\n", stderr);
    abort();
}

void buf_init(buf_t *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(buf_t *b)
{
    free(b->data);
    buf_init(b);
}

void buf_reset(buf_t *b)
{
    b->len = 0;
    if (b->data)
        b->data[0] = '\0';
}

void buf_reserve(buf_t *b, size_t extra)
{
    size_t need = b->len + extra + 1;
    size_t cap;

    if (need < extra)
        die_oom();
    if (need <= b->cap)
        return;
    cap = b->cap ? b->cap : 64;
    while (cap < need) {
        if (cap > (size_t)-1 / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    {
        char *p = realloc(b->data, cap);
        if (!p)
            die_oom();
        b->data = p;
        b->cap = cap;
    }
}

void buf_append(buf_t *b, const void *p, size_t n)
{
    buf_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void buf_puts(buf_t *b, const char *s)
{
    buf_append(b, s, strlen(s));
}

void buf_putc(buf_t *b, char c)
{
    buf_append(b, &c, 1);
}

void buf_printf(buf_t *b, const char *fmt, ...)
{
    va_list ap, ap2;
    int n;

    va_start(ap, fmt);
    va_copy(ap2, ap);
    /* size measurement with NULL destination: C99 7.19.6.12; the
     * gcc -Wformat-truncation warning here is a false positive */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
    n = vsnprintf(NULL, 0, fmt, ap);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        die_oom(); /* invalid fmt: caller bug */
    }
    buf_reserve(b, (size_t)n);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

void buf_b64(buf_t *b, const void *p, size_t n)
{
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *s = p;
    size_t i;

    buf_reserve(b, (n + 2) / 3 * 4);
    for (i = 0; i + 3 <= n; i += 3) {
        buf_putc(b, tab[s[i] >> 2]);
        buf_putc(b, tab[(s[i] & 0x03) << 4 | s[i+1] >> 4]);
        buf_putc(b, tab[(s[i+1] & 0x0f) << 2 | s[i+2] >> 6]);
        buf_putc(b, tab[s[i+2] & 0x3f]);
    }
    if (i < n) {
        buf_putc(b, tab[s[i] >> 2]);
        if (i + 1 < n) {
            buf_putc(b, tab[(s[i] & 0x03) << 4 | s[i+1] >> 4]);
            buf_putc(b, tab[(s[i+1] & 0x0f) << 2]);
        } else {
            buf_putc(b, tab[(s[i] & 0x03) << 4]);
            buf_putc(b, '=');
        }
        buf_putc(b, '=');
    }
}

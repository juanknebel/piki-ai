#ifndef PIKI_BUF_H
#define PIKI_BUF_H

#include <stddef.h>

/* Dynamic string buffer. data is always NUL-terminated after any
 * operation (len does not count the NUL); it may contain embedded NULs if
 * added with buf_append. On out of memory it aborts the process. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} buf_t;

void buf_init(buf_t *b);
void buf_free(buf_t *b);
void buf_reset(buf_t *b);

/* Guarantees room for extra bytes plus the final NUL. */
void buf_reserve(buf_t *b, size_t extra);

void buf_append(buf_t *b, const void *p, size_t n);
void buf_puts(buf_t *b, const char *s);
void buf_putc(buf_t *b, char c);

/* Appends the base64 encoding of p (RFC 4648, with '=' padding). */
void buf_b64(buf_t *b, const void *p, size_t n);

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void buf_printf(buf_t *b, const char *fmt, ...);

#endif /* PIKI_BUF_H */

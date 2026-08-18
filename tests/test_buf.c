#include "buf.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fails++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

int main(void)
{
    buf_t b, c;
    char big[3000];
    int i;

    buf_init(&b);
    CHECK(b.len == 0 && b.data == NULL);

    buf_puts(&b, "hola");
    CHECK(b.len == 4 && strcmp(b.data, "hola") == 0);

    buf_putc(&b, ' ');
    buf_printf(&b, "mundo %d", 42);
    CHECK(strcmp(b.data, "hola mundo 42") == 0);

    buf_reset(&b);
    CHECK(b.len == 0 && b.data[0] == '\0');

    /* binary with embedded NUL; the trailing NUL is still present */
    buf_append(&b, "a\0b", 3);
    CHECK(b.len == 3 && memcmp(b.data, "a\0b\0", 4) == 0);

    /* growth with many small writes */
    buf_reset(&b);
    for (i = 0; i < 100000; i++)
        buf_putc(&b, 'x');
    CHECK(b.len == 100000 && b.data[99999] == 'x' && b.data[100000] == '\0');

    /* printf larger than the initial capacity */
    buf_init(&c);
    memset(big, 'y', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    buf_printf(&c, "<%s>", big);
    CHECK(c.len == sizeof big - 1 + 2);
    CHECK(c.data[0] == '<' && c.data[c.len - 1] == '>' &&
          c.data[c.len] == '\0');

    /* printf over previous content does not overwrite it */
    buf_reset(&b);
    buf_puts(&b, "ab");
    buf_printf(&b, "%s", "cd");
    CHECK(strcmp(b.data, "abcd") == 0);

    /* base64: RFC 4648 test vectors */
    {
        static const struct { const char *in, *out; } v[] = {
            {"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"},
            {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="},
            {"foobar", "Zm9vYmFy"},
        };
        size_t i;

        for (i = 0; i < sizeof v / sizeof v[0]; i++) {
            buf_reset(&b);
            buf_b64(&b, v[i].in, strlen(v[i].in));
            CHECK(strcmp(b.data, v[i].out) == 0);
        }

        /* binary bytes, including NUL and 0xFF */
        buf_reset(&b);
        buf_b64(&b, "\x00\xff\x10", 3);
        CHECK(strcmp(b.data, "AP8Q") == 0);

        /* appends after existing content, longer than one group run */
        buf_reset(&b);
        buf_puts(&b, "x:");
        buf_b64(&b, "aaaaaaaaaaaa", 12);   /* 4 groups of 3 */
        CHECK(strcmp(b.data, "x:YWFhYWFhYWFhYWFh") == 0);
    }

    buf_free(&b);
    buf_free(&c);
    CHECK(b.data == NULL && b.cap == 0);

    if (fails) {
        fprintf(stderr, "test_buf: %d failures\n", fails);
        return 1;
    }
    puts("test_buf: OK");
    return 0;
}

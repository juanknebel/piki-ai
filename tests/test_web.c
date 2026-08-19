#include <stdio.h>
#include <string.h>

#include "web.h"

static int fails;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fails++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define STR(b) ((b).data ? (b).data : "")

/* Strips html feeding step bytes at a time. */
static void strip(const char *html, size_t step, buf_t *out)
{
    htmlstrip h;
    size_t len = strlen(html), i;

    htmlstrip_init(&h);
    for (i = 0; i < len; i += step) {
        size_t n = len - i < step ? len - i : step;

        htmlstrip_feed(&h, html + i, n, out);
    }
    htmlstrip_finish(&h, out);
}

/* One-shot output must match want, and every split size must match the
 * one-shot output (the incremental-parser invariant). */
static void check_strip(const char *html, const char *want)
{
    buf_t whole, split;
    size_t step;

    buf_init(&whole);
    strip(html, strlen(html) ? strlen(html) : 1, &whole);
    if (want)
        CHECK(strcmp(STR(whole), want) == 0);
    buf_init(&split);
    for (step = 1; step <= 5; step++) {
        buf_reset(&split);
        strip(html, step, &split);
        CHECK(strcmp(STR(split), STR(whole)) == 0);
    }
    buf_free(&whole);
    buf_free(&split);
}

/* A trimmed-down DDG Lite results page: one redirect-wrapped link, one
 * direct https link, navigation links that must be skipped. */
static const char DDG_FIXTURE[] =
    "<html><body><form action='/lite/'>"
    "<a href='/lite/?q=x&s=10'>Next Page</a>"
    "<table>"
    "<tr><td>1.</td><td><a rel=\"nofollow\" "
    "href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fwww.example.com"
    "%2Fdocs%2Fintro&rut=abc\" class='result-link'>Example &amp; "
    "<b>Friends</b></a></td></tr>"
    "<tr><td></td><td class='result-snippet'>An example snippet with "
    "<b>bold</b> text.</td></tr>"
    "<tr><td>2.</td><td><a rel=\"nofollow\" "
    "href=\"https://second.org/page\">Second result</a></td></tr>"
    "<tr><td></td><td class='result-snippet'>Second snippet.</td></tr>"
    "<tr><td><a href='https://duckduckgo.com/feedback'>Feedback</a>"
    "</td></tr>"
    "</table></body></html>";

static const char DDG_CAPTCHA[] =
    "<html><body><div class='anomaly-modal'>"
    "<p>Select all squares containing a duck:</p>"
    "<img src='../assets/anomaly/images/challenge/x.jpg'>"
    "</div></body></html>";

int main(void)
{
    buf_t b;

    /* --- html stripper --- */
    check_strip("plain text", "plain text");
    check_strip("a <b>bold</b> word", "a bold word");
    check_strip("one<p>two</p>three", "one\ntwo\nthree");
    check_strip("a<br>b", "a\nb");
    check_strip("x <script>var a = '<p>ignored</p>';</script> y", "x y");
    check_strip("x<style>p { color: red }</style>y", "xy");
    check_strip("a<!-- <b>comment</b> -->b", "ab");
    check_strip("<!DOCTYPE html><p>hi</p>", "hi");
    check_strip("l&amp;m &lt;tag&gt; &quot;q&quot; &#39;a&#39;&nbsp;end",
                "l&m <tag> \"q\" 'a' end");
    check_strip("&#65;&#x42;", "AB");
    check_strip("&unknown; &", "&unknown; &");
    check_strip("a   b\n\n  c", "a b c");
    check_strip("<a href=\"x>y\" title='p>q'>t</a>", "t");
    check_strip("<td>a</td><td>b</td>", "a b");
    check_strip("", "");

    /* --- percent encode/decode --- */
    buf_init(&b);
    web_percent_encode("a b+c/d?e=f", &b);
    CHECK(strcmp(b.data, "a%20b%2Bc%2Fd%3Fe%3Df") == 0);
    buf_reset(&b);
    web_percent_encode("cafe~_-.", &b);
    CHECK(strcmp(b.data, "cafe~_-.") == 0);
    buf_reset(&b);
    web_percent_decode("https%3A%2F%2Fx.com%2Fa%20b", 27, &b);
    CHECK(strcmp(b.data, "https://x.com/a b") == 0);
    buf_reset(&b);
    web_percent_decode("100%zX%4", 8, &b);   /* invalid escapes literal */
    CHECK(strcmp(b.data, "100%zX%4") == 0);
    buf_reset(&b);
    web_percent_decode("%C3%B1", 6, &b);     /* UTF-8 bytes preserved */
    CHECK(b.len == 2 && (unsigned char)b.data[0] == 0xC3 &&
          (unsigned char)b.data[1] == 0xB1);
    buf_free(&b);

    /* --- url splitter --- */
    {
        char host[256], path[512];
        int port, tls;

        CHECK(web_url_split("https://example.com", host, sizeof host,
                            &port, &tls, path, sizeof path) == 0);
        CHECK(strcmp(host, "example.com") == 0 && port == 443 &&
              tls == 1 && strcmp(path, "/") == 0);

        CHECK(web_url_split("http://h:8080/p?q=x", host, sizeof host,
                            &port, &tls, path, sizeof path) == 0);
        CHECK(strcmp(host, "h") == 0 && port == 8080 && tls == 0 &&
              strcmp(path, "/p?q=x") == 0);

        CHECK(web_url_split("ftp://x/", host, sizeof host, &port, &tls,
                            path, sizeof path) == -1);
        CHECK(web_url_split("example.com/x", host, sizeof host, &port,
                            &tls, path, sizeof path) == -1);
        CHECK(web_url_split("https:///x", host, sizeof host, &port,
                            &tls, path, sizeof path) == -1);
        CHECK(web_url_split("https://h:99999/", host, sizeof host,
                            &port, &tls, path, sizeof path) == -1);
        {
            char tiny[4];

            CHECK(web_url_split("https://longhostname.com/", tiny,
                                sizeof tiny, &port, &tls, path,
                                sizeof path) == -1);
        }
    }

    /* --- DDG results parser --- */
    buf_init(&b);
    {
        int rc = web_ddg_parse(DDG_FIXTURE, sizeof DDG_FIXTURE - 1, 5, &b);

        CHECK(rc == 2);
        CHECK(strstr(STR(b), "1. Example & Friends") != NULL);
        CHECK(strstr(STR(b), "https://www.example.com/docs/intro") != NULL);
        CHECK(strstr(STR(b), "An example snippet with bold text.") != NULL);
        CHECK(strstr(STR(b), "2. Second result") != NULL);
        CHECK(strstr(STR(b), "https://second.org/page") != NULL);
        CHECK(strstr(STR(b), "Feedback") == NULL);     /* ddg link skipped */
        CHECK(strstr(STR(b), "Next Page") == NULL);    /* relative skipped */
    }
    buf_reset(&b);
    {
        int rc = web_ddg_parse(DDG_FIXTURE, sizeof DDG_FIXTURE - 1, 1, &b);

        CHECK(rc == 1);   /* max_results respected */
        CHECK(strstr(STR(b), "Second result") == NULL);
    }
    buf_reset(&b);
    CHECK(web_ddg_parse(DDG_CAPTCHA, sizeof DDG_CAPTCHA - 1, 5, &b) == -1);
    CHECK(strstr(STR(b), "captcha") != NULL);
    buf_reset(&b);
    CHECK(web_ddg_parse("<html><body>no links</body></html>", 34, 5,
                        &b) == -1);
    CHECK(strstr(STR(b), "no results") != NULL);
    buf_free(&b);

    if (fails) {
        fprintf(stderr, "test_web: %d failures\n", fails);
        return 1;
    }
    puts("test_web: OK");
    return 0;
}

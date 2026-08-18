#include <stdio.h>
#include <string.h>

#include "md.h"

static int fails;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fails++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* buf data stays NULL until the first append */
#define STR(b) ((b).data ? (b).data : "")

#define DIM   "\033[2m"
#define BOLD  "\033[1m"
#define RESET "\033[0m"

/* Renders the whole input feeding step bytes at a time. */
static void render(const char *in, size_t step, int color, buf_t *out)
{
    md_state st;
    size_t len = strlen(in), i;

    md_init(&st, color);
    for (i = 0; i < len; i += step) {
        size_t n = len - i < step ? len - i : step;

        md_feed(&st, in + i, n, out);
    }
    md_finish(&st, out);
}

/* One-shot output must match the expectation and every split must match
 * the one-shot output (the incremental-parser invariant). */
static void check_render(const char *in, const char *want)
{
    buf_t whole, split;
    size_t step;

    buf_init(&whole);
    render(in, strlen(in) ? strlen(in) : 1, 1, &whole);
    if (want)
        CHECK(strcmp(STR(whole), want) == 0);
    buf_init(&split);
    for (step = 1; step <= 4; step++) {
        buf_reset(&split);
        render(in, step, 1, &split);
        CHECK(strcmp(STR(split), STR(whole)) == 0);
    }
    buf_free(&whole);
    buf_free(&split);
}

int main(void)
{
    buf_t b;

    /* plain text is untouched */
    check_render("hello world\n", "hello world\n");

    /* inline code */
    check_render("hi `x` yo", "hi " DIM "`x`" RESET " yo");

    /* bold */
    check_render("a **b** c", "a " BOLD "**b**" RESET " c");

    /* single * and _ markers are dimmed */
    check_render("a * b", "a " DIM "*" RESET " b");
    check_render("a_b", "a" DIM "_" RESET "b");

    /* fenced block: dim from the opening fence to the closing one;
     * markers inside are literal */
    check_render("```\na_`b **c\n```\n",
                 DIM "```\na_`b **c\n```" RESET "\n");
    check_render("see:\n```c\nint x;\n```\ndone",
                 "see:\n" DIM "```c\nint x;\n```" RESET "\ndone");

    /* three backticks NOT at line start are literal, not a fence */
    check_render("a```b", "a```b");

    /* a double backtick is literal */
    check_render("a``b", "a``b");

    /* bold marker inside inline code is literal */
    check_render("`a*b`", DIM "`a*b`" RESET);

    /* nesting: inline code inside bold restores bold after the reset */
    check_render("**a `b` c**",
                 BOLD "**a " DIM "`b`" RESET BOLD " c**" RESET);

    /* unclosed modes are reset by md_finish */
    check_render("**x", BOLD "**x" RESET);
    check_render("`x", DIM "`x" RESET);
    check_render("```\nx", DIM "```\nx" RESET);

    /* trailing lone backtick is flushed (and reset) by md_finish */
    check_render("x`", "x" DIM "`" RESET);

    /* color off: exact passthrough, markers and all */
    {
        md_state st;

        buf_init(&b);
        md_init(&st, 0);
        md_feed(&st, "**a** `b`\n```\nc\n```", 19, &b);
        md_finish(&st, &b);
        CHECK(strcmp(b.data, "**a** `b`\n```\nc\n```") == 0);
        buf_free(&b);
    }

    /* empty input */
    check_render("", "");

    if (fails) {
        fprintf(stderr, "test_md: %d failures\n", fails);
        return 1;
    }
    puts("test_md: OK");
    return 0;
}

#include "chat.h"

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
    chat_t c;
    chat_msg win[16];
    size_t n;

    chat_init(&c);
    CHECK(c.n == 0);

    chat_add(&c, "user", "hola");
    chat_add(&c, "assistant", "buenas");
    chat_add(&c, "user", "como va?");
    CHECK(c.n == 3);
    CHECK(strcmp(c.msgs[1].content, "buenas") == 0);

    /* full window, no system */
    n = chat_window(&c, 40, 0, win, 16);
    CHECK(n == 3 && strcmp(win[0].content, "hola") == 0);

    /* with system: goes first */
    chat_set_system(&c, "se breve");
    n = chat_window(&c, 40, 0, win, 16);
    CHECK(n == 4);
    CHECK(strcmp(win[0].role, "system") == 0 &&
          strcmp(win[0].content, "se breve") == 0);
    CHECK(strcmp(win[1].content, "hola") == 0);

    /* truncated: only the last 2 messages + system */
    n = chat_window(&c, 2, 0, win, 16);
    CHECK(n == 3);
    CHECK(strcmp(win[1].content, "buenas") == 0 &&
          strcmp(win[2].content, "como va?") == 0);

    /* small outcap limits */
    n = chat_window(&c, 40, 0, win, 2);
    CHECK(n == 2 && strcmp(win[0].role, "system") == 0);

    /* pop */
    chat_pop(&c);
    CHECK(c.n == 2 && strcmp(c.msgs[1].content, "buenas") == 0);

    /* clear keeps the system */
    chat_clear(&c);
    CHECK(c.n == 0 && c.system && strcmp(c.system, "se breve") == 0);
    n = chat_window(&c, 40, 0, win, 16);
    CHECK(n == 1 && strcmp(win[0].role, "system") == 0);

    /* remove the system */
    chat_set_system(&c, NULL);
    n = chat_window(&c, 40, 0, win, 16);
    CHECK(n == 0);

    /* array growth */
    {
        int i;
        char msg[32];

        for (i = 0; i < 100; i++) {
            snprintf(msg, sizeof msg, "msg %d", i);
            chat_add(&c, "user", msg);
        }
        CHECK(c.n == 100);
        CHECK(strcmp(c.msgs[99].content, "msg 99") == 0);
        n = chat_window(&c, 10, 0, win, 16);
        CHECK(n == 10 && strcmp(win[0].content, "msg 90") == 0);
    }

    chat_free(&c);
    CHECK(c.n == 0 && c.msgs == NULL && c.system == NULL);

    /* --- RAM cap: oldest messages are evicted and freed --- */
    {
        char big[201];
        int i;

        memset(big, 'x', sizeof big - 1);
        big[sizeof big - 1] = '\0';

        chat_init(&c);
        chat_set_system(&c, "sys");
        chat_set_max_bytes(&c, 1000);      /* room for 5 x 200 bytes */
        for (i = 0; i < 20; i++)
            chat_add(&c, "user", big);
        CHECK(c.n == 5 && c.bytes == 1000);
        /* the system prompt is never evicted */
        CHECK(c.system && strcmp(c.system, "sys") == 0);
        /* bytes stays consistent after popping */
        chat_pop(&c);
        CHECK(c.n == 4 && c.bytes == 800);
        chat_free(&c);
    }

    /* a single message larger than the cap is still kept */
    {
        char huge[2001];

        memset(huge, 'y', sizeof huge - 1);
        huge[sizeof huge - 1] = '\0';
        chat_init(&c);
        chat_set_max_bytes(&c, 100);
        chat_add(&c, "user", huge);
        CHECK(c.n == 1 && c.bytes == 2000);
        chat_free(&c);
    }

    /* lowering the cap later evicts right away */
    {
        chat_init(&c);
        chat_add(&c, "user", "aaaaa");     /* 5 */
        chat_add(&c, "user", "bbbbb");     /* 5 */
        chat_add(&c, "user", "ccccc");     /* 5 */
        CHECK(c.n == 3 && c.bytes == 15);
        chat_set_max_bytes(&c, 10);
        CHECK(c.n == 2 && c.bytes == 10);
        CHECK(strcmp(c.msgs[0].content, "bbbbb") == 0);
        chat_free(&c);
    }

    /* --- send window: byte budget --- */
    {
        chat_init(&c);
        chat_add(&c, "user", "1234567890");      /* 10 */
        chat_add(&c, "assistant", "1234567890"); /* 10 */
        chat_add(&c, "user", "1234567890");      /* 10 */

        /* no byte limit: everything */
        n = chat_window(&c, 40, 0, win, 16);
        CHECK(n == 3);

        /* budget for two messages: the newest two */
        n = chat_window(&c, 40, 20, win, 16);
        CHECK(n == 2);

        /* budget smaller than one message: still returns the newest */
        n = chat_window(&c, 40, 1, win, 16);
        CHECK(n == 1);

        /* the system prompt counts against the budget but always goes */
        chat_set_system(&c, "12345");            /* 5 */
        n = chat_window(&c, 40, 1, win, 16);
        CHECK(n == 2 && strcmp(win[0].role, "system") == 0);

        /* whichever limit binds first wins */
        n = chat_window(&c, 1, 1000, win, 16);
        CHECK(n == 2);                            /* system + 1 message */
        chat_free(&c);
    }

    /* --- trim: keeps only the newest n, system untouched --- */
    {
        chat_init(&c);
        chat_set_system(&c, "sys");
        chat_add(&c, "user", "aaaaa");
        chat_add(&c, "assistant", "bbbbb");
        chat_add(&c, "user", "ccccc");
        chat_trim(&c, 5);                  /* larger than n: no-op */
        CHECK(c.n == 3 && c.bytes == 15);
        chat_trim(&c, 2);
        CHECK(c.n == 2 && c.bytes == 10);
        CHECK(strcmp(c.msgs[0].content, "bbbbb") == 0);
        CHECK(c.system && strcmp(c.system, "sys") == 0);
        chat_trim(&c, 0);
        CHECK(c.n == 0 && c.bytes == 0);
        chat_free(&c);
    }

    /* --- compact: summary replaces everything before the kept tail --- */
    {
        size_t i, want;

        chat_init(&c);
        chat_set_system(&c, "sys");
        for (i = 0; i < 5; i++) {
            chat_add(&c, "user", "question");
            chat_add(&c, "assistant", "answer");
        }
        chat_compact_apply(&c, "the summary", 4);
        CHECK(c.n == 5);
        CHECK(strcmp(c.msgs[0].role, "user") == 0);
        CHECK(strcmp(c.msgs[0].content,
                     "[Summary of the earlier conversation]\n"
                     "the summary") == 0);
        CHECK(strcmp(c.msgs[1].role, "user") == 0);
        CHECK(strcmp(c.msgs[4].content, "answer") == 0);
        CHECK(c.system && strcmp(c.system, "sys") == 0);
        want = 0;
        for (i = 0; i < c.n; i++)
            want += strlen(c.msgs[i].content);
        CHECK(c.bytes == want);
        chat_free(&c);
    }

    /* --- compact: no-op when there is nothing older than the tail --- */
    {
        chat_init(&c);
        chat_add(&c, "user", "q");
        chat_add(&c, "assistant", "a");
        chat_compact_apply(&c, "s", 4);
        CHECK(c.n == 2 && strcmp(c.msgs[0].content, "q") == 0);
        chat_free(&c);
    }

    /* --- compact: tail is shrunk until it starts with a user message --- */
    {
        chat_init(&c);
        chat_add(&c, "user", "q1");
        chat_add(&c, "assistant", "a1");
        chat_add(&c, "user", "q2");
        chat_add(&c, "assistant", "a2");
        chat_compact_apply(&c, "s", 3);   /* tail would start at a1 */
        CHECK(c.n == 3);
        CHECK(strcmp(c.msgs[1].content, "q2") == 0);
        CHECK(strcmp(c.msgs[2].content, "a2") == 0);
        chat_free(&c);
    }

    /* --- compact: a tail with no user message leaves just the summary --- */
    {
        chat_init(&c);
        chat_add(&c, "user", "q1");
        chat_add(&c, "assistant", "a1");
        chat_add(&c, "assistant", "a2");
        chat_compact_apply(&c, "s", 2);
        CHECK(c.n == 1);
        CHECK(strcmp(c.msgs[0].role, "user") == 0);
        chat_free(&c);
    }

    /* --- compact: max_bytes still enforced, newest never evicted --- */
    {
        chat_init(&c);
        chat_set_max_bytes(&c, 20);
        chat_add(&c, "user", "aaaaaaaa");
        chat_add(&c, "assistant", "bbbbbbbb");
        chat_add(&c, "user", "cccc");
        chat_compact_apply(&c,
                           "a very long summary that blows past the cap",
                           1);
        CHECK(c.n == 1);   /* oversized summary evicted, newest kept */
        CHECK(strcmp(c.msgs[0].content, "cccc") == 0);
        chat_free(&c);
    }

    if (fails) {
        fprintf(stderr, "test_chat: %d failures\n", fails);
        return 1;
    }
    puts("test_chat: OK");
    return 0;
}

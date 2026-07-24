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
    n = chat_window(&c, 40, win, 16);
    CHECK(n == 3 && strcmp(win[0].content, "hola") == 0);

    /* with system: goes first */
    chat_set_system(&c, "se breve");
    n = chat_window(&c, 40, win, 16);
    CHECK(n == 4);
    CHECK(strcmp(win[0].role, "system") == 0 &&
          strcmp(win[0].content, "se breve") == 0);
    CHECK(strcmp(win[1].content, "hola") == 0);

    /* truncated: only the last 2 messages + system */
    n = chat_window(&c, 2, win, 16);
    CHECK(n == 3);
    CHECK(strcmp(win[1].content, "buenas") == 0 &&
          strcmp(win[2].content, "como va?") == 0);

    /* small outcap limits */
    n = chat_window(&c, 40, win, 2);
    CHECK(n == 2 && strcmp(win[0].role, "system") == 0);

    /* pop */
    chat_pop(&c);
    CHECK(c.n == 2 && strcmp(c.msgs[1].content, "buenas") == 0);

    /* clear keeps the system */
    chat_clear(&c);
    CHECK(c.n == 0 && c.system && strcmp(c.system, "se breve") == 0);
    n = chat_window(&c, 40, win, 16);
    CHECK(n == 1 && strcmp(win[0].role, "system") == 0);

    /* remove the system */
    chat_set_system(&c, NULL);
    n = chat_window(&c, 40, win, 16);
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
        n = chat_window(&c, 10, win, 16);
        CHECK(n == 10 && strcmp(win[0].content, "msg 90") == 0);
    }

    chat_free(&c);
    CHECK(c.n == 0 && c.msgs == NULL && c.system == NULL);

    if (fails) {
        fprintf(stderr, "test_chat: %d failures\n", fails);
        return 1;
    }
    puts("test_chat: OK");
    return 0;
}

#include "edit.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fails++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

static void ins(editline *e, const char *s)
{
    el_insert(e, s, strlen(s));
}

#define IS(e, want, p) \
    (strcmp((e).buf.data, want) == 0 && (e).pos == (size_t)(p))

int main(void)
{
    editline e;
    history h;

    el_init(&e);

    /* insertion and cursor */
    ins(&e, "hola");
    CHECK(IS(e, "hola", 4));

    /* home / left / right */
    el_home(&e);
    CHECK(e.pos == 0);
    el_right(&e);
    CHECK(e.pos == 1);
    el_end(&e);
    CHECK(e.pos == 4);

    /* insert in the middle */
    el_home(&e);
    el_right(&e);
    ins(&e, "X");
    CHECK(IS(e, "hXola", 2));

    /* backspace and delete */
    el_backspace(&e);
    CHECK(IS(e, "hola", 1));
    el_delete(&e);
    CHECK(IS(e, "hla", 1));

    /* kill to end / start */
    el_set(&e, "uno dos tres");
    el_home(&e);
    el_right(&e); el_right(&e); el_right(&e); el_right(&e);  /* "uno " */
    el_kill_to_end(&e);
    CHECK(IS(e, "uno ", 4));
    el_kill_to_start(&e);
    CHECK(IS(e, "", 0));

    /* kill prev word */
    el_set(&e, "alfa beta gamma");
    el_kill_prev_word(&e);
    CHECK(IS(e, "alfa beta ", 10));
    el_kill_prev_word(&e);
    CHECK(IS(e, "alfa ", 5));

    /* --- UTF-8: the cursor moves by characters --- */
    el_set(&e, "áéí");          /* 3 chars, 6 bytes */
    CHECK(e.buf.len == 6 && e.pos == 6);
    el_left(&e);
    CHECK(e.pos == 4);           /* skipped the 2 bytes of the last char */
    el_left(&e);
    CHECK(e.pos == 2);
    el_right(&e);
    CHECK(e.pos == 4);

    /* backspace does not corrupt multibyte */
    el_end(&e);
    el_backspace(&e);
    CHECK(strcmp(e.buf.data, "áé") == 0 && e.buf.len == 4);

    /* insert between multibyte */
    el_set(&e, "ñ");             /* 2 bytes */
    el_home(&e);
    ins(&e, "x");
    CHECK(strcmp(e.buf.data, "xñ") == 0 && e.pos == 1);

    el_free(&e);

    /* --- history --- */
    hist_init(&h);
    hist_add(&h, "uno");
    hist_add(&h, "dos");
    hist_add(&h, "dos");        /* consecutive duplicate: ignored */
    hist_add(&h, "");           /* empty: ignored */
    hist_add(&h, "tres");
    CHECK(h.n == 3);
    CHECK(strcmp(h.items[0], "uno") == 0);
    CHECK(strcmp(h.items[2], "tres") == 0);
    hist_free(&h);

    if (fails) {
        fprintf(stderr, "test_edit: %d failures\n", fails);
        return 1;
    }
    puts("test_edit: OK");
    return 0;
}

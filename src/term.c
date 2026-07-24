#include "term.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int term_is_tty(void)
{
    return isatty(0) && isatty(1);
}

static int readline_fgets(const char *prompt, buf_t *out, int tty)
{
    char tmp[512];

    buf_reset(out);
    if (tty) {
        fputs(prompt, stdout);
        fflush(stdout);
    }
    for (;;) {
        if (!fgets(tmp, sizeof tmp, stdin)) {
            if (ferror(stdin) && errno == EINTR) {
                clearerr(stdin);
                return -1;
            }
            return out->len ? 1 : 0; /* EOF */
        }
        {
            size_t n = strlen(tmp);

            if (n && tmp[n - 1] == '\n') {
                buf_append(out, tmp, n - 1);
                return 1;
            }
            buf_append(out, tmp, n); /* long line: keep accumulating */
        }
    }
}

int term_readline(const char *prompt, buf_t *out, history *h)
{
    if (term_is_tty()) {
        int rc = edit_readline(prompt, out, h);

        if (rc != -2)
            return rc;
        /* -2: termios not available; fall back to simple mode */
    }
    return readline_fgets(prompt, out, term_is_tty());
}

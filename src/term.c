#include "term.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int term_is_tty(void)
{
    return isatty(0) && isatty(1);
}

int term_width(void)
{
    const char *cols;

#ifdef TIOCGWINSZ
    {
        struct winsize ws;

        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
            return ws.ws_col;
    }
#endif
    cols = getenv("COLUMNS");
    if (cols && *cols) {
        int n = atoi(cols);

        if (n > 0)
            return n;
    }
    return 80;
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

int term_readline(const char *prompt, buf_t *out, history *h,
                  el_completer comp, void *cuser)
{
    if (term_is_tty()) {
        int rc = edit_readline(prompt, out, h, comp, cuser);

        if (rc != -2)
            return rc;
        /* -2: termios not available; fall back to simple mode */
    }
    return readline_fgets(prompt, out, term_is_tty());
}

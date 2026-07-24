#ifndef PIKI_TERM_H
#define PIKI_TERM_H

#include "buf.h"
#include "edit.h"

int term_is_tty(void);

/* Reads a line from stdin into out (without the '\n'). On a TTY it uses
 * the editor with history (h, may be NULL); if there is no TTY or termios,
 * it falls back to fgets. Returns 1 line read, 0 EOF (Ctrl-D),
 * -1 interrupted (Ctrl-C). */
int term_readline(const char *prompt, buf_t *out, history *h);

#endif /* PIKI_TERM_H */

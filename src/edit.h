#ifndef PIKI_EDIT_H
#define PIKI_EDIT_H

#include <stddef.h>

#include "buf.h"

/* Line editor with history (minimalist readline style) over raw
 * termios. UTF-8: the cursor moves by characters, not by bytes.
 *
 * The pure part (the buffer and its operations) is testable without a
 * terminal; the interactive part lives in edit_readline. */

typedef struct {
    buf_t buf;    /* content, NUL-terminated */
    size_t pos;   /* cursor, byte offset (always on a UTF-8 boundary) */
} editline;

void el_init(editline *e);
void el_free(editline *e);
void el_set(editline *e, const char *s);   /* replaces and goes to the end */

/* Editing operations (pure). They insert/delete at pos. */
void el_insert(editline *e, const char *s, size_t n);
void el_backspace(editline *e);   /* deletes the char before the cursor */
void el_delete(editline *e);      /* deletes the char at the cursor */
void el_left(editline *e);
void el_right(editline *e);
void el_home(editline *e);
void el_end(editline *e);
void el_kill_to_end(editline *e);    /* Ctrl-K */
void el_kill_to_start(editline *e);  /* Ctrl-U */
void el_kill_prev_word(editline *e); /* Ctrl-W */

/* Command history. */
typedef struct {
    char **items;
    size_t n, cap;
} history;

void hist_init(history *h);
void hist_free(history *h);
void hist_add(history *h, const char *line);   /* ignores empty and duplicate */
int  hist_load(history *h, const char *path);
int  hist_save(const history *h, const char *path, size_t max);

/* Reads an interactive line with editing and history navigation.
 * out receives the line (without '\n'). Returns 1 line, 0 EOF (Ctrl-D on
 * empty), -1 interrupted (Ctrl-C). Requires a TTY. */
int edit_readline(const char *prompt, buf_t *out, history *h);

#endif /* PIKI_EDIT_H */

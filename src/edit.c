#include "edit.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* --- UTF-8 utilities -------------------------------------------------- */

static int is_cont(unsigned char c)
{
    return (c & 0xC0) == 0x80;
}

/* char boundary before off */
static size_t utf8_prev(const char *s, size_t off)
{
    if (off == 0)
        return 0;
    off--;
    while (off > 0 && is_cont((unsigned char)s[off]))
        off--;
    return off;
}

/* char boundary after off */
static size_t utf8_next(const char *s, size_t len, size_t off)
{
    if (off >= len)
        return len;
    off++;
    while (off < len && is_cont((unsigned char)s[off]))
        off++;
    return off;
}

/* --- editline (pure operations) -------------------------------------- */

void el_init(editline *e)
{
    buf_init(&e->buf);
    e->pos = 0;
}

void el_free(editline *e)
{
    buf_free(&e->buf);
    e->pos = 0;
}

void el_set(editline *e, const char *s)
{
    buf_reset(&e->buf);
    buf_puts(&e->buf, s);
    e->pos = e->buf.len;
}

void el_insert(editline *e, const char *s, size_t n)
{
    buf_t *b = &e->buf;

    buf_reserve(b, n);
    memmove(b->data + e->pos + n, b->data + e->pos, b->len - e->pos);
    memcpy(b->data + e->pos, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    e->pos += n;
}

static void el_erase(editline *e, size_t from, size_t to)
{
    buf_t *b = &e->buf;

    memmove(b->data + from, b->data + to, b->len - to);
    b->len -= (to - from);
    b->data[b->len] = '\0';
    e->pos = from;
}

void el_backspace(editline *e)
{
    size_t start;

    if (e->pos == 0)
        return;
    start = utf8_prev(e->buf.data, e->pos);
    el_erase(e, start, e->pos);
}

void el_delete(editline *e)
{
    size_t next;

    if (e->pos >= e->buf.len)
        return;
    next = utf8_next(e->buf.data, e->buf.len, e->pos);
    el_erase(e, e->pos, next);   /* the cursor (from) does not move */
}

void el_left(editline *e)
{
    e->pos = utf8_prev(e->buf.data, e->pos);
}

void el_right(editline *e)
{
    e->pos = utf8_next(e->buf.data, e->buf.len, e->pos);
}

void el_home(editline *e)
{
    e->pos = 0;
}

void el_end(editline *e)
{
    e->pos = e->buf.len;
}

void el_kill_to_end(editline *e)
{
    e->buf.len = e->pos;
    e->buf.data[e->pos] = '\0';
}

void el_kill_to_start(editline *e)
{
    if (e->pos > 0)
        el_erase(e, 0, e->pos);
}

void el_kill_prev_word(editline *e)
{
    size_t p = e->pos;
    const char *s = e->buf.data;

    while (p > 0 && s[p - 1] == ' ')
        p--;
    while (p > 0 && s[p - 1] != ' ')
        p--;
    if (p < e->pos)
        el_erase(e, p, e->pos);
}

/* --- history ---------------------------------------------------------- */

void hist_init(history *h)
{
    memset(h, 0, sizeof *h);
}

void hist_free(history *h)
{
    size_t i;

    for (i = 0; i < h->n; i++)
        free(h->items[i]);
    free(h->items);
    memset(h, 0, sizeof *h);
}

void hist_add(history *h, const char *line)
{
    char *copy;

    if (!*line)
        return;
    if (h->n && strcmp(h->items[h->n - 1], line) == 0)
        return; /* do not repeat the last one */
    copy = malloc(strlen(line) + 1);
    if (!copy)
        return;
    strcpy(copy, line);
    if (h->n == h->cap) {
        size_t nc = h->cap ? h->cap * 2 : 32;
        char **ni = realloc(h->items, nc * sizeof *ni);

        if (!ni) {
            free(copy);
            return;
        }
        h->items = ni;
        h->cap = nc;
    }
    h->items[h->n++] = copy;
}

int hist_load(history *h, const char *path)
{
    FILE *f = fopen(path, "r");
    buf_t line;
    int c;

    if (!f)
        return -1;
    buf_init(&line);
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            hist_add(h, line.data ? line.data : "");
            buf_reset(&line);
        } else {
            char ch = (char)c;

            buf_append(&line, &ch, 1);
        }
    }
    if (line.len)
        hist_add(h, line.data);
    buf_free(&line);
    fclose(f);
    return 0;
}

int hist_save(const history *h, const char *path, size_t max)
{
    FILE *f = fopen(path, "w");
    size_t start = 0, i;

    if (!f)
        return -1;
    if (h->n > max)
        start = h->n - max;
    for (i = start; i < h->n; i++)
        fprintf(f, "%s\n", h->items[i]);
    fclose(f);
    return 0;
}

/* --- interactive reading --------------------------------------------- */

/* Redraws prompt + line and repositions the cursor. Reprints the prefix
 * so the column is correct with UTF-8 without counting widths. */
static void redraw(const char *prompt, editline *e)
{
    buf_t o;

    buf_init(&o);
    buf_puts(&o, "\r\033[K");         /* to the start + clear line */
    buf_puts(&o, prompt);
    buf_append(&o, e->buf.data, e->buf.len);
    buf_puts(&o, "\r");              /* back to the start */
    buf_puts(&o, prompt);
    buf_append(&o, e->buf.data, e->pos); /* advance up to the cursor */
    (void)write(STDOUT_FILENO, o.data, o.len);
    buf_free(&o);
}

/* On Tab: complete the whole line against comp's candidates. Single match
 * completes and adds a space; several complete the common prefix and are
 * listed below. Only acts with the cursor at the end of the line. */
static void do_complete(const char *prompt, editline *e,
                        el_completer comp, void *cuser)
{
    char **cands = NULL;
    size_t nc = 0, k;

    if (!comp || e->pos != e->buf.len)
        return;
    comp(e->buf.data ? e->buf.data : "", &cands, &nc, cuser);
    if (nc == 1) {
        el_set(e, cands[0]);
        el_insert(e, " ", 1);
        redraw(prompt, e);
    } else if (nc > 1) {
        size_t lcp = strlen(cands[0]);

        for (k = 1; k < nc; k++) {
            size_t j = 0;

            while (j < lcp && cands[k][j] == cands[0][j])
                j++;
            lcp = j;
        }
        (void)write(STDOUT_FILENO, "\r\n", 2);
        for (k = 0; k < nc; k++) {
            (void)write(STDOUT_FILENO, cands[k], strlen(cands[k]));
            (void)write(STDOUT_FILENO, "  ", 2);
        }
        (void)write(STDOUT_FILENO, "\r\n", 2);
        if (lcp > e->buf.len) {
            char *tmp = malloc(lcp + 1);

            if (tmp) {
                memcpy(tmp, cands[0], lcp);
                tmp[lcp] = '\0';
                el_set(e, tmp);
                free(tmp);
            }
        }
        redraw(prompt, e);
    }
    for (k = 0; k < nc; k++)
        free(cands[k]);
    free(cands);
}

int edit_readline(const char *prompt, buf_t *out, history *h,
                  el_completer comp, void *cuser)
{
    struct termios orig, raw;
    editline e;
    size_t hpos;          /* navigation index; h->n = current line */
    buf_t saved;          /* in-progress line saved when going up in history */
    int ret = 1;

    if (tcgetattr(STDIN_FILENO, &orig) < 0)
        return -2;        /* no termios: the caller falls back to fgets */
    raw = orig;
    raw.c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(unsigned)(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0)
        return -2;

    el_init(&e);
    buf_init(&saved);
    hpos = h ? h->n : 0;
    redraw(prompt, &e);

    for (;;) {
        unsigned char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);

        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            ret = e.buf.len ? 1 : 0;
            break;
        }

        if (c == '\r' || c == '\n') {
            (void)write(STDOUT_FILENO, "\r\n", 2);
            ret = 1;
            break;
        } else if (c == 3) {          /* Ctrl-C */
            (void)write(STDOUT_FILENO, "\r\n", 2);
            ret = -1;
            break;
        } else if (c == 4) {          /* Ctrl-D */
            if (e.buf.len == 0) {
                ret = 0;
                break;
            }
            el_delete(&e);
            redraw(prompt, &e);
        } else if (c == 127 || c == 8) { /* Backspace */
            el_backspace(&e);
            redraw(prompt, &e);
        } else if (c == 1) {          /* Ctrl-A */
            el_home(&e);
            redraw(prompt, &e);
        } else if (c == 5) {          /* Ctrl-E */
            el_end(&e);
            redraw(prompt, &e);
        } else if (c == 11) {         /* Ctrl-K */
            el_kill_to_end(&e);
            redraw(prompt, &e);
        } else if (c == 21) {         /* Ctrl-U */
            el_kill_to_start(&e);
            redraw(prompt, &e);
        } else if (c == 23) {         /* Ctrl-W */
            el_kill_prev_word(&e);
            redraw(prompt, &e);
        } else if (c == 9) {          /* Tab: completion */
            do_complete(prompt, &e, comp, cuser);
        } else if (c == 27) {         /* escape sequence */
            unsigned char seq[2];

            if (read(STDIN_FILENO, &seq[0], 1) != 1)
                continue;
            if (read(STDIN_FILENO, &seq[1], 1) != 1)
                continue;
            if (seq[0] == '[') {
                switch (seq[1]) {
                case 'C': el_right(&e); redraw(prompt, &e); break;
                case 'D': el_left(&e);  redraw(prompt, &e); break;
                case 'H': el_home(&e);  redraw(prompt, &e); break;
                case 'F': el_end(&e);   redraw(prompt, &e); break;
                case 'A': /* up: older history */
                    if (h && hpos > 0) {
                        if (hpos == h->n) {
                            buf_reset(&saved);
                            buf_append(&saved, e.buf.data, e.buf.len);
                        }
                        hpos--;
                        el_set(&e, h->items[hpos]);
                        redraw(prompt, &e);
                    }
                    break;
                case 'B': /* down: newer history */
                    if (h && hpos < h->n) {
                        hpos++;
                        if (hpos == h->n)
                            el_set(&e, saved.data ? saved.data : "");
                        else
                            el_set(&e, h->items[hpos]);
                        redraw(prompt, &e);
                    }
                    break;
                case '3': { /* Delete: ESC [ 3 ~ */
                    unsigned char tilde;

                    if (read(STDIN_FILENO, &tilde, 1) == 1 &&
                        tilde == '~') {
                        el_delete(&e);
                        redraw(prompt, &e);
                    }
                    break;
                }
                default:
                    break;
                }
            }
        } else if (c >= 32) {         /* printable (incl. UTF-8 bytes) */
            char ch = (char)c;

            el_insert(&e, &ch, 1);
            redraw(prompt, &e);
        }
        /* other controls: ignore */
    }

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
    buf_reset(out);
    if (ret == 1)
        buf_append(out, e.buf.data, e.buf.len);
    el_free(&e);
    buf_free(&saved);
    return ret;
}

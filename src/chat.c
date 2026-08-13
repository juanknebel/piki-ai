#include "chat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "json.h"

static void die_oom(void)
{
    fputs("piki: out of memory\n", stderr);
    abort();
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);

    if (!p)
        die_oom();
    memcpy(p, s, n);
    return p;
}

void chat_init(chat_t *c)
{
    memset(c, 0, sizeof *c);
}

void chat_free(chat_t *c)
{
    chat_clear(c);
    free(c->msgs);
    free(c->system);
    memset(c, 0, sizeof *c);
}

void chat_set_system(chat_t *c, const char *s)
{
    free(c->system);
    c->system = s ? xstrdup(s) : NULL;
}

/* Drops the oldest message, freeing it. */
static void evict_oldest(chat_t *c)
{
    size_t len;

    if (!c->n)
        return;
    len = strlen(c->msgs[0].content);
    free((char *)c->msgs[0].content);
    c->bytes -= len;
    c->n--;
    memmove(c->msgs, c->msgs + 1, c->n * sizeof *c->msgs);
}

/* Evicts from the front until under the cap, always keeping the newest. */
static void enforce_cap(chat_t *c)
{
    while (c->max_bytes && c->bytes > c->max_bytes && c->n > 1)
        evict_oldest(c);
}

void chat_set_max_bytes(chat_t *c, size_t max_bytes)
{
    c->max_bytes = max_bytes;
    enforce_cap(c);
}

void chat_add(chat_t *c, const char *role, const char *content)
{
    if (c->n == c->cap) {
        size_t nc = c->cap ? c->cap * 2 : 16;
        chat_msg *nm = realloc(c->msgs, nc * sizeof *nm);

        if (!nm)
            die_oom();
        c->msgs = nm;
        c->cap = nc;
    }
    c->msgs[c->n].role = role;
    c->msgs[c->n].content = xstrdup(content);
    c->bytes += strlen(content);
    c->n++;
    enforce_cap(c);
}

void chat_pop(chat_t *c)
{
    if (c->n) {
        c->n--;
        c->bytes -= strlen(c->msgs[c->n].content);
        free((char *)c->msgs[c->n].content);
    }
}

void chat_clear(chat_t *c)
{
    while (c->n)
        chat_pop(c);
}

void chat_trim(chat_t *c, size_t keep)
{
    while (c->n > keep)
        evict_oldest(c);
}

size_t chat_window(const chat_t *c, size_t max_msgs, size_t max_bytes,
                   chat_msg *out, size_t outcap)
{
    size_t k = c->n, w = 0, start, i, used = 0;

    if (k > max_msgs)
        k = max_msgs;
    if (c->system && w < outcap) {
        out[w].role = "system";
        out[w].content = c->system;
        used += strlen(c->system);
        w++;
    }
    if (k > outcap - w)
        k = outcap - w;

    if (max_bytes) {
        size_t fit = 0;

        /* walk newest-first, keeping what fits; the newest always goes */
        for (i = 0; i < k; i++) {
            size_t len = strlen(c->msgs[c->n - 1 - i].content);

            if (fit && used + len > max_bytes)
                break;
            used += len;
            fit++;
        }
        k = fit;
    }

    start = c->n - k;
    for (i = 0; i < k; i++)
        out[w++] = c->msgs[start + i];
    return w;
}

int chat_save(const chat_t *c, const char *path, char *err, size_t errlen)
{
    buf_t out;
    FILE *f;
    size_t i;
    int ok;

    buf_init(&out);
    buf_puts(&out, "{\"system\":");
    if (c->system)
        json_escape(&out, c->system);
    else
        buf_puts(&out, "null");
    buf_puts(&out, ",\"messages\":[");
    for (i = 0; i < c->n; i++) {
        if (i)
            buf_putc(&out, ',');
        buf_puts(&out, "{\"role\":");
        json_escape(&out, c->msgs[i].role);
        buf_puts(&out, ",\"content\":");
        json_escape(&out, c->msgs[i].content);
        buf_putc(&out, '}');
    }
    buf_puts(&out, "]}\n");

    f = fopen(path, "w");
    if (!f) {
        snprintf(err, errlen, "could not write %s", path);
        buf_free(&out);
        return -1;
    }
    ok = fwrite(out.data, 1, out.len, f) == out.len;
    if (fclose(f) != 0 || !ok) {
        snprintf(err, errlen, "error writing %s", path);
        buf_free(&out);
        return -1;
    }
    buf_free(&out);
    return 0;
}

int chat_load(chat_t *c, const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "r");
    buf_t text;
    char tmp[4096];
    size_t n, i;
    json_doc *doc = NULL;
    json_val *root, *msgs, *sys;
    int ret = -1;

    if (!f) {
        snprintf(err, errlen, "could not open %s", path);
        return -1;
    }
    buf_init(&text);
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0)
        buf_append(&text, tmp, n);
    fclose(f);

    root = json_parse(text.data ? text.data : "", &doc, err, errlen);
    if (!root)
        goto done;
    msgs = json_get(root, "messages");
    if (!msgs || msgs->type != JSON_ARR) {
        snprintf(err, errlen, "%s is not a valid conversation", path);
        goto done;
    }

    chat_clear(c);
    sys = json_get(root, "system");
    chat_set_system(c, sys ? json_str(sys) : NULL);
    for (i = 0; i < msgs->u.arr.n; i++) {
        json_val *m = msgs->u.arr.items[i];
        const char *role = json_str(json_get(m, "role"));
        const char *content = json_str(json_get(m, "content"));
        const char *rlit;

        if (!role || !content)
            continue;
        if (strcmp(role, "user") == 0)
            rlit = "user";
        else if (strcmp(role, "assistant") == 0)
            rlit = "assistant";
        else if (strcmp(role, "system") == 0)
            rlit = "system";
        else
            continue;
        chat_add(c, rlit, content);
    }
    ret = 0;

done:
    json_doc_free(doc);
    buf_free(&text);
    return ret;
}

int chat_export_md(const chat_t *c, const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "w");
    size_t i;

    if (!f) {
        snprintf(err, errlen, "could not open %s", path);
        return -1;
    }
    if (c->system) {
        fprintf(f, "# System\n\n%s\n\n", c->system);
    }
    for (i = 0; i < c->n; i++) {
        const char *role = c->msgs[i].role;
        const char *content = c->msgs[i].content;
        fprintf(f, "## %s\n\n%s\n\n", role, content);
    }
    if (fclose(f) != 0) {
        snprintf(err, errlen, "error writing %s", path);
        return -1;
    }
    return 0;
}

#include "json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 128
#define ARENA_BLOCK    4096

typedef struct arena_block {
    struct arena_block *next;
    size_t used, cap;
    /* the data follows the header */
} arena_block;

struct json_doc {
    arena_block *head;
};

static void die_oom(void)
{
    fputs("piki: out of memory\n", stderr);
    abort();
}

static void *doc_alloc(json_doc *d, size_t n)
{
    size_t align = sizeof(void *);
    arena_block *b = d->head;
    void *p;

    n = (n + align - 1) & ~(align - 1);
    if (!b || b->cap - b->used < n) {
        size_t cap = n > ARENA_BLOCK ? n : ARENA_BLOCK;

        b = malloc(sizeof *b + cap);
        if (!b)
            die_oom();
        b->used = 0;
        b->cap = cap;
        b->next = d->head;
        d->head = b;
    }
    p = (char *)(b + 1) + b->used;
    b->used += n;
    return p;
}

void json_doc_free(json_doc *d)
{
    arena_block *b, *next;

    if (!d)
        return;
    for (b = d->head; b; b = next) {
        next = b->next;
        free(b);
    }
    free(d);
}

/* ------------------------------------------------------------------ */

typedef struct {
    const char *p;
    const char *end;
    const char *start;
    json_doc *doc;
    buf_t scratch;
    int depth;
    char *err;
    size_t errlen;
} parser;

static void perr(parser *ps, const char *msg)
{
    if (ps->err && ps->errlen)
        snprintf(ps->err, ps->errlen, "invalid JSON (byte %ld): %s",
                 (long)(ps->p - ps->start), msg);
}

static void skip_ws(parser *ps)
{
    while (ps->p < ps->end &&
           (*ps->p == ' ' || *ps->p == '\t' ||
            *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static void put_utf8(buf_t *b, unsigned long cp)
{
    char t[4];

    if (cp < 0x80) {
        buf_putc(b, (char)cp);
    } else if (cp < 0x800) {
        t[0] = (char)(0xC0 | (cp >> 6));
        t[1] = (char)(0x80 | (cp & 0x3F));
        buf_append(b, t, 2);
    } else if (cp < 0x10000) {
        t[0] = (char)(0xE0 | (cp >> 12));
        t[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[2] = (char)(0x80 | (cp & 0x3F));
        buf_append(b, t, 3);
    } else {
        t[0] = (char)(0xF0 | (cp >> 18));
        t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        t[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[3] = (char)(0x80 | (cp & 0x3F));
        buf_append(b, t, 4);
    }
}

static int hex4(const char *p, unsigned long *out)
{
    unsigned long v = 0;
    int i;

    for (i = 0; i < 4; i++) {
        char c = p[i];

        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (unsigned long)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (unsigned long)(c - 'A' + 10);
        else
            return -1;
    }
    *out = v;
    return 0;
}

/* Parses a JSON string (with quotes) and leaves it decoded in the arena. */
static int parse_string(parser *ps, const char **out, size_t *outlen)
{
    char *s;

    if (ps->p >= ps->end || *ps->p != '"') {
        perr(ps, "expected string");
        return -1;
    }
    ps->p++;
    buf_reset(&ps->scratch);

    for (;;) {
        char c;

        if (ps->p >= ps->end) {
            perr(ps, "unterminated string");
            return -1;
        }
        c = *ps->p;
        if (c == '"') {
            ps->p++;
            break;
        }
        if (c != '\\') {
            /* raw UTF-8 and control chars pass through as-is (lenient) */
            buf_putc(&ps->scratch, c);
            ps->p++;
            continue;
        }
        if (ps->end - ps->p < 2) {
            perr(ps, "truncated escape");
            return -1;
        }
        switch (ps->p[1]) {
        case '"':  buf_putc(&ps->scratch, '"');  ps->p += 2; break;
        case '\\': buf_putc(&ps->scratch, '\\'); ps->p += 2; break;
        case '/':  buf_putc(&ps->scratch, '/');  ps->p += 2; break;
        case 'b':  buf_putc(&ps->scratch, '\b'); ps->p += 2; break;
        case 'f':  buf_putc(&ps->scratch, '\f'); ps->p += 2; break;
        case 'n':  buf_putc(&ps->scratch, '\n'); ps->p += 2; break;
        case 'r':  buf_putc(&ps->scratch, '\r'); ps->p += 2; break;
        case 't':  buf_putc(&ps->scratch, '\t'); ps->p += 2; break;
        case 'u': {
            unsigned long cp;

            if (ps->end - ps->p < 6 || hex4(ps->p + 2, &cp) < 0) {
                perr(ps, "invalid \\u");
                return -1;
            }
            ps->p += 6;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                unsigned long lo;

                if (ps->end - ps->p >= 6 &&
                    ps->p[0] == '\\' && ps->p[1] == 'u' &&
                    hex4(ps->p + 2, &lo) == 0 &&
                    lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 +
                         ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ps->p += 6;
                } else {
                    cp = 0xFFFD; /* lone surrogate */
                }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                cp = 0xFFFD;
            }
            put_utf8(&ps->scratch, cp);
            break;
        }
        default:
            perr(ps, "unknown escape");
            return -1;
        }
    }

    s = doc_alloc(ps->doc, ps->scratch.len + 1);
    memcpy(s, ps->scratch.data ? ps->scratch.data : "", ps->scratch.len);
    s[ps->scratch.len] = '\0';
    *out = s;
    *outlen = ps->scratch.len;
    return 0;
}

static json_val *parse_value(parser *ps);

static json_val *new_val(parser *ps, json_type t)
{
    json_val *v = doc_alloc(ps->doc, sizeof *v);

    memset(v, 0, sizeof *v);
    v->type = t;
    return v;
}

static void push_ptr(void ***arr, size_t *n, size_t *cap, void *p)
{
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 8;
        void **na = realloc(*arr, nc * sizeof *na);

        if (!na)
            die_oom();
        *arr = na;
        *cap = nc;
    }
    (*arr)[(*n)++] = p;
}

static json_val *parse_array(parser *ps)
{
    json_val *v = new_val(ps, JSON_ARR);
    void **items = NULL;
    size_t n = 0, cap = 0;

    ps->p++; /* '[' */
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') {
        ps->p++;
        return v;
    }
    for (;;) {
        json_val *item = parse_value(ps);

        if (!item) {
            free(items);
            return NULL;
        }
        push_ptr(&items, &n, &cap, item);
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            skip_ws(ps);
            continue;
        }
        if (ps->p < ps->end && *ps->p == ']') {
            ps->p++;
            break;
        }
        perr(ps, "expected ',' or ']'");
        free(items);
        return NULL;
    }
    v->u.arr.items = doc_alloc(ps->doc, n * sizeof(json_val *));
    memcpy(v->u.arr.items, items, n * sizeof(json_val *));
    v->u.arr.n = n;
    free(items);
    return v;
}

static json_val *parse_object(parser *ps)
{
    json_val *v = new_val(ps, JSON_OBJ);
    void **keys = NULL, **vals = NULL;
    size_t n = 0, kcap = 0, vcap = 0, vn = 0;

    ps->p++; /* '{' */
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') {
        ps->p++;
        return v;
    }
    for (;;) {
        const char *key;
        size_t keylen;
        json_val *val;

        skip_ws(ps);
        if (parse_string(ps, &key, &keylen) < 0)
            goto fail;
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') {
            perr(ps, "expected ':'");
            goto fail;
        }
        ps->p++;
        val = parse_value(ps);
        if (!val)
            goto fail;
        push_ptr(&keys, &n, &kcap, (void *)key);
        push_ptr(&vals, &vn, &vcap, val);
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == '}') {
            ps->p++;
            break;
        }
        perr(ps, "expected ',' or '}'");
        goto fail;
    }
    v->u.obj.keys = doc_alloc(ps->doc, n * sizeof(char *));
    v->u.obj.vals = doc_alloc(ps->doc, n * sizeof(json_val *));
    memcpy(v->u.obj.keys, keys, n * sizeof(char *));
    memcpy(v->u.obj.vals, vals, n * sizeof(json_val *));
    v->u.obj.n = n;
    free(keys);
    free(vals);
    return v;

fail:
    free(keys);
    free(vals);
    return NULL;
}

static json_val *parse_value(parser *ps)
{
    json_val *v;
    char c;

    if (++ps->depth > JSON_MAX_DEPTH) {
        perr(ps, "excessive nesting");
        return NULL;
    }
    skip_ws(ps);
    if (ps->p >= ps->end) {
        perr(ps, "expected a value");
        ps->depth--;
        return NULL;
    }
    c = *ps->p;
    if (c == '{') {
        v = parse_object(ps);
    } else if (c == '[') {
        v = parse_array(ps);
    } else if (c == '"') {
        const char *s;
        size_t slen;

        if (parse_string(ps, &s, &slen) < 0) {
            v = NULL;
        } else {
            v = new_val(ps, JSON_STR);
            v->u.str.ptr = s;
            v->u.str.len = slen;
        }
    } else if (c == 't' && ps->end - ps->p >= 4 &&
               memcmp(ps->p, "true", 4) == 0) {
        v = new_val(ps, JSON_BOOL);
        v->u.b = 1;
        ps->p += 4;
    } else if (c == 'f' && ps->end - ps->p >= 5 &&
               memcmp(ps->p, "false", 5) == 0) {
        v = new_val(ps, JSON_BOOL);
        v->u.b = 0;
        ps->p += 5;
    } else if (c == 'n' && ps->end - ps->p >= 4 &&
               memcmp(ps->p, "null", 4) == 0) {
        v = new_val(ps, JSON_NULL);
        ps->p += 4;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        char *numend;
        double d = strtod(ps->p, &numend);

        if (numend == ps->p) {
            perr(ps, "invalid number");
            v = NULL;
        } else {
            v = new_val(ps, JSON_NUM);
            v->u.num = d;
            ps->p = numend;
        }
    } else {
        perr(ps, "unexpected character");
        v = NULL;
    }
    ps->depth--;
    return v;
}

json_val *json_parse(const char *text, json_doc **docp,
                     char *err, size_t errlen)
{
    parser ps;
    json_val *v;

    *docp = NULL;
    memset(&ps, 0, sizeof ps);
    ps.p = ps.start = text;
    ps.end = text + strlen(text);
    ps.err = err;
    ps.errlen = errlen;
    ps.doc = calloc(1, sizeof *ps.doc);
    if (!ps.doc)
        die_oom();
    buf_init(&ps.scratch);

    v = parse_value(&ps);
    if (v) {
        skip_ws(&ps);
        if (ps.p != ps.end) {
            perr(&ps, "extra content at the end");
            v = NULL;
        }
    }
    buf_free(&ps.scratch);
    if (!v) {
        json_doc_free(ps.doc);
        return NULL;
    }
    *docp = ps.doc;
    return v;
}

/* ------------------------------------------------------------------ */

json_val *json_get(const json_val *v, const char *path)
{
    while (v && path && *path) {
        const char *dot = strchr(path, '.');
        size_t seglen = dot ? (size_t)(dot - path) : strlen(path);
        const json_val *next = NULL;

        if (seglen == 0)
            return NULL;
        if (v->type == JSON_OBJ) {
            size_t i;

            for (i = 0; i < v->u.obj.n; i++) {
                const char *k = v->u.obj.keys[i];

                if (strlen(k) == seglen &&
                    memcmp(k, path, seglen) == 0) {
                    next = v->u.obj.vals[i];
                    break;
                }
            }
        } else if (v->type == JSON_ARR) {
            size_t idx = 0, i;
            int digits = 1;

            for (i = 0; i < seglen; i++) {
                if (path[i] < '0' || path[i] > '9') {
                    digits = 0;
                    break;
                }
                idx = idx * 10 + (size_t)(path[i] - '0');
            }
            if (digits && idx < v->u.arr.n)
                next = v->u.arr.items[idx];
        }
        v = next;
        path = dot ? dot + 1 : path + seglen;
    }
    return (json_val *)v;
}

const char *json_str(const json_val *v)
{
    return (v && v->type == JSON_STR) ? v->u.str.ptr : NULL;
}

double json_num(const json_val *v, double dflt)
{
    return (v && v->type == JSON_NUM) ? v->u.num : dflt;
}

void json_escape(buf_t *out, const char *s)
{
    buf_putc(out, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;

        switch (c) {
        case '"':  buf_puts(out, "\\\""); break;
        case '\\': buf_puts(out, "\\\\"); break;
        case '\n': buf_puts(out, "\\n");  break;
        case '\r': buf_puts(out, "\\r");  break;
        case '\t': buf_puts(out, "\\t");  break;
        case '\b': buf_puts(out, "\\b");  break;
        case '\f': buf_puts(out, "\\f");  break;
        default:
            if (c < 0x20)
                buf_printf(out, "\\u%04x", c);
            else
                buf_putc(out, (char)c);
        }
    }
    buf_putc(out, '"');
}

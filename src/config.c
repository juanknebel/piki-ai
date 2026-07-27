#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"

enum { SECT_NONE, SECT_DEFAULTS, SECT_PROVIDER, SECT_UNKNOWN };

void config_defaults(config_t *c)
{
    memset(c, 0, sizeof *c);
    c->max_history = 40;
    c->max_memory = 256;          /* KB */
    c->max_context_tokens = 8000;
}

static char *trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = '\0';
    return s;
}

static int copystr(char *dst, size_t cap, const char *src)
{
    if (strlen(src) >= cap)
        return -1;
    strcpy(dst, src);
    return 0;
}

/* Parses a strictly positive integer. 0 ok, -1 invalid. */
static int parse_positive(const char *val, long *out)
{
    char *end;
    long v = strtol(val, &end, 10);

    if (end == val || *end || v < 1)
        return -1;
    *out = v;
    return 0;
}

static void seterr(char *err, size_t errlen, const char *msg, int lineno)
{
    if (err && errlen)
        snprintf(err, errlen, "%s (line %d)", msg, lineno);
}

int config_parse(config_t *c, const char *text, char *err, size_t errlen)
{
    const char *p = text;
    int lineno = 0;
    int sect = SECT_NONE;
    cfg_provider *prov = NULL;

    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[512];
        char *s, *eq, *key, *val;

        lineno++;
        if (len >= sizeof line) {
            seterr(err, errlen, "line too long", lineno);
            return -1;
        }
        memcpy(line, p, len);
        line[len] = '\0';
        p = nl ? nl + 1 : p + len;

        s = trim(line);
        if (!*s || *s == '#' || *s == ';')
            continue;

        if (*s == '[') {
            char *rb = strchr(s, ']');

            if (!rb || *trim(rb + 1)) {
                seterr(err, errlen, "invalid section", lineno);
                return -1;
            }
            *rb = '\0';
            s = trim(s + 1);
            prov = NULL;
            if (strcmp(s, "defaults") == 0) {
                sect = SECT_DEFAULTS;
            } else if (strncmp(s, "provider", 8) == 0) {
                char *q1 = strchr(s, '"');
                char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;

                if (!q1 || !q2 || q2 == q1 + 1) {
                    seterr(err, errlen,
                           "provider without a quoted name",
                           lineno);
                    return -1;
                }
                *q2 = '\0';
                if (c->nproviders >= CFG_MAX_PROVIDERS) {
                    seterr(err, errlen, "too many providers",
                           lineno);
                    return -1;
                }
                prov = &c->providers[c->nproviders];
                memset(prov, 0, sizeof *prov);
                if (copystr(prov->name, sizeof prov->name,
                            q1 + 1) < 0) {
                    seterr(err, errlen,
                           "provider name too long",
                           lineno);
                    return -1;
                }
                c->nproviders++;
                sect = SECT_PROVIDER;
            } else {
                sect = SECT_UNKNOWN; /* ignore content */
            }
            continue;
        }

        eq = strchr(s, '=');
        if (!eq) {
            seterr(err, errlen, "expected key = value", lineno);
            return -1;
        }
        *eq = '\0';
        key = trim(s);
        val = trim(eq + 1);

        if (sect == SECT_DEFAULTS) {
            int bad = 0;

            if (strcmp(key, "provider") == 0)
                bad = copystr(c->default_provider,
                              sizeof c->default_provider, val);
            else if (strcmp(key, "model") == 0)
                bad = copystr(c->model, sizeof c->model, val);
            else if (strcmp(key, "system") == 0)
                bad = copystr(c->system, sizeof c->system, val);
            else if (strcmp(key, "max_history") == 0) {
                if (parse_positive(val, &c->max_history) < 0) {
                    seterr(err, errlen, "invalid max_history", lineno);
                    return -1;
                }
            } else if (strcmp(key, "max_memory") == 0) {
                if (parse_positive(val, &c->max_memory) < 0) {
                    seterr(err, errlen, "invalid max_memory", lineno);
                    return -1;
                }
            } else if (strcmp(key, "max_context_tokens") == 0) {
                if (parse_positive(val, &c->max_context_tokens) < 0) {
                    seterr(err, errlen, "invalid max_context_tokens",
                           lineno);
                    return -1;
                }
            }
            /* unknown key: ignore */
            if (bad) {
                seterr(err, errlen, "value too long", lineno);
                return -1;
            }
        } else if (sect == SECT_PROVIDER && prov) {
            int bad = 0;

            if (strcmp(key, "url") == 0)
                bad = copystr(prov->url, sizeof prov->url, val);
            else if (strcmp(key, "key") == 0)
                bad = copystr(prov->key, sizeof prov->key, val);
            if (bad) {
                seterr(err, errlen, "value too long", lineno);
                return -1;
            }
        } else if (sect == SECT_NONE) {
            seterr(err, errlen, "key outside a section", lineno);
            return -1;
        }
        /* SECT_UNKNOWN: ignore */
    }
    return 0;
}

int config_load(config_t *c, char *err, size_t errlen)
{
    char path[512];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    FILE *f;
    buf_t text;
    char tmp[4096];
    size_t n;
    int rc;

    if (xdg && *xdg)
        snprintf(path, sizeof path, "%s/piki/config", xdg);
    else if (home && *home)
        snprintf(path, sizeof path, "%s/.config/piki/config", home);
    else
        return 1;

    f = fopen(path, "r");
    if (!f)
        return 1;
    buf_init(&text);
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0)
        buf_append(&text, tmp, n);
    fclose(f);
    rc = config_parse(c, text.data ? text.data : "", err, errlen);
    buf_free(&text);
    return rc;
}

cfg_provider *config_provider(config_t *c, const char *name)
{
    size_t i;

    for (i = 0; i < c->nproviders; i++)
        if (strcmp(c->providers[i].name, name) == 0)
            return &c->providers[i];
    return NULL;
}

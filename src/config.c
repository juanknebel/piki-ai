#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "buf.h"

enum { SECT_NONE, SECT_DEFAULTS, SECT_PROVIDER, SECT_SEARCH,
       SECT_UNKNOWN };

void config_defaults(config_t *c)
{
    memset(c, 0, sizeof *c);
    c->max_history = 40;
    c->max_memory = 256;          /* KB */
    c->max_context_tokens = 8000;
    c->max_agent_steps = 12;
    c->check_updates = 1;
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
            } else if (strcmp(s, "search") == 0) {
                sect = SECT_SEARCH;
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
            } else if (strcmp(key, "max_agent_steps") == 0) {
                if (parse_positive(val, &c->max_agent_steps) < 0) {
                    seterr(err, errlen, "invalid max_agent_steps", lineno);
                    return -1;
                }
            } else if (strcmp(key, "check_updates") == 0) {
                if (strcmp(val, "0") == 0) {
                    c->check_updates = 0;
                } else if (strcmp(val, "1") == 0) {
                    c->check_updates = 1;
                } else {
                    seterr(err, errlen, "invalid check_updates", lineno);
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
            else if (strcmp(key, "model") == 0)
                bad = copystr(prov->model, sizeof prov->model, val);
            else if (strcmp(key, "web_search") == 0) {
                if (strcmp(val, "none") != 0 &&
                    strcmp(val, "plugin") != 0 &&
                    strcmp(val, "responses") != 0 &&
                    strcmp(val, "local") != 0) {
                    seterr(err, errlen, "invalid web_search", lineno);
                    return -1;
                }
                bad = copystr(prov->web_search,
                              sizeof prov->web_search, val);
            }
            if (bad) {
                seterr(err, errlen, "value too long", lineno);
                return -1;
            }
        } else if (sect == SECT_SEARCH) {
            int bad = 0;

            if (strcmp(key, "engine") == 0) {
                if (strcmp(val, "ddg") != 0 &&
                    strcmp(val, "brave") != 0) {
                    seterr(err, errlen, "invalid engine", lineno);
                    return -1;
                }
                bad = copystr(c->search_engine,
                              sizeof c->search_engine, val);
            } else if (strcmp(key, "key") == 0) {
                bad = copystr(c->search_key, sizeof c->search_key, val);
            }
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

static void config_path_for_save(char *out, size_t cap)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");

    if (xdg && *xdg)
        snprintf(out, cap, "%s/piki/config", xdg);
    else if (home && *home)
        snprintf(out, cap, "%s/.config/piki/config", home);
    else
        out[0] = '\0';
}

static int ensure_dir_for_file(const char *path, char *err, size_t errlen)
{
    char dir[512];
    const char *slash = strrchr(path, '/');
    size_t dlen;

    if (!slash)
        return 0;
    dlen = (size_t)(slash - path);
    if (dlen >= sizeof dir) {
        if (err && errlen)
            snprintf(err, errlen, "path too long");
        return -1;
    }
    memcpy(dir, path, dlen);
    dir[dlen] = '\0';

    /* mkdir -p */
    {
        char tmp[512];
        size_t i;

        if (strlen(dir) >= sizeof tmp) {
            if (err && errlen)
                snprintf(err, errlen, "path too long");
            return -1;
        }
        strcpy(tmp, dir);
        for (i = 1; i < strlen(tmp); i++) {
            if (tmp[i] == '/') {
                tmp[i] = '\0';
                mkdir(tmp, 0700);
                tmp[i] = '/';
            }
        }
        if (mkdir(tmp, 0700) < 0 && errno != EEXIST) {
            if (err && errlen)
                snprintf(err, errlen, "could not create %s: %s", tmp,
                         strerror(errno));
            return -1;
        }
    }
    return 0;
}

/* Does the section header line (already trimmed, without brackets) name
 * the provider we are looking for? */
static int is_provider_section(char *s, const char *provider)
{
    char *q1, *q2;

    if (strncmp(s, "provider", 8) != 0)
        return 0;
    q1 = strchr(s, '"');
    q2 = q1 ? strchr(q1 + 1, '"') : NULL;
    if (!q1 || !q2)
        return 0;
    *q2 = '\0';
    return strcmp(q1 + 1, provider) == 0;
}

/* Inserts "model = <model>\n" into out at position at (so the line lands
 * right after the section's last key, before any trailing blank lines). */
static void insert_model_line(buf_t *out, size_t at, const char *model)
{
    buf_t tail;

    buf_init(&tail);
    buf_append(&tail, out->data + at, out->len - at);
    out->len = at;
    buf_printf(out, "model = %s\n", model);
    buf_append(out, tail.data, tail.len);
    buf_free(&tail);
}

int config_save_model(const char *provider, const char *model,
                      char *err, size_t errlen)
{
    char path[512];
    char tmppath[520];
    FILE *f;
    buf_t old;
    buf_t out;
    const char *p;
    size_t insert_at = 0;  /* end of the target section's last key line */
    int in_target = 0;
    int found_target = 0;
    int found_model = 0;

    if (!provider || !*provider) {
        if (err && errlen)
            snprintf(err, errlen, "no provider section to save the model "
                     "in (PIKI_BASE_URL?); set it in the config");
        return -1;
    }
    if (!model || !*model) {
        if (err && errlen)
            snprintf(err, errlen, "model is empty");
        return -1;
    }
    if (strlen(model) >= sizeof(((cfg_provider *)0)->model)) {
        if (err && errlen)
            snprintf(err, errlen, "model name too long");
        return -1;
    }

    config_path_for_save(path, sizeof path);
    if (!path[0]) {
        if (err && errlen)
            snprintf(err, errlen, "no config directory");
        return -1;
    }
    if (ensure_dir_for_file(path, err, errlen) < 0)
        return -1;

    buf_init(&old);
    f = fopen(path, "r");
    if (f) {
        char tmp[4096];
        size_t n;

        while ((n = fread(tmp, 1, sizeof tmp, f)) > 0)
            buf_append(&old, tmp, n);
        fclose(f);
    }

    buf_init(&out);

    if (!old.data || !old.len) {
        if (strcmp(provider, "openrouter") != 0) {
            if (err && errlen)
                snprintf(err, errlen, "provider %s is not in the config",
                         provider);
            buf_free(&old);
            buf_free(&out);
            return -1;
        }
        buf_printf(&out, "[provider \"openrouter\"]\n"
                   "url = https://openrouter.ai/api/v1\n"
                   "model = %s\n", model);
        goto write;
    }

    /* Ensure trailing newline for easier parsing. */
    if (old.len && old.data[old.len - 1] != '\n')
        buf_putc(&old, '\n');

    p = old.data;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[512];
        char copy[512];
        char *s;

        if (len >= sizeof line) {
            if (err && errlen)
                snprintf(err, errlen, "line too long");
            buf_free(&old);
            buf_free(&out);
            return -1;
        }
        memcpy(line, p, len);
        line[len] = '\0';
        memcpy(copy, line, len + 1);
        p = nl ? nl + 1 : p + len;

        s = trim(copy);
        if (*s == '[') {
            /* leaving the target section without a model line: insert it */
            if (in_target && !found_model) {
                insert_model_line(&out, insert_at, model);
                found_model = 1;
            }
            {
                char *rb = strchr(s, ']');
                int is_target = 0;

                if (rb) {
                    *rb = '\0';
                    s = trim(s + 1);
                    is_target = is_provider_section(s, provider);
                }
                in_target = is_target;
                if (is_target)
                    found_target = 1;
            }
            buf_append(&out, line, len);
            buf_putc(&out, '\n');
            insert_at = out.len;
            continue;
        }

        if (in_target && *s && *s != '#' && *s != ';') {
            char *eq = strchr(s, '=');
            if (eq) {
                char *key;

                *eq = '\0';
                key = trim(s);
                if (strcmp(key, "model") == 0) {
                    buf_printf(&out, "model = %s\n", model);
                    found_model = 1;
                    continue;
                }
            }
        }
        buf_append(&out, line, len);
        buf_putc(&out, '\n');
        if (in_target && *s)        /* last non-blank line of the section */
            insert_at = out.len;
    }

    if (!found_target) {
        if (strcmp(provider, "openrouter") != 0) {
            if (err && errlen)
                snprintf(err, errlen, "provider %s is not in the config",
                         provider);
            buf_free(&old);
            buf_free(&out);
            return -1;
        }
        if (out.len && out.data[out.len - 1] != '\n')
            buf_putc(&out, '\n');
        buf_printf(&out, "[provider \"openrouter\"]\n"
                   "url = https://openrouter.ai/api/v1\n"
                   "model = %s\n", model);
    } else if (!found_model) {
        /* target section runs to EOF */
        insert_model_line(&out, insert_at, model);
    }

write:
    buf_free(&old);
    snprintf(tmppath, sizeof tmppath, "%s.tmp", path);
    f = fopen(tmppath, "w");
    if (!f) {
        if (err && errlen)
            snprintf(err, errlen, "could not write %s: %s", tmppath,
                     strerror(errno));
        buf_free(&out);
        return -1;
    }
    if (out.len && fwrite(out.data, 1, out.len, f) != out.len) {
        if (err && errlen)
            snprintf(err, errlen, "write failed: %s", strerror(errno));
        fclose(f);
        unlink(tmppath);
        buf_free(&out);
        return -1;
    }
    if (fclose(f) != 0) {
        if (err && errlen)
            snprintf(err, errlen, "write failed: %s", strerror(errno));
        unlink(tmppath);
        buf_free(&out);
        return -1;
    }
    if (rename(tmppath, path) != 0) {
        if (err && errlen)
            snprintf(err, errlen, "could not save %s: %s", path,
                     strerror(errno));
        unlink(tmppath);
        buf_free(&out);
        return -1;
    }
    buf_free(&out);
    return 0;
}

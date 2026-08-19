#include "web.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==================================================================== */
/* pure parts                                                           */
/* ==================================================================== */

int web_url_split(const char *url, char *host, size_t hostcap, int *port,
                  int *use_tls, char *path, size_t pathcap)
{
    const char *p, *slash, *colon;
    size_t hlen;

    if (strncmp(url, "https://", 8) == 0) {
        *use_tls = 1;
        *port = 443;
        p = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        *use_tls = 0;
        *port = 80;
        p = url + 7;
    } else {
        return -1;
    }

    slash = strchr(p, '/');
    hlen = slash ? (size_t)(slash - p) : strlen(p);
    if (!hlen || hlen >= hostcap)
        return -1;
    memcpy(host, p, hlen);
    host[hlen] = '\0';

    colon = strchr(host, ':');
    if (colon) {
        int prt = atoi(colon + 1);

        if (prt < 1 || prt > 65535)
            return -1;
        *port = prt;
        host[colon - host] = '\0';
        if (!host[0])
            return -1;
    }
    /* a host with spaces or control bytes is never valid */
    for (p = host; *p; p++)
        if ((unsigned char)*p <= ' ')
            return -1;

    if (!slash || !slash[0]) {
        if (pathcap < 2)
            return -1;
        strcpy(path, "/");
        return 0;
    }
    if (strlen(slash) >= pathcap)
        return -1;
    strcpy(path, slash);
    return 0;
}

void web_percent_encode(const char *s, buf_t *out)
{
    static const char hex[] = "0123456789ABCDEF";

    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;

        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            buf_putc(out, (char)c);
        } else {
            buf_putc(out, '%');
            buf_putc(out, hex[c >> 4]);
            buf_putc(out, hex[c & 0x0f]);
        }
    }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

void web_percent_decode(const char *s, size_t n, buf_t *out)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n) {
            int hi = hexval(s[i + 1]), lo = hexval(s[i + 2]);

            if (hi >= 0 && lo >= 0) {
                buf_putc(out, (char)(hi << 4 | lo));
                i += 2;
                continue;
            }
        }
        buf_putc(out, s[i]);
    }
}

/* --- html-to-text stripper ------------------------------------------ */

enum { HS_TEXT, HS_TAGNAME, HS_TAGATTRS, HS_BANG, HS_COMMENT, HS_ENT };

void htmlstrip_init(htmlstrip *h)
{
    memset(h, 0, sizeof *h);
    h->state = HS_TEXT;
}

static int tag_is_block(const char *t)
{
    static const char *const blocks[] = {
        "p", "br", "hr", "li", "tr", "div", "ul", "ol", "table",
        "blockquote", "pre", NULL
    };
    size_t i;

    if (t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && !t[2])
        return 1;
    for (i = 0; blocks[i]; i++)
        if (strcmp(t, blocks[i]) == 0)
            return 1;
    return 0;
}

/* Emits one visible character, flushing pending separators first. */
static void put_visible(htmlstrip *h, char c, buf_t *out)
{
    if (h->skip[0])
        return;
    if (h->pending_nl) {
        if (h->got_any)
            buf_putc(out, '\n');
    } else if (h->pending_space && h->got_any) {
        buf_putc(out, ' ');
    }
    h->pending_nl = h->pending_space = 0;
    buf_putc(out, c);
    h->got_any = 1;
}

static void text_char(htmlstrip *h, char c, buf_t *out)
{
    if (h->skip[0])
        return;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        h->pending_space = 1;
        return;
    }
    put_visible(h, c, out);
}

/* Decodes the accumulated entity (h->ent, no '&', terminated by ';'),
 * or emits it raw including the ';'. */
static void flush_entity(htmlstrip *h, buf_t *out)
{
    static const struct { const char *name; char c; } ents[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'},
        {"apos", '\''}, {"nbsp", ' '},
    };
    size_t i;

    h->ent[h->entlen] = '\0';
    for (i = 0; i < sizeof ents / sizeof ents[0]; i++) {
        if (strcmp(h->ent, ents[i].name) == 0) {
            text_char(h, ents[i].c, out);
            return;
        }
    }
    if (h->ent[0] == '#') {
        long v = h->ent[1] == 'x' || h->ent[1] == 'X'
                     ? strtol(h->ent + 2, NULL, 16)
                     : strtol(h->ent + 1, NULL, 10);

        if (v >= 32 && v < 127) {
            text_char(h, (char)v, out);
            return;
        }
    }
    /* unknown: emit it literally */
    put_visible(h, '&', out);
    for (i = 0; i < h->entlen; i++)
        put_visible(h, h->ent[i], out);
    put_visible(h, ';', out);
}

/* A '<' was closed: act on the collected tag name. */
static void end_tag(htmlstrip *h)
{
    h->tag[h->taglen] = '\0';
    if (h->skip[0]) {
        /* inside a skipped body only the matching close tag matters */
        if (h->closing && strcmp(h->tag, h->skip) == 0)
            h->skip[0] = '\0';
        h->state = HS_TEXT;
        return;
    }
    if (!h->closing && (strcmp(h->tag, "script") == 0 ||
                        strcmp(h->tag, "style") == 0))
        strcpy(h->skip, h->tag);
    if (tag_is_block(h->tag))
        h->pending_nl = 1;
    else if (strcmp(h->tag, "td") == 0 || strcmp(h->tag, "th") == 0)
        h->pending_space = 1;
    h->state = HS_TEXT;
}

void htmlstrip_feed(htmlstrip *h, const char *p, size_t n, buf_t *out)
{
    size_t i;

    for (i = 0; i < n; i++) {
        char c = p[i];

        switch (h->state) {
        case HS_TEXT:
            if (c == '<') {
                h->state = HS_TAGNAME;
                h->taglen = 0;
                h->closing = 0;
                h->quote = 0;
            } else if (c == '&' && !h->skip[0]) {
                h->state = HS_ENT;
                h->entlen = 0;
            } else {
                text_char(h, c, out);
            }
            break;
        case HS_TAGNAME:
            if (c == '/' && h->taglen == 0 && !h->closing) {
                h->closing = 1;
            } else if (c == '!' && h->taglen == 0 && !h->closing) {
                h->state = HS_BANG;
                h->taglen = 0;   /* counts leading dashes */
            } else if (isalnum((unsigned char)c)) {
                if (h->taglen < sizeof h->tag - 1)
                    h->tag[h->taglen++] = (char)tolower((unsigned char)c);
            } else if (c == '>') {
                end_tag(h);
            } else {
                h->state = HS_TAGATTRS;
            }
            break;
        case HS_TAGATTRS:
            if (h->quote) {
                if (c == h->quote)
                    h->quote = 0;
            } else if (c == '"' || c == '\'') {
                h->quote = c;
            } else if (c == '>') {
                end_tag(h);
            }
            break;
        case HS_BANG:
            /* <!-- comment --> vs <!DOCTYPE ...> */
            if (c == '-' && h->taglen < 2) {
                if (++h->taglen == 2) {
                    h->state = HS_COMMENT;
                    h->taglen = 0;   /* now counts closing dashes */
                }
            } else if (c == '>') {
                h->state = HS_TEXT;
            } else {
                h->state = HS_TAGATTRS;
            }
            break;
        case HS_COMMENT:
            if (c == '-') {
                if (h->taglen < 2)
                    h->taglen++;
            } else if (c == '>' && h->taglen >= 2) {
                h->state = HS_TEXT;
            } else {
                h->taglen = 0;
            }
            break;
        case HS_ENT:
            if (c == ';') {
                flush_entity(h, out);
                h->state = HS_TEXT;
            } else if (h->entlen < sizeof h->ent - 1 &&
                       (isalnum((unsigned char)c) || c == '#')) {
                h->ent[h->entlen++] = c;
            } else {
                /* not an entity after all: emit raw and reprocess c */
                size_t j;

                put_visible(h, '&', out);
                for (j = 0; j < h->entlen; j++)
                    put_visible(h, h->ent[j], out);
                h->state = HS_TEXT;
                i--;   /* run c through HS_TEXT */
            }
            break;
        }
    }
}

void htmlstrip_finish(htmlstrip *h, buf_t *out)
{
    if (h->state == HS_ENT) {
        size_t j;

        put_visible(h, '&', out);
        for (j = 0; j < h->entlen; j++)
            put_visible(h, h->ent[j], out);
    }
    h->state = HS_TEXT;
    (void)out;
}

/* --- DuckDuckGo Lite results parser --------------------------------- */

/* Case-insensitive memmem. */
static const char *ci_find(const char *hay, size_t n, const char *needle)
{
    size_t nl = strlen(needle), i;

    if (!nl || n < nl)
        return NULL;
    for (i = 0; i + nl <= n; i++) {
        size_t j;

        for (j = 0; j < nl; j++)
            if (tolower((unsigned char)hay[i + j]) !=
                tolower((unsigned char)needle[j]))
                break;
        if (j == nl)
            return hay + i;
    }
    return NULL;
}

/* Strips a slice of HTML to trimmed text appended to out (capped). */
static void strip_slice(const char *p, size_t n, size_t cap, buf_t *out)
{
    htmlstrip h;
    buf_t tmp;
    size_t len, start = 0;

    htmlstrip_init(&h);
    buf_init(&tmp);
    htmlstrip_feed(&h, p, n, &tmp);
    htmlstrip_finish(&h, &tmp);
    len = tmp.len;
    while (start < len && (tmp.data[start] == ' ' || tmp.data[start] == '\n'))
        start++;
    while (len > start &&
           (tmp.data[len - 1] == ' ' || tmp.data[len - 1] == '\n'))
        len--;
    if (len - start > cap)
        len = start + cap;
    if (len > start)
        buf_append(out, tmp.data + start, len - start);
    buf_free(&tmp);
}

/* Extracts the href value of the <a ...> tag starting at a (which points
 * at "<a"); href and hlen get the raw value, returns the position right
 * after the closing '>' of the tag, or NULL. */
static const char *anchor_href(const char *a, const char *end,
                               const char **href, size_t *hlen)
{
    const char *gt, *hv;

    gt = memchr(a, '>', (size_t)(end - a));
    if (!gt)
        return NULL;
    *href = NULL;
    *hlen = 0;
    hv = ci_find(a, (size_t)(gt - a), "href=");
    if (hv) {
        hv += 5;
        if (hv < gt && (*hv == '"' || *hv == '\'')) {
            char q = *hv++;
            const char *e = memchr(hv, q, (size_t)(gt - hv));

            if (e) {
                *href = hv;
                *hlen = (size_t)(e - hv);
            }
        }
    }
    return gt + 1;
}

int web_ddg_parse(const char *html, size_t n, int max_results, buf_t *out)
{
    const char *p = html, *end = html + n;
    int count = 0;

    while (count < max_results && p < end) {
        const char *a = ci_find(p, (size_t)(end - p), "<a");
        const char *body, *aend, *next, *href;
        size_t hlen;
        buf_t url;

        if (!a)
            break;
        /* "<a" must be a real tag ("<a " or "<a\n" etc.) */
        if (a + 2 >= end ||
            (!isspace((unsigned char)a[2]) && a[2] != '>')) {
            p = a + 2;
            continue;
        }
        body = anchor_href(a, end, &href, &hlen);
        if (!body)
            break;
        aend = ci_find(body, (size_t)(end - body), "</a");
        if (!aend) {
            p = body;
            continue;
        }

        /* classify the href: unwrap DDG redirects, keep absolute URLs,
         * skip everything else (navigation, pagination) */
        buf_init(&url);
        if (href && hlen) {
            const char *u = NULL;
            size_t un = 0;

            if ((hlen > 20 &&
                 ci_find(href, hlen > 40 ? 40 : hlen, "duckduckgo.com/l/?"))) {
                const char *ud = ci_find(href, hlen, "uddg=");

                if (ud) {
                    ud += 5;
                    un = (size_t)(href + hlen - ud);
                    {
                        const char *amp = memchr(ud, '&', un);

                        if (amp)
                            un = (size_t)(amp - ud);
                    }
                    u = ud;
                }
                if (u)
                    web_percent_decode(u, un, &url);
            } else if (hlen > 8 &&
                       (strncmp(href, "https://", 8) == 0 ||
                        strncmp(href, "http://", 7) == 0)) {
                buf_append(&url, href, hlen);
            }
        }
        if (!url.len ||
            ci_find(url.data, url.len, "duckduckgo.com")) {
            buf_free(&url);
            p = aend + 3;
            continue;
        }

        /* title = anchor text; snippet = text up to the next anchor */
        {
            buf_t title;

            buf_init(&title);
            strip_slice(body, (size_t)(aend - body), 200, &title);
            if (!title.len) {
                buf_free(&title);
                buf_free(&url);
                p = aend + 3;
                continue;
            }
            count++;
            buf_printf(out, "%d. ", count);
            buf_append(out, title.data, title.len);
            buf_putc(out, '\n');
            buf_puts(out, "   ");
            buf_append(out, url.data, url.len);
            buf_putc(out, '\n');
            buf_free(&title);
        }
        next = ci_find(aend, (size_t)(end - aend), "<a");
        if (!next)
            next = end;
        {
            buf_t snip;
            const char *s = memchr(aend, '>', (size_t)(next - aend));

            buf_init(&snip);
            if (s)
                strip_slice(s + 1, (size_t)(next - s - 1), 400, &snip);
            if (snip.len) {
                buf_puts(out, "   ");
                buf_append(out, snip.data, snip.len);
                buf_putc(out, '\n');
            }
            buf_free(&snip);
        }
        buf_putc(out, '\n');
        buf_free(&url);
        p = next;
    }

    if (count == 0) {
        if (ci_find(html, n, "anomaly") || ci_find(html, n, "challenge") ||
            ci_find(html, n, "captcha"))
            buf_puts(out, "error: duckduckgo served a captcha page "
                          "(rate limited); try again later");
        else
            buf_puts(out, "error: no results");
        return -1;
    }
    return count;
}

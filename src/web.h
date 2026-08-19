#ifndef PIKI_WEB_H
#define PIKI_WEB_H

#include <stddef.h>
#include "buf.h"

/* Client-side web search: the web_search and fetch_url agent tools,
 * their search backends, and the pure helpers behind them. The pure
 * parts (URL handling, HTML stripping, results parsing) are exposed so
 * they are testable without a network, like the other parsers. */

enum { WEB_ENGINE_DDG = 0, WEB_ENGINE_BRAVE = 1 };

typedef struct {
    int engine;         /* WEB_ENGINE_* (config [search] engine) */
    char key[256];      /* API key for engines that need one ("" = none) */
} web_cfg;

/* --- pure parts (testable without network) ------------------------- */

/* Splits an http(s) URL into caller-owned buffers (parse_base_url in
 * main.c keeps static state and is not reusable per call). Default port
 * 443/80 by scheme; path keeps the query string; "" path becomes "/".
 * 0 ok, -1 invalid (bad scheme, empty/oversized host). */
int web_url_split(const char *url, char *host, size_t hostcap, int *port,
                  int *use_tls, char *path, size_t pathcap);

/* Percent-encodes s as a query value (unreserved chars kept, space
 * becomes %20) and appends it to out. */
void web_percent_encode(const char *s, buf_t *out);

/* Decodes %XX sequences ('+' stays '+'); invalid escapes are copied
 * literally. Appends to out. */
void web_percent_decode(const char *s, size_t n, buf_t *out);

/* Incremental HTML-to-text stripper: drops tags, skips <script>/<style>
 * bodies and <!-- comments -->, decodes a minimal entity set, collapses
 * whitespace, emits a newline at block-level tags. Must tolerate input
 * split at arbitrary byte boundaries. */
typedef struct {
    int state;
    char tag[12];       /* current tag name (lowercased) */
    size_t taglen;
    int closing;        /* tag started with </ */
    char quote;         /* attribute quote char inside a tag, or 0 */
    char skip[8];       /* tag whose body is being skipped ("" = none) */
    char ent[12];       /* entity accumulator (without '&') */
    size_t entlen;
    int pending_space;
    int pending_nl;
    int got_any;        /* emitted at least one visible char */
} htmlstrip;

void htmlstrip_init(htmlstrip *h);
void htmlstrip_feed(htmlstrip *h, const char *p, size_t n, buf_t *out);
void htmlstrip_finish(htmlstrip *h, buf_t *out);

/* Parses a DuckDuckGo Lite results page into numbered
 * "title\n   url\n   snippet" blocks appended to out. Returns the
 * number of results (> 0), or -1 with an error message in out when the
 * page carries no results (including the CAPTCHA/anomaly page). */
int web_ddg_parse(const char *html, size_t n, int max_results, buf_t *out);

/* --- network + tools ------------------------------------------------ */

/* Runs one search through the configured engine. 0 ok (results in out),
 * -1 error ("error: ..." in out). */
int web_search_run(const web_cfg *cfg, const char *query, int max_results,
                   buf_t *out);

/* Fetches url (http or https) following up to 5 redirects and appends
 * the page's readable text to out. 0 ok, -1 error ("error: ..." in out). */
int web_fetch_url(const char *url, buf_t *out);

/* Tool objects for the request schema (array items, no brackets). */
extern const char *const WEB_TOOLS_ITEMS;

int web_tool_is(const char *name);   /* web_search or fetch_url */
void web_tool_describe(const char *name, const char *args_json, buf_t *out);

/* Dispatches one web tool call. 0 ok, -1 error (text in out, sent to
 * the model either way), -2 unknown tool. Mirrors tool_run. */
int web_tool_run(const web_cfg *cfg, const char *name,
                 const char *args_json, buf_t *out);

#endif /* PIKI_WEB_H */

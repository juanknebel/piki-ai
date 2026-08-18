#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "json.h"
#include "net.h"
#include "sse.h"

/* OpenRouter web search plugin (equivalent to the ":online" model suffix). */
#define WEB_PLUGIN ",\"plugins\":[{\"id\":\"web\"}]"

static void build_body(buf_t *b, const char *model,
                       const chat_msg *msgs, size_t nmsgs,
                       int stream, int web)
{
    size_t i;

    buf_puts(b, "{\"model\":");
    json_escape(b, model);
    buf_printf(b, ",\"stream\":%s,\"messages\":[",
               stream ? "true" : "false");
    for (i = 0; i < nmsgs; i++) {
        if (i)
            buf_putc(b, ',');
        buf_puts(b, "{\"role\":");
        json_escape(b, msgs[i].role);
        buf_puts(b, ",\"content\":");
        json_escape(b, msgs[i].content);
        buf_putc(b, '}');
    }
    buf_puts(b, "]");
    if (stream)   /* ask for token usage in the final SSE event */
        buf_puts(b, ",\"stream_options\":{\"include_usage\":true}");
    if (web)
        buf_puts(b, WEB_PLUGIN);
    buf_putc(b, '}');
}

/* Reads prompt_tokens/completion_tokens from a "usage" object into u. */
static void read_usage(json_val *usage_obj, token_usage *u)
{
    if (!usage_obj || !u)
        return;
    u->prompt_tokens = (long)json_num(
        json_get(usage_obj, "prompt_tokens"), u->prompt_tokens);
    u->completion_tokens = (long)json_num(
        json_get(usage_obj, "completion_tokens"), u->completion_tokens);
}

static const char *status_hint(int status)
{
    switch (status) {
    case 401: return " (invalid API key?)";
    case 402: return " (no credits?)";
    case 404: return " (unknown model or endpoint?)";
    case 429: return " (rate limit)";
    default:  return "";
    }
}

static void set_http_error(int status, const char *body,
                           char *err, size_t errlen)
{
    json_doc *doc = NULL;
    json_val *v = json_parse(body, &doc, NULL, 0);
    const char *msg = v ? json_str(json_get(v, "error.message")) : NULL;

    snprintf(err, errlen, "HTTP %d%s%s%s",
             status, status_hint(status),
             msg ? ": " : "", msg ? msg : "");
    json_doc_free(doc);
}

/* --- connection reuse -------------------------------------------------- */

/* One cached keep-alive connection for the active provider: on old
 * hardware the TLS handshake is the most expensive part of a turn. */
static net_conn *cc_conn;
static char cc_host[256];
static int cc_port, cc_tls;

void api_conn_close(void)
{
    if (cc_conn) {
        net_close(cc_conn);
        cc_conn = NULL;
    }
}

/* Keeps c for the next request iff the response left it clean;
 * otherwise closes it. */
static void api_conn_put(const provider_t *pv, net_conn *c,
                         const http_resp *r)
{
    if (!http_resp_reusable(r) ||
        strlen(pv->host) >= sizeof cc_host) {
        net_close(c);
        return;
    }
    api_conn_close();
    snprintf(cc_host, sizeof cc_host, "%s", pv->host);
    cc_port = pv->port;
    cc_tls = pv->use_tls;
    cc_conn = c;
}

/* Connects (or reuses the cached connection), sends one request (POST
 * when body is set, GET otherwise) and reads the response headers into
 * r. On success *out_c is the live connection. 0 ok, -1 error (err),
 * -2 interrupted.
 *
 * Retry policy: a request is re-sent ONCE, on a fresh connection, only
 * when the REUSED connection failed at write time or the server closed
 * it before sending a single response byte -- the keep-alive race where
 * nothing was consumed, so the retry cannot duplicate a reply or a
 * charge (RFC 9112 9.4). A fresh connection is never retried here (the
 * connect itself already retries in net_connect), and neither is any
 * failure after the first response byte. */
static int api_send(const provider_t *pv, const char *path,
                    const char *body, size_t bodylen, int timeout_s,
                    net_conn **out_c, http_resp *r,
                    char *err, size_t errlen)
{
    int attempt;

    for (attempt = 0; attempt < 2; attempt++) {
        net_conn *c;
        int rc, reused = 0;

        if (cc_conn && strcmp(cc_host, pv->host) == 0 &&
            cc_port == pv->port && cc_tls == pv->use_tls) {
            c = cc_conn;
            cc_conn = NULL;
            reused = 1;
        } else {
            api_conn_close();   /* provider changed: drop the old one */
            c = net_connect(pv->host, pv->port, pv->use_tls,
                            err, errlen);
            if (!c)
                return -1;
        }
        /* read timeout is per-connection state: always reset it */
        net_set_read_timeout(c, timeout_s);

        rc = body ? http_post(c, pv->host, path, pv->api_key,
                              body, bodylen, 1)
                  : http_get(c, pv->host, path, pv->api_key, 1);
        if (rc == NET_EINTR) {
            net_close(c);
            return -2;
        }
        if (rc < 0) {
            net_close(c);
            if (reused)
                continue;   /* stale keep-alive: once more, fresh */
            snprintf(err, errlen, "network error sending the request");
            return -1;
        }

        rc = http_read_response(c, r, err, errlen);
        if (rc == NET_EINTR) {
            net_close(c);
            return -2;
        }
        if (rc == HTTP_EARLY_EOF) {
            net_close(c);
            if (reused)
                continue;   /* idle-close race: once more, fresh */
            snprintf(err, errlen,
                     "the server closed the connection "
                     "without responding");
            return -1;
        }
        if (rc < 0) {
            net_close(c);
            return -1;
        }
        *out_c = c;
        return 0;
    }
    /* unreachable: the second attempt never uses a reused connection */
    snprintf(err, errlen, "network error sending the request");
    return -1;
}

int api_chat(const provider_t *pv, const char *model,
             const chat_msg *msgs, size_t nmsgs,
             buf_t *out, char *err, size_t errlen)
{
    buf_t body, path, resp;
    net_conn *c = NULL;
    json_doc *doc = NULL;
    http_resp r;
    int rc, ret = -1;

    buf_init(&body);
    buf_init(&path);
    buf_init(&resp);
    build_body(&body, model, msgs, nmsgs, 0, 0);
    buf_printf(&path, "%s/chat/completions", pv->base_path);

    rc = api_send(pv, path.data, body.data, body.len, 0, &c, &r,
                  err, errlen);
    if (rc == -2) {
        ret = -2;
        goto done;
    }
    if (rc < 0)
        goto done; /* err ya seteado */

    rc = http_read_all(&r, &resp);
    if (rc == NET_EINTR) {
        ret = -2;
        goto done;
    }
    if (rc < 0) {
        snprintf(err, errlen, "network error reading the response");
        goto done;
    }
    api_conn_put(pv, c, &r);
    c = NULL;

    if (r.meta.status != 200) {
        set_http_error(r.meta.status, resp.data ? resp.data : "",
                       err, errlen);
        goto done;
    }

    {
        json_val *v = json_parse(resp.data, &doc, err, errlen);
        const char *content;

        if (!v)
            goto done;
        content = json_str(json_get(v, "choices.0.message.content"));
        if (!content) {
            snprintf(err, errlen,
                     "response has no choices[0].message.content");
            goto done;
        }
        buf_puts(out, content);
    }
    ret = 0;

done:
    json_doc_free(doc);
    if (c)
        net_close(c);
    buf_free(&body);
    buf_free(&path);
    buf_free(&resp);
    return ret;
}

/* --- Responses API (server-side web search) --------------------------- */

/* Request body for {base}/responses: the history goes in "input" as an
 * array of role/content messages and web search is a built-in tool.
 * tool_choice forces the search so /web behaves like OpenRouter's
 * plugin: on = every answer is grounded, off = the tool does not exist. */
static void build_responses_body(buf_t *b, const char *model,
                                 const chat_msg *msgs, size_t nmsgs)
{
    size_t i;

    buf_puts(b, "{\"model\":");
    json_escape(b, model);
    buf_puts(b, ",\"input\":[");
    for (i = 0; i < nmsgs; i++) {
        if (i)
            buf_putc(b, ',');
        buf_puts(b, "{\"role\":");
        json_escape(b, msgs[i].role);
        buf_puts(b, ",\"content\":");
        json_escape(b, msgs[i].content);
        buf_putc(b, '}');
    }
    buf_puts(b, "],\"tools\":[{\"type\":\"web_search\"}],"
                "\"tool_choice\":{\"type\":\"web_search\"}}");
}

/* Appends "title <url>" to sources unless that url is already there. */
static void add_citation(buf_t *sources, const char *title,
                         const char *url)
{
    if (!url || !*url)
        return;
    if (sources->data && strstr(sources->data, url))
        return;
    if (title && *title) {
        buf_puts(sources, title);
        buf_putc(sources, ' ');
    }
    buf_putc(sources, '<');
    buf_puts(sources, url);
    buf_puts(sources, ">\n");
}

int api_responses_parse(const char *body, buf_t *out, buf_t *sources,
                        token_usage *usage, char *err, size_t errlen)
{
    json_doc *doc = NULL;
    json_val *v = json_parse(body, &doc, err, errlen);
    json_val *output;
    size_t i, j, k;
    int got_text = 0;

    if (!v)
        return -1;
    output = json_get(v, "output");
    if (!output || output->type != JSON_ARR) {
        snprintf(err, errlen, "response has no output array");
        json_doc_free(doc);
        return -1;
    }
    for (i = 0; i < output->u.arr.n; i++) {
        json_val *item = output->u.arr.items[i];
        json_val *content;
        const char *type = json_str(json_get(item, "type"));

        if (!type || strcmp(type, "message") != 0)
            continue;   /* web_search_call, reasoning, ... */
        content = json_get(item, "content");
        if (!content || content->type != JSON_ARR)
            continue;
        for (j = 0; j < content->u.arr.n; j++) {
            json_val *part = content->u.arr.items[j];
            json_val *anns;
            const char *ptype = json_str(json_get(part, "type"));
            const char *text = json_str(json_get(part, "text"));

            if (!ptype || strcmp(ptype, "output_text") != 0 || !text)
                continue;
            buf_puts(out, text);
            got_text = 1;
            anns = json_get(part, "annotations");
            if (!sources || !anns || anns->type != JSON_ARR)
                continue;
            for (k = 0; k < anns->u.arr.n; k++) {
                json_val *a = anns->u.arr.items[k];
                const char *atype = json_str(json_get(a, "type"));

                if (atype && strcmp(atype, "url_citation") == 0)
                    add_citation(sources,
                                 json_str(json_get(a, "title")),
                                 json_str(json_get(a, "url")));
            }
        }
    }
    if (usage) {
        json_val *u = json_get(v, "usage");

        usage->prompt_tokens = (long)json_num(
            json_get(u, "input_tokens"), usage->prompt_tokens);
        usage->completion_tokens = (long)json_num(
            json_get(u, "output_tokens"), usage->completion_tokens);
    }
    json_doc_free(doc);
    if (!got_text) {
        snprintf(err, errlen, "response has no output text");
        return -1;
    }
    return 0;
}

int api_responses_turn(const provider_t *pv, const char *model,
                       const chat_msg *msgs, size_t nmsgs,
                       token_usage *usage, buf_t *out, buf_t *sources,
                       char *err, size_t errlen)
{
    buf_t body, path, resp;
    net_conn *c = NULL;
    http_resp r;
    int rc, ret = -1;

    buf_init(&body);
    buf_init(&path);
    buf_init(&resp);
    build_responses_body(&body, model, msgs, nmsgs);
    buf_printf(&path, "%s/responses", pv->base_path);

    rc = api_send(pv, path.data, body.data, body.len, 0, &c, &r,
                  err, errlen);
    if (rc == -2) {
        ret = -2;
        goto done;
    }
    if (rc < 0)
        goto done;
    rc = http_read_all(&r, &resp);
    if (rc == NET_EINTR) {
        ret = -2;
        goto done;
    }
    if (rc < 0) {
        snprintf(err, errlen, "network error reading the response");
        goto done;
    }
    api_conn_put(pv, c, &r);
    c = NULL;
    if (r.meta.status != 200) {
        set_http_error(r.meta.status, resp.data ? resp.data : "",
                       err, errlen);
        goto done;
    }
    ret = api_responses_parse(resp.data ? resp.data : "", out, sources,
                              usage, err, errlen);

done:
    if (c)
        net_close(c);
    buf_free(&body);
    buf_free(&path);
    buf_free(&resp);
    return ret;
}

int api_models(const provider_t *pv, buf_t *out,
               char *err, size_t errlen)
{
    buf_t path, resp;
    net_conn *c = NULL;
    json_doc *doc = NULL;
    http_resp r;
    int rc, ret = -1;

    buf_init(&path);
    buf_init(&resp);
    buf_printf(&path, "%s/models", pv->base_path);

    rc = api_send(pv, path.data, NULL, 0, 0, &c, &r, err, errlen);
    if (rc == -2) {
        ret = -2;
        goto done;
    }
    if (rc < 0)
        goto done;
    rc = http_read_all(&r, &resp);
    if (rc == NET_EINTR) {
        ret = -2;
        goto done;
    }
    if (rc < 0) {
        snprintf(err, errlen, "network error reading the response");
        goto done;
    }
    api_conn_put(pv, c, &r);
    c = NULL;
    if (r.meta.status != 200) {
        set_http_error(r.meta.status, resp.data ? resp.data : "",
                       err, errlen);
        goto done;
    }
    {
        json_val *v = json_parse(resp.data, &doc, err, errlen);
        json_val *data;
        size_t i;

        if (!v)
            goto done;
        data = json_get(v, "data");
        if (!data || data->type != JSON_ARR) {
            snprintf(err, errlen, "response has no model list");
            goto done;
        }
        for (i = 0; i < data->u.arr.n; i++) {
            const char *id =
                json_str(json_get(data->u.arr.items[i], "id"));

            if (id) {
                buf_puts(out, id);
                buf_putc(out, '\n');
            }
        }
    }
    ret = 0;

done:
    json_doc_free(doc);
    if (c)
        net_close(c);
    buf_free(&path);
    buf_free(&resp);
    return ret;
}

int api_latest_version(const char *owner_repo, char *out, size_t cap,
                       char *err, size_t errlen)
{
    buf_t path, resp;
    net_conn *c = NULL;
    json_doc *doc = NULL;
    http_resp r;
    int rc, ret = -1;

    buf_init(&path);
    buf_init(&resp);
    buf_printf(&path, "/repos/%s/releases/latest", owner_repo);

    c = net_connect("api.github.com", 443, 1, err, errlen);
    if (!c)
        goto done;

    rc = http_get(c, "api.github.com", path.data, NULL, 0);
    if (rc == NET_EINTR) {
        ret = -2;
        goto done;
    }
    if (rc < 0) {
        snprintf(err, errlen, "network error sending the request");
        goto done;
    }
    rc = http_read_response(c, &r, err, errlen);
    if (rc == NET_EINTR) {
        ret = -2;
        goto done;
    }
    if (rc < 0) {
        if (rc == HTTP_EARLY_EOF)
            snprintf(err, errlen, "the server closed the connection "
                     "without responding");
        goto done;
    }
    rc = http_read_all(&r, &resp);
    if (rc == NET_EINTR) {
        ret = -2;
        goto done;
    }
    if (rc < 0) {
        snprintf(err, errlen, "network error reading the response");
        goto done;
    }
    if (r.meta.status != 200) {
        set_http_error(r.meta.status, resp.data ? resp.data : "",
                       err, errlen);
        goto done;
    }
    {
        json_val *v = json_parse(resp.data, &doc, err, errlen);
        const char *tag;

        if (!v)
            goto done;
        tag = json_str(json_get(v, "tag_name"));
        if (!tag || !*tag) {
            snprintf(err, errlen, "response has no tag_name");
            goto done;
        }
        if (*tag == 'v')
            tag++;
        snprintf(out, cap, "%s", tag);
    }
    ret = 0;

done:
    json_doc_free(doc);
    if (c)
        net_close(c);
    buf_free(&path);
    buf_free(&resp);
    return ret;
}

/* --- tool use / agent ------------------------------------------------- */

void api_turn_init(api_turn *t)
{
    buf_init(&t->content);
    t->calls = NULL;
    t->ncalls = 0;
}

void api_turn_free(api_turn *t)
{
    size_t i;

    buf_free(&t->content);
    for (i = 0; i < t->ncalls; i++) {
        free(t->calls[i].id);
        free(t->calls[i].name);
        free(t->calls[i].arguments);
    }
    free(t->calls);
    t->calls = NULL;
    t->ncalls = 0;
}

static char *dup_or_empty(const char *s)
{
    size_t n = (s ? strlen(s) : 0) + 1;
    char *p = malloc(n);

    if (!p) {
        fputs("piki: out of memory\n", stderr);
        abort();
    }
    if (s)
        memcpy(p, s, n);
    else
        p[0] = '\0';
    return p;
}

int api_agent_turn(const provider_t *pv, const char *model,
                   const char *messages_json, const char *tools_json,
                   int web, token_usage *usage, api_turn *out,
                   char *err, size_t errlen)
{
    buf_t body, path, resp;
    net_conn *c = NULL;
    json_doc *doc = NULL;
    http_resp r;
    int rc, ret = -1;

    if (usage) {
        usage->prompt_tokens = 0;
        usage->completion_tokens = 0;
    }
    api_turn_init(out);
    buf_init(&body);
    buf_init(&path);
    buf_init(&resp);

    buf_puts(&body, "{\"model\":");
    json_escape(&body, model);
    buf_puts(&body, ",\"stream\":false,\"messages\":");
    buf_puts(&body, messages_json);
    if (tools_json) {
        buf_puts(&body, ",\"tools\":");
        buf_puts(&body, tools_json);
    }
    if (web)
        buf_puts(&body, WEB_PLUGIN);
    buf_putc(&body, '}');
    buf_printf(&path, "%s/chat/completions", pv->base_path);

    rc = api_send(pv, path.data, body.data, body.len, 0, &c, &r,
                  err, errlen);
    if (rc == -2) { ret = -2; goto done; }
    if (rc < 0)
        goto done;
    rc = http_read_all(&r, &resp);
    if (rc == NET_EINTR) { ret = -2; goto done; }
    if (rc < 0) {
        snprintf(err, errlen, "network error reading the response");
        goto done;
    }
    api_conn_put(pv, c, &r);
    c = NULL;
    if (r.meta.status != 200) {
        set_http_error(r.meta.status, resp.data ? resp.data : "",
                       err, errlen);
        goto done;
    }

    {
        json_val *v = json_parse(resp.data, &doc, err, errlen);
        json_val *msg, *content, *calls;
        const char *cs;

        if (!v)
            goto done;
        read_usage(json_get(v, "usage"), usage);
        msg = json_get(v, "choices.0.message");
        if (!msg) {
            snprintf(err, errlen, "response has no choices[0].message");
            goto done;
        }
        content = json_get(msg, "content");
        cs = json_str(content);
        if (cs)
            buf_puts(&out->content, cs);

        calls = json_get(msg, "tool_calls");
        if (calls && calls->type == JSON_ARR && calls->u.arr.n) {
            size_t i;

            out->calls = calloc(calls->u.arr.n, sizeof *out->calls);
            if (!out->calls) {
                snprintf(err, errlen, "out of memory");
                goto done;
            }
            for (i = 0; i < calls->u.arr.n; i++) {
                json_val *tc = calls->u.arr.items[i];

                out->calls[i].id =
                    dup_or_empty(json_str(json_get(tc, "id")));
                out->calls[i].name =
                    dup_or_empty(json_str(json_get(tc, "function.name")));
                out->calls[i].arguments = dup_or_empty(
                    json_str(json_get(tc, "function.arguments")));
                out->ncalls++;
            }
        }
    }
    ret = 0;

done:
    if (ret != 0 && ret != -2)
        api_turn_free(out);
    json_doc_free(doc);
    if (c)
        net_close(c);
    buf_free(&body);
    buf_free(&path);
    buf_free(&resp);
    return ret;
}

/* --- streaming --------------------------------------------------------- */

typedef struct {
    api_on_delta on_delta;
    void *user;
    int done;        /* we saw data: [DONE] */
    int got_error;   /* the stream carried {"error":...} */
    int user_stop;   /* the callback requested to cut */
    token_usage usage;
    char *err;
    size_t errlen;
} stream_ctx;

static int on_event(const char *data, void *user)
{
    stream_ctx *sc = user;
    json_doc *doc = NULL;
    json_val *v;
    const char *msg, *delta;
    int rc = 0;

    if (strcmp(data, "[DONE]") == 0) {
        sc->done = 1;
        return 1;
    }
    v = json_parse(data, &doc, NULL, 0);
    if (!v)
        return 0; /* non-JSON event: ignore */

    msg = json_str(json_get(v, "error.message"));
    if (msg) {
        snprintf(sc->err, sc->errlen, "server error: %s", msg);
        sc->got_error = 1;
        json_doc_free(doc);
        return 1;
    }
    read_usage(json_get(v, "usage"), &sc->usage);  /* final chunk carries it */
    delta = json_str(json_get(v, "choices.0.delta.content"));
    if (delta && *delta) {
        rc = sc->on_delta(delta, sc->user);
        if (rc)
            sc->user_stop = 1;
    }
    json_doc_free(doc);
    return rc;
}

int api_chat_stream(const provider_t *pv, const char *model,
                    const chat_msg *msgs, size_t nmsgs, int web,
                    token_usage *usage,
                    api_on_delta on_delta, void *user,
                    char *err, size_t errlen)
{
    buf_t body, path, chunk;
    net_conn *c = NULL;
    http_resp r;
    sse_parser sp;
    stream_ctx sc;
    int rc, ret = -1;

    buf_init(&body);
    buf_init(&path);
    buf_init(&chunk);
    sse_init(&sp);
    memset(&sc, 0, sizeof sc);
    sc.on_delta = on_delta;
    sc.user = user;
    sc.err = err;
    sc.errlen = errlen;

    build_body(&body, model, msgs, nmsgs, 1, web);
    buf_printf(&path, "%s/chat/completions", pv->base_path);

    /* the 120 s inactivity timeout cuts off a server that stops
     * sending tokens mid-stream */
    rc = api_send(pv, path.data, body.data, body.len, 120, &c, &r,
                  err, errlen);
    if (rc == -2) {
        ret = -2;
        goto done;
    }
    if (rc < 0)
        goto done;

    if (r.meta.status != 200) {
        buf_t resp;

        buf_init(&resp);
        rc = http_read_all(&r, &resp);
        if (rc == NET_EINTR) {
            buf_free(&resp);
            ret = -2;
            goto done;
        }
        set_http_error(r.meta.status, resp.data ? resp.data : "",
                       err, errlen);
        buf_free(&resp);
        goto done;
    }

    for (;;) {
        ssize_t nrc;

        buf_reset(&chunk);
        nrc = http_read_body(&r, &chunk);
        if (nrc == 0)
            break; /* end of body */
        if (nrc == NET_EINTR) {
            ret = -2;
            goto done;
        }
        if (nrc == NET_TIMEOUT) {
            snprintf(err, errlen,
                     "timeout: the server stopped responding");
            goto done;
        }
        if (nrc < 0) {
            snprintf(err, errlen,
                     "connection lost during streaming");
            goto done;
        }
        if (sse_feed(&sp, chunk.data, chunk.len, on_event, &sc))
            break;
    }
    if (sc.got_error)
        goto done; /* err already set by on_event */
    /* only a stream read to the end leaves the connection clean; a cut
     * (Ctrl-C, error) still has bytes in flight and is closed instead */
    api_conn_put(pv, c, &r);
    c = NULL;
    ret = 0;

done:
    if (usage && ret == 0)
        *usage = sc.usage;
    sse_free(&sp);
    if (c)
        net_close(c);
    buf_free(&body);
    buf_free(&path);
    buf_free(&chunk);
    return ret;
}

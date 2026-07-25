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
    if (web)
        buf_puts(b, WEB_PLUGIN);
    buf_putc(b, '}');
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

    c = net_connect(pv->host, pv->port, pv->use_tls, err, errlen);
    if (!c)
        goto done;

    rc = http_post(c, pv->host, path.data, pv->api_key,
                   body.data, body.len);
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

    c = net_connect(pv->host, pv->port, pv->use_tls, err, errlen);
    if (!c)
        goto done;

    rc = http_get(c, pv->host, path.data, pv->api_key);
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
                   int web, api_turn *out, char *err, size_t errlen)
{
    buf_t body, path, resp;
    net_conn *c = NULL;
    json_doc *doc = NULL;
    http_resp r;
    int rc, ret = -1;

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

    c = net_connect(pv->host, pv->port, pv->use_tls, err, errlen);
    if (!c)
        goto done;
    rc = http_post(c, pv->host, path.data, pv->api_key,
                   body.data, body.len);
    if (rc == NET_EINTR) { ret = -2; goto done; }
    if (rc < 0) {
        snprintf(err, errlen, "network error sending the request");
        goto done;
    }
    rc = http_read_response(c, &r, err, errlen);
    if (rc == NET_EINTR) { ret = -2; goto done; }
    if (rc < 0)
        goto done;
    rc = http_read_all(&r, &resp);
    if (rc == NET_EINTR) { ret = -2; goto done; }
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
        json_val *msg, *content, *calls;
        const char *cs;

        if (!v)
            goto done;
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

    c = net_connect(pv->host, pv->port, pv->use_tls, err, errlen);
    if (!c)
        goto done;
    /* if the server stops sending tokens for 120 s, we cut off */
    net_set_read_timeout(c, 120);

    rc = http_post(c, pv->host, path.data, pv->api_key,
                   body.data, body.len);
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
    ret = 0;

done:
    sse_free(&sp);
    if (c)
        net_close(c);
    buf_free(&body);
    buf_free(&path);
    buf_free(&chunk);
    return ret;
}

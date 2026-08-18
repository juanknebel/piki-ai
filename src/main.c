/* piki — lightweight LLM chat client.
 *
 * usage: piki [options]                interactive REPL
 *        piki [options] "question"    one shot and exit
 *
 * options: -m model  -p provider  -s system_prompt  -t (tools)  --version
 * config:  ~/.config/piki/config   (see src/config.h)
 * env:     OPENROUTER_API_KEY  overrides the "openrouter" provider key
 *          PIKI_BASE_URL       overrides the endpoint (e.g. http://host:11434/v1)
 */
#include <ctype.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "api.h"
#include "buf.h"
#include "chat.h"
#include "config.h"
#include "edit.h"
#include "json.h"
#include "md.h"
#include "net.h"
#include "term.h"
#include "tools.h"
#include "version.h"

#define PIKI_AUTHOR   "Juan Knebel <juanknebel@gmail.com>"
#define PIKI_REPO     "https://github.com/juanknebel/piki-ai"
#define DEFAULT_MODEL "anthropic/claude-haiku-4.5"
#define HISTORY_MAX 1000

static int use_color;

#define C_PROMPT (use_color ? "\033[1;36m> \033[0m" : "> ")
#define C_DIM    (use_color ? "\033[2m" : "")
#define C_BOLD   (use_color ? "\033[1m" : "")
#define C_RESET  (use_color ? "\033[0m" : "")

static void on_sigint(int sig)
{
    (void)sig;
    net_interrupt = 1;
}

static void usage(void)
{
    fputs("usage: piki [-m model] [-p provider] [-s system_prompt] "
          "[-t] [-w] [--resume] [\"question\"]\n"
          "     with no question it enters interactive mode (/help for "
          "commands)\n"
          "     piped stdin is sent as the question, or appended to it\n"
          "        as context: cmd | piki, piki \"explain\" < file\n"
          "     -t enables tool use for a one-shot question (in the REPL\n"
          "        tools are on by default; /tools toggles them)\n"
          "     -w enables OpenRouter web search\n"
          "     --resume continues the last conversation\n"
          "config: ~/.config/piki/config\n"
          "env: OPENROUTER_API_KEY, PIKI_BASE_URL\n"
          "\n" PIKI_REPO "\n", stderr);
}

/* How this provider does server-side web search: the provider's
 * web_search config key wins; otherwise detect by host. New providers
 * (e.g. Anthropic server tools) plug in here and in api.h/api.c. */
static int provider_web_kind(const char *host, const char *cfg_web)
{
    if (cfg_web && *cfg_web) {
        if (strcmp(cfg_web, "plugin") == 0)
            return API_WEB_PLUGIN;
        if (strcmp(cfg_web, "responses") == 0)
            return API_WEB_RESPONSES;
        return API_WEB_NONE;
    }
    if (strstr(host, "openrouter.ai"))
        return API_WEB_PLUGIN;
    if (strstr(host, "meta.ai"))
        return API_WEB_RESPONSES;
    return API_WEB_NONE;
}

/* Parses http[s]://host[:port][/base] into pv. 0 ok, -1 invalid. */
static int parse_base_url(const char *url, provider_t *pv)
{
    static char host[256];
    static char path[256];
    const char *p;
    const char *slash;
    size_t hlen;
    char *colon;

    if (strncmp(url, "https://", 8) == 0) {
        pv->use_tls = 1;
        pv->port = 443;
        p = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        pv->use_tls = 0;
        pv->port = 80;
        p = url + 7;
    } else {
        return -1;
    }

    slash = strchr(p, '/');
    hlen = slash ? (size_t)(slash - p) : strlen(p);
    if (hlen == 0 || hlen >= sizeof host)
        return -1;
    memcpy(host, p, hlen);
    host[hlen] = '\0';

    path[0] = '\0';
    if (slash) {
        size_t plen = strlen(slash);

        while (plen > 1 && slash[plen - 1] == '/')
            plen--;
        if (plen >= sizeof path)
            return -1;
        if (plen > 1) {
            memcpy(path, slash, plen);
            path[plen] = '\0';
        }
    }

    colon = strchr(host, ':');
    if (colon) {
        *colon = '\0';
        pv->port = atoi(colon + 1);
        if (pv->port <= 0 || pv->port > 65535 || !*host)
            return -1;
    }
    pv->host = host;
    pv->base_path = path;
    return 0;
}

static int bytes_per_token(const char *model)
{
    if (!model) return 4;
    if (strstr(model, "llama") || strstr(model, "mistral") || strstr(model, "qwen"))
        return 3;
    if (strstr(model, "gpt") || strstr(model, "o1") || strstr(model, "o3"))
        return 3;
    return 4;
}

/* --- streaming of a simple turn (no tools) --------------------------- */

/* Lightweight markdown to ANSI (TTY only). Handles **bold**, `code`,
 * ```block``` and *italic* / _italic_ via inline state in print_ctx. */
typedef struct {
    int last_was_nl;
    int got_any;
    buf_t acc;
    md_state md;
    buf_t rend;   /* scratch for the rendered bytes of one delta */
} print_ctx;

static int print_delta(const char *text, void *user)
{
    print_ctx *pc = user;
    size_t n = strlen(text);

    buf_reset(&pc->rend);
    md_feed(&pc->md, text, n, &pc->rend);
    if (pc->rend.len)
        fwrite(pc->rend.data, 1, pc->rend.len, stdout);
    fflush(stdout);
    buf_puts(&pc->acc, text);
    if (n) {
        pc->last_was_nl = text[n - 1] == '\n';
        pc->got_any = 1;
    }
    return 0;
}

/* Renders a complete markdown answer (agent and Responses turns print
 * whole messages, not deltas). */
static void print_md(const char *s, size_t n)
{
    md_state st;
    buf_t out;

    md_init(&st, use_color);
    buf_init(&out);
    md_feed(&st, s, n, &out);
    md_finish(&st, &out);
    if (out.len)
        fwrite(out.data, 1, out.len, stdout);
    buf_free(&out);
}

/* Limits on what is SENT per turn (distinct from chat_t's RAM cap). */
typedef struct {
    size_t max_msgs;
    size_t max_bytes;   /* 0 = no limit */
} send_limits;

static int stream_turn(const provider_t *pv, const char *model,
                       const chat_t *chat, send_limits lim, int web,
                       token_usage *usage, print_ctx *pc,
                       char *err, size_t errlen)
{
    chat_msg *win;
    size_t wn;
    int rc;

    win = malloc((chat->n + 1) * sizeof *win);
    if (!win) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    wn = chat_window(chat, lim.max_msgs, lim.max_bytes, win, chat->n + 1);
    memset(pc, 0, sizeof *pc);
    buf_init(&pc->acc);
    buf_init(&pc->rend);
    md_init(&pc->md, use_color);
    rc = api_chat_stream(pv, model, win, wn, web, usage, print_delta, pc,
                         err, errlen);
    /* flush held-back marker bytes; reset the color if a mode is open
     * (also after an interrupt, so the terminal is not left dim) */
    buf_reset(&pc->rend);
    md_finish(&pc->md, &pc->rend);
    if (pc->rend.len)
        fwrite(pc->rend.data, 1, pc->rend.len, stdout);
    buf_free(&pc->rend);
    if (pc->got_any && !pc->last_was_nl) {
        putchar('\n');
        fflush(stdout);
    }
    free(win);
    return rc;
}

/* One web-search turn through the provider's Responses API. Non-streaming
 * (like agent turns); prints the answer and its sources, and leaves the
 * text in final for the history. */
static int responses_turn(const provider_t *pv, const char *model,
                          const chat_t *chat, send_limits lim,
                          token_usage *usage, buf_t *final,
                          char *err, size_t errlen)
{
    chat_msg *win;
    size_t wn;
    buf_t sources;
    int rc;

    win = malloc((chat->n + 1) * sizeof *win);
    if (!win) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    wn = chat_window(chat, lim.max_msgs, lim.max_bytes, win, chat->n + 1);
    buf_init(&sources);
    rc = api_responses_turn(pv, model, win, wn, usage, final, &sources,
                            err, errlen);
    if (rc == 0) {
        print_md(final->data ? final->data : "", final->len);
        putchar('\n');
        if (sources.len)
            printf("%s%.*s%s", C_DIM, (int)sources.len, sources.data,
                   C_RESET);
        fflush(stdout);
    }
    buf_free(&sources);
    free(win);
    return rc;
}

/* --- agent with tools ------------------------------------------------- */

/* Appends a {role,content} message to the JSON array in msgs (without
 * brackets; the caller handles the commas). */
static void append_msg_json(buf_t *msgs, int first,
                            const char *role, const char *content)
{
    if (!first)
        buf_putc(msgs, ',');
    buf_puts(msgs, "{\"role\":");
    json_escape(msgs, role);
    buf_puts(msgs, ",\"content\":");
    json_escape(msgs, content);
    buf_putc(msgs, '}');
}

/* Reads a y/N answer on stdin (the caller printed the question).
 * Returns 1 if yes. */
static int ask_yn(void)
{
    char resp[16];

    fflush(stdout);
    if (!fgets(resp, sizeof resp, stdin))
        return 0;
    return resp[0] == 'y' || resp[0] == 'Y' || resp[0] == 's' ||
           resp[0] == 'S';
}

/* Tools the user answered 'a' (always) for: skip their confirmation for
 * the rest of the session. Never persisted. */
#define MAX_ALWAYS_OK 8
static char always_ok[MAX_ALWAYS_OK][32];
static size_t n_always_ok;

static int always_allowed(const char *tool)
{
    size_t i;

    for (i = 0; i < n_always_ok; i++)
        if (strcmp(always_ok[i], tool) == 0)
            return 1;
    return 0;
}

static void allow_always(const char *tool)
{
    if (n_always_ok < MAX_ALWAYS_OK) {
        snprintf(always_ok[n_always_ok], sizeof always_ok[0], "%s", tool);
        n_always_ok++;
    }
}

/* Asks y/N/a on stdin (line). Returns 0 no, 1 yes, 2 always (yes and do
 * not ask again for this tool this session). */
static int confirm(const char *what)
{
    char resp[16];

    printf("%srun %s? [y/N/a] %s", C_BOLD, what, C_RESET);
    fflush(stdout);
    if (!fgets(resp, sizeof resp, stdin))
        return 0;
    if (resp[0] == 'a' || resp[0] == 'A')
        return 2;
    return (resp[0] == 'y' || resp[0] == 'Y' || resp[0] == 's' ||
            resp[0] == 'S') ? 1 : 0;
}

/* Runs the agent loop for the last user message already added to chat.
 * Every max_steps tool rounds it asks whether to keep going; declining is
 * not an error: the text produced so far is kept as the reply so the next
 * turn knows what the agent was doing. Returns 0 ok, -1 error (err),
 * -2 interrupted. Leaves the final response in final (for the history). */
static int agent_turn(const provider_t *pv, const char *model,
                      const chat_t *chat, send_limits lim, int web,
                      long max_steps, token_usage *usage, buf_t *final,
                      char *err, size_t errlen)
{
    buf_t msgs;
    buf_t acc;            /* all assistant text, kept if stopped early */
    chat_msg *win;
    size_t wn, i;
    long step, limit = max_steps;
    int ret = -1;
    int first = 1;

    if (usage) {
        usage->prompt_tokens = 0;
        usage->completion_tokens = 0;
    }

    /* initial message array from the history window */
    buf_init(&msgs);
    buf_init(&acc);
    buf_putc(&msgs, '[');
    win = malloc((chat->n + 1) * sizeof *win);
    if (!win) {
        snprintf(err, errlen, "out of memory");
        buf_free(&msgs);
        buf_free(&acc);
        return -1;
    }
    wn = chat_window(chat, lim.max_msgs, lim.max_bytes, win, chat->n + 1);
    for (i = 0; i < wn; i++) {
        append_msg_json(&msgs, first, win[i].role, win[i].content);
        first = 0;
    }
    free(win);

    for (step = 0; ; step++) {
        api_turn turn;
        size_t j;
        int rc;

        if (step == limit) {
            printf("%sthe agent has used %ld steps%s\n"
                   "%scontinue for another %ld? [y/N] %s",
                   C_DIM, step, C_RESET, C_BOLD, max_steps, C_RESET);
            if (!ask_yn()) {
                /* graceful stop: keep what was done as the reply */
                if (acc.len)
                    buf_puts(&acc, "\n");
                buf_puts(&acc, "[agent stopped by the user after ");
                {
                    char n[32];

                    snprintf(n, sizeof n, "%ld", step);
                    buf_puts(&acc, n);
                }
                buf_puts(&acc, " steps; the task may be unfinished]");
                buf_reset(final);
                buf_append(final, acc.data, acc.len);
                ret = 0;
                goto done;
            }
            limit += max_steps;
        }

        buf_putc(&msgs, ']');
        {
            token_usage step_u = {0, 0};

            rc = api_agent_turn(pv, model, msgs.data, TOOLS_SCHEMA, web,
                                &step_u, &turn, err, errlen);
            if (usage) {
                usage->prompt_tokens += step_u.prompt_tokens;
                usage->completion_tokens += step_u.completion_tokens;
            }
        }
        msgs.len--;              /* reopen the array */
        msgs.data[msgs.len] = '\0';
        if (rc != 0) {
            ret = rc;
            goto done;
        }

        if (turn.content.len) {
            print_md(turn.content.data, turn.content.len);
            putchar('\n');
            fflush(stdout);
            if (acc.len)
                buf_puts(&acc, "\n");
            buf_append(&acc, turn.content.data, turn.content.len);
        }

        if (turn.ncalls == 0) {
            buf_reset(final);
            buf_append(final, turn.content.data, turn.content.len);
            api_turn_free(&turn);
            ret = 0;
            goto done;
        }

        /* rebuild the assistant message with its tool_calls */
        if (!first)
            buf_putc(&msgs, ',');
        first = 0;
        buf_puts(&msgs, "{\"role\":\"assistant\",\"content\":");
        json_escape(&msgs, turn.content.data ? turn.content.data : "");
        buf_puts(&msgs, ",\"tool_calls\":[");
        for (j = 0; j < turn.ncalls; j++) {
            if (j)
                buf_putc(&msgs, ',');
            buf_puts(&msgs, "{\"id\":");
            json_escape(&msgs, turn.calls[j].id);
            buf_puts(&msgs, ",\"type\":\"function\",\"function\":{"
                            "\"name\":");
            json_escape(&msgs, turn.calls[j].name);
            buf_puts(&msgs, ",\"arguments\":");
            json_escape(&msgs, turn.calls[j].arguments);
            buf_puts(&msgs, "}}");
        }
        buf_puts(&msgs, "]}");

        /* run each tool and add its result */
        for (j = 0; j < turn.ncalls; j++) {
            api_tool_call *tc = &turn.calls[j];
            buf_t desc, result;
            int allowed = 1;

            buf_init(&desc);
            buf_init(&result);
            tool_describe(tc->name, tc->arguments, &desc);
            printf("%s· %s%s\n", C_DIM, desc.data, C_RESET);
            fflush(stdout);

            if (tool_is_dangerous(tc->name) &&
                !always_allowed(tc->name)) {
                int c3 = confirm(desc.data);

                allowed = c3 > 0;
                if (c3 == 2) {
                    allow_always(tc->name);
                    printf("%swill not ask again for %s this session%s\n",
                           C_DIM, tc->name, C_RESET);
                }
            }

            if (allowed) {
                tool_run(tc->name, tc->arguments, &result);
            } else {
                buf_puts(&result,
                         "the user declined to run this action");
            }

            append_msg_json(&msgs, first, "tool", result.data);
            /* note: tool messages need tool_call_id; we add it by hand
             * because append_msg_json does not include it */
            msgs.len--;         /* remove the '}' */
            buf_puts(&msgs, ",\"tool_call_id\":");
            json_escape(&msgs, tc->id);
            buf_putc(&msgs, '}');
            first = 0;

            buf_free(&desc);
            buf_free(&result);
        }
        api_turn_free(&turn);

        if (net_interrupt) {
            ret = -2;
            goto done;
        }
    }

done:
    buf_free(&msgs);
    buf_free(&acc);
    return ret;
}

/* --- REPL ------------------------------------------------------------- */

static void repl_help(int tools_on, int web_on)
{
    printf("commands:\n"
           "  /model [id]         show or change the model\n"
           "  /model save         save the model as the provider default\n"
           "  /model <id> save    change model and save as default\n"
           "  /default [id]       alias for /model save\n"
           "  /models             list the provider's models\n"
           "  /tools              toggle tool use (now: %s)\n"
           "  /web                toggle web search (now: %s)\n"
           "  /system [text]      show or set the system prompt ('-' removes it)\n"
           "  /trim <n>           keep only the last n messages\n"
           "  /retry              regenerate the last reply\n"
           "  /undo               drop the last exchange\n"
           "  /copy               copy the last reply to the clipboard\n"
           "  /paste              compose a multi-line message (end with '.')\n"
           "  /chats              list the named chats\n"
           "  /switch <name>      switch to a named chat (creates it if new)\n"
           "  /rename <name>      name (or rename) the current chat\n"
           "  /delete <name>      delete a named chat\n"
           "  /save <file>        save the conversation\n"
           "  /load <file>        load a conversation\n"
           "  /new                start a new conversation\n"
           "  /help               this help\n"
           "  /quit               quit (also Ctrl-D)\n"
           "  !cmd                run a shell command (output shown to you)\n"
           "  !!cmd               run a shell command, add its output to the chat\n",
           tools_on ? "on" : "off", web_on ? "on" : "off");
}

typedef struct {
    const provider_t *pv;
    const char *provider_name; /* config section name; "" = PIKI_BASE_URL */
    char *model;
    size_t modelcap;
    chat_t *chat;
    send_limits lim;
    long max_agent_steps;
    int tools_on;
    int web;
    long sent_total;   /* prompt tokens accumulated this session */
    long recv_total;   /* completion tokens accumulated this session */
    int usage_est;     /* totals include estimated counts (shown as ~) */
    const char *session_path;  /* NULL disables autosave */
    int save_warned;           /* only complain once */
    char chat_name[64];        /* named chat under chats/; "" = unnamed */
} repl_state;

static void config_path(char *out, size_t cap, const char *name);

/* Builds the path of a named chat: <config>/chats/<name>.json. Creates the
 * chats directory on the way (best effort). Empty on failure. */
static void chats_path(char *out, size_t cap, const char *name)
{
    char dir[1060];

    config_path(dir, sizeof dir, "chats");
    if (!dir[0]) {
        out[0] = '\0';
        return;
    }
    mkdir(dir, 0700);
    snprintf(out, cap, "%s/%s.json", dir, name);
}

/* Chat names become file names: keep them to a safe charset. */
static int chat_name_ok(const char *s)
{
    size_t i;

    if (!*s || strlen(s) >= 64)
        return 0;
    for (i = 0; s[i]; i++) {
        if (!isalnum((unsigned char)s[i]) &&
            s[i] != '-' && s[i] != '_' && s[i] != '.')
            return 0;
    }
    return s[0] != '.';   /* no hidden files, no ".." */
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Lists the chats saved under chats/, marking the active one with '*'. */
static void list_chats(const repl_state *st)
{
    char dir[1060];
    DIR *d;
    struct dirent *e;
    char **names = NULL;
    size_t n = 0, cap = 0, i;

    config_path(dir, sizeof dir, "chats");
    d = dir[0] ? opendir(dir) : NULL;
    if (d) {
        while ((e = readdir(d)) != NULL) {
            size_t len = strlen(e->d_name);

            if (len <= 5 || strcmp(e->d_name + len - 5, ".json") != 0)
                continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                names = realloc(names, cap * sizeof *names);
                if (!names)
                    abort();   /* same OOM policy as buf */
            }
            names[n] = malloc(len - 4);
            if (!names[n])
                abort();
            memcpy(names[n], e->d_name, len - 5);
            names[n][len - 5] = '\0';
            n++;
        }
        closedir(d);
    }
    if (!n) {
        printf("%sno named chats yet (/switch <name> starts one)%s\n",
               C_DIM, C_RESET);
    } else {
        qsort(names, n, sizeof *names, cmp_str);
        for (i = 0; i < n; i++) {
            printf("%c %s\n",
                   strcmp(names[i], st->chat_name) == 0 ? '*' : ' ',
                   names[i]);
        }
    }
    for (i = 0; i < n; i++)
        free(names[i]);
    free(names);
}

/* Persists the conversation so a crash or a power cut does not lose it.
 * Warns at most once and never interrupts the chat. */
static void session_autosave(repl_state *st)
{
    char err[256];

    if (!st->session_path || !*st->session_path)
        return;
    if (chat_save(st->chat, st->session_path, err, sizeof err) == 0)
        return;
    if (!st->save_warned) {
        fprintf(stderr, "piki: could not autosave the session: %s\n", err);
        st->save_warned = 1;
    }
}

/* Named chats get their own file too, so /switch never loses anything. */
static void chat_autosave(repl_state *st)
{
    char path[1200], err[256];

    session_autosave(st);
    if (!st->chat_name[0])
        return;
    chats_path(path, sizeof path, st->chat_name);
    if (path[0] && chat_save(st->chat, path, err, sizeof err) != 0 &&
        !st->save_warned) {
        fprintf(stderr, "piki: could not save the chat: %s\n", err);
        st->save_warned = 1;
    }
}

/* Sends the newest user message (already added to the chat) as one model
 * turn: agent loop or streaming, usage accounting, autosave. */
static void send_user_turn(repl_state *st)
{
    token_usage tu = {0, 0};
    char err[512];
    size_t sent_bytes, reply_bytes;
    int got_reply, rc;

    /* what this turn will send, for the no-usage fallback below */
    sent_bytes = st->chat->bytes;
    if (st->lim.max_bytes && sent_bytes > st->lim.max_bytes)
        sent_bytes = st->lim.max_bytes;
    reply_bytes = 0;
    got_reply = 0;

    /* the OpenRouter plugin rides on chat/completions; the Responses API
     * is its own endpoint and takes over the whole turn */
    if (st->web && st->pv->web_kind == API_WEB_RESPONSES) {
        buf_t final;

        buf_init(&final);
        rc = responses_turn(st->pv, st->model, st->chat, st->lim,
                            &tu, &final, err, sizeof err);
        if (rc == 0) {
            chat_add(st->chat, "assistant", final.data);
            reply_bytes = final.len;
            got_reply = 1;
        } else if (rc == -2) {
            net_interrupt = 0;
            fprintf(stderr, "%s[interrupted]%s\n", C_DIM, C_RESET);
            chat_pop(st->chat);
        } else {
            fprintf(stderr, "piki: %s\n", err);
            chat_pop(st->chat);
        }
        buf_free(&final);
    } else if (st->tools_on) {
        int plugin_web = st->web &&
                         st->pv->web_kind == API_WEB_PLUGIN;
        buf_t final;

        buf_init(&final);
        rc = agent_turn(st->pv, st->model, st->chat,
                        st->lim, plugin_web, st->max_agent_steps,
                        &tu, &final, err, sizeof err);
        if (rc == 0) {
            chat_add(st->chat, "assistant", final.data);
            reply_bytes = final.len;
            got_reply = 1;
        } else if (rc == -2) {
            net_interrupt = 0;
            fprintf(stderr, "%s[interrupted]%s\n", C_DIM, C_RESET);
            chat_pop(st->chat);
        } else {
            fprintf(stderr, "piki: %s\n", err);
            chat_pop(st->chat);
        }
        buf_free(&final);
    } else {
        int plugin_web = st->web &&
                         st->pv->web_kind == API_WEB_PLUGIN;
        print_ctx pc;

        rc = stream_turn(st->pv, st->model, st->chat,
                         st->lim, plugin_web, &tu, &pc,
                         err, sizeof err);
        if (rc == 0) {
            chat_add(st->chat, "assistant", pc.acc.data);
            reply_bytes = pc.acc.len;
            got_reply = 1;
        } else if (rc == -2) {
            net_interrupt = 0;
            fprintf(stderr, "%s[interrupted]%s\n", C_DIM, C_RESET);
            if (pc.got_any) {
                chat_add(st->chat, "assistant", pc.acc.data);
                reply_bytes = pc.acc.len;
                got_reply = 1;
            } else {
                chat_pop(st->chat);
            }
        } else {
            fprintf(stderr, "piki: %s\n", err);
            chat_pop(st->chat);
        }
        buf_free(&pc.acc);
    }

    /* Providers that omit usage: fall back to per-model bytes/token
     * estimate and flag the session totals as approximate. */
    if (got_reply && tu.prompt_tokens == 0 && tu.completion_tokens == 0) {
        int bpt = bytes_per_token(st->model);
        tu.prompt_tokens = (long)(sent_bytes / (size_t)bpt);
        tu.completion_tokens = (long)(reply_bytes / (size_t)bpt);
        st->usage_est = 1;
    }
    st->sent_total += tu.prompt_tokens;
    st->recv_total += tu.completion_tokens;
    chat_autosave(st);
}

/* Processes a line starting with '/'. Returns 1 continue, 0 quit. */
static int handle_command(repl_state *st, char *line)
{
    char *cmd = line;
    char *arg = strchr(cmd, ' ');
    char err[512];
    int rc;

    if (arg) {
        *arg++ = '\0';
        while (*arg == ' ')
            arg++;
    }

    if (strcmp(cmd, "/quit") == 0 || strcmp(cmd, "/exit") == 0)
        return 0;
    if (strcmp(cmd, "/help") == 0) {
        repl_help(st->tools_on, st->web);
    } else if (strcmp(cmd, "/new") == 0) {
        chat_clear(st->chat);
        st->chat_name[0] = '\0';   /* back to the unnamed session */
        session_autosave(st);      /* keep the saved session in sync */
        printf("%snew conversation%s\n", C_DIM, C_RESET);
    } else if (strcmp(cmd, "/system") == 0) {
        if (arg && *arg) {
            if (strcmp(arg, "-") == 0) {
                chat_set_system(st->chat, NULL);
                printf("%ssystem prompt removed%s\n", C_DIM, C_RESET);
            } else {
                chat_set_system(st->chat, arg);
                printf("%ssystem prompt set%s\n", C_DIM, C_RESET);
            }
            chat_autosave(st);
        } else if (st->chat->system) {
            printf("%s%s%s\n", C_DIM, st->chat->system, C_RESET);
        } else {
            printf("%sno system prompt (/system <text> sets, "
                   "/system - removes)%s\n", C_DIM, C_RESET);
        }
    } else if (strcmp(cmd, "/trim") == 0) {
        long keep = arg && *arg ? strtol(arg, NULL, 10) : -1;

        if (keep < 0) {
            fputs("usage: /trim <n>  (keep only the last n messages)\n",
                  stderr);
        } else {
            chat_trim(st->chat, (size_t)keep);
            chat_autosave(st);
            printf("%skept the last %lu messages%s\n", C_DIM,
                   (unsigned long)st->chat->n, C_RESET);
        }
    } else if (strcmp(cmd, "/retry") == 0 || strcmp(cmd, "/undo") == 0) {
        /* Both need the tail to be a user + assistant pair; anything else
         * (empty chat, interrupted turn already popped) is a no-op. */
        chat_t *c = st->chat;

        if (c->n < 2 ||
            strcmp(c->msgs[c->n - 1].role, "assistant") != 0 ||
            strcmp(c->msgs[c->n - 2].role, "user") != 0) {
            printf("%snothing to %s%s\n", C_DIM, cmd + 1, C_RESET);
        } else if (strcmp(cmd, "/undo") == 0) {
            chat_pop(c);
            chat_pop(c);
            chat_autosave(st);
            printf("%sundid the last exchange (%lu messages left)%s\n",
                   C_DIM, (unsigned long)c->n, C_RESET);
        } else {
            chat_pop(c);   /* drop the reply, keep the user message */
            send_user_turn(st);
        }
    } else if (strcmp(cmd, "/copy") == 0) {
        /* OSC 52 clipboard write: works locally and over ssh, but not
         * every terminal supports it, so the confirmation is optimistic. */
        const char *text = NULL;
        size_t k = st->chat->n;

        while (k > 0 && !text) {
            k--;
            if (strcmp(st->chat->msgs[k].role, "assistant") == 0)
                text = st->chat->msgs[k].content;
        }
        if (!text) {
            printf("%snothing to copy%s\n", C_DIM, C_RESET);
        } else if (!term_is_tty()) {
            fputs("piki: /copy needs a terminal\n", stderr);
        } else {
            enum { COPY_MAX = 74 * 1024 };   /* ~100 KB of base64 */
            size_t len = strlen(text), off = 0;
            buf_t seq;

            if (len > COPY_MAX) {
                len = COPY_MAX;
                fprintf(stderr, "piki: reply truncated to %d KB for the "
                        "clipboard\n", COPY_MAX / 1024);
            }
            buf_init(&seq);
            buf_puts(&seq, "\033]52;c;");
            buf_b64(&seq, text, len);
            buf_putc(&seq, '\a');
            while (off < seq.len) {
                ssize_t w = write(STDOUT_FILENO, seq.data + off,
                                  seq.len - off);

                if (w <= 0)
                    break;
                off += (size_t)w;
            }
            buf_free(&seq);
            printf("%scopied %lu bytes to the clipboard%s\n", C_DIM,
                   (unsigned long)len, C_RESET);
        }
    } else if (strcmp(cmd, "/paste") == 0) {
        buf_t msg;
        char tmp[4096];

        printf("%spaste the message; end with a single '.' line%s\n",
               C_DIM, C_RESET);
        buf_init(&msg);
        while (fgets(tmp, sizeof tmp, stdin)) {
            if (strcmp(tmp, ".\n") == 0 || strcmp(tmp, ".") == 0)
                break;
            buf_puts(&msg, tmp);
        }
        while (msg.len && msg.data[msg.len - 1] == '\n')
            msg.data[--msg.len] = '\0';
        if (msg.len) {
            chat_add(st->chat, "user", msg.data);
            send_user_turn(st);
        } else {
            printf("%snothing to send%s\n", C_DIM, C_RESET);
        }
        buf_free(&msg);
    } else if (strcmp(cmd, "/chats") == 0) {
        list_chats(st);
    } else if (strcmp(cmd, "/switch") == 0) {
        if (!arg || !*arg || !chat_name_ok(arg)) {
            fputs("usage: /switch <name>  (letters, digits, - _ .)\n",
                  stderr);
        } else {
            char path[1200];

            chat_autosave(st);   /* do not lose the chat we leave */
            chats_path(path, sizeof path, arg);
            if (!path[0]) {
                fputs("piki: no config directory\n", stderr);
                return 1;
            }
            if (chat_load(st->chat, path, err, sizeof err) == 0) {
                printf("%sswitched to %s (%lu messages)%s\n", C_DIM, arg,
                       (unsigned long)st->chat->n, C_RESET);
            } else {
                chat_clear(st->chat);
                printf("%snew chat %s%s\n", C_DIM, arg, C_RESET);
            }
            snprintf(st->chat_name, sizeof st->chat_name, "%s", arg);
            chat_autosave(st);
        }
    } else if (strcmp(cmd, "/rename") == 0) {
        if (!arg || !*arg || !chat_name_ok(arg)) {
            fputs("usage: /rename <name>  (letters, digits, - _ .)\n",
                  stderr);
        } else {
            char oldp[1200];

            if (st->chat_name[0]) {   /* drop the file under the old name */
                chats_path(oldp, sizeof oldp, st->chat_name);
                if (oldp[0])
                    remove(oldp);
            }
            snprintf(st->chat_name, sizeof st->chat_name, "%s", arg);
            chat_autosave(st);
            printf("%sthis chat is now %s%s\n", C_DIM, arg, C_RESET);
        }
    } else if (strcmp(cmd, "/delete") == 0) {
        if (!arg || !*arg || !chat_name_ok(arg)) {
            fputs("usage: /delete <name>\n", stderr);
        } else if (strcmp(arg, st->chat_name) == 0) {
            fputs("piki: that chat is active; /switch away first\n", stderr);
        } else {
            char path[1200];

            chats_path(path, sizeof path, arg);
            if (path[0] && remove(path) == 0)
                printf("%sdeleted %s%s\n", C_DIM, arg, C_RESET);
            else
                fprintf(stderr, "piki: no chat named %s\n", arg);
        }
    } else if (strcmp(cmd, "/tools") == 0) {
        st->tools_on = !st->tools_on;
        printf("%stool use: %s%s\n", C_DIM,
               st->tools_on ? "on" : "off", C_RESET);
    } else if (strcmp(cmd, "/web") == 0) {
        if (!st->web && st->pv->web_kind == API_WEB_NONE) {
            printf("%s%s has no server-side web search%s\n",
                   C_DIM, st->pv->host, C_RESET);
        } else {
            st->web = !st->web;
            printf("%sweb search: %s%s\n", C_DIM,
                   st->web ? "on" : "off", C_RESET);
            if (st->web && st->pv->web_kind == API_WEB_RESPONSES)
                printf("%sweb turns use the Responses API: no streaming "
                       "and no local tools while on%s\n", C_DIM, C_RESET);
        }
    } else if (strcmp(cmd, "/model") == 0) {
        if (arg && *arg) {
            if (strcmp(arg, "save") == 0) {
                char serr[256];

                if (config_save_model(st->provider_name, st->model,
                                      serr, sizeof serr) == 0)
                    printf("%sdefault model saved: %s%s\n", C_DIM,
                           st->model, C_RESET);
                else
                    fprintf(stderr, "piki: %s\n", serr);
            } else {
                char *sp = strrchr(arg, ' ');
                int do_save = 0;
                const char *model_arg = arg;

                if (sp && strcmp(sp + 1, "save") == 0) {
                    /* "model save" -> change and persist */
                    *sp = '\0';
                    model_arg = arg;
                    /* trim trailing spaces from model_arg */
                    {
                        size_t ml = strlen(model_arg);
                        while (ml && model_arg[ml - 1] == ' ')
                            ml--;
                        ((char *)model_arg)[ml] = '\0';
                    }
                    if (!*model_arg) {
                        fputs("piki: model name missing\n", stderr);
                        return 1;
                    }
                    do_save = 1;
                }
                if (strlen(model_arg) < st->modelcap) {
                    strcpy(st->model, model_arg);
                    printf("%smodel: %s%s\n", C_DIM, st->model, C_RESET);
                    if (do_save) {
                        char serr[256];

                        if (config_save_model(st->provider_name,
                                              st->model, serr,
                                              sizeof serr) == 0)
                            printf("%sdefault model saved: %s%s\n", C_DIM,
                                   st->model, C_RESET);
                        else
                            fprintf(stderr, "piki: %s\n", serr);
                    }
                } else {
                    fputs("piki: model name too long\n", stderr);
                }
            }
        } else {
            printf("%scurrent model: %s%s\n", C_DIM, st->model, C_RESET);
        }
    } else if (strcmp(cmd, "/default") == 0 ||
               strcmp(cmd, "/save-model") == 0) {
        if (arg && *arg) {
            if (strlen(arg) < st->modelcap) {
                char serr[256];

                strcpy(st->model, arg);
                printf("%smodel: %s%s\n", C_DIM, st->model, C_RESET);
                if (config_save_model(st->provider_name, st->model,
                                      serr, sizeof serr) == 0)
                    printf("%sdefault model saved: %s%s\n", C_DIM,
                           st->model, C_RESET);
                else
                    fprintf(stderr, "piki: %s\n", serr);
            } else {
                fputs("piki: model name too long\n", stderr);
            }
        } else {
            char serr[256];

            if (config_save_model(st->provider_name, st->model,
                                      serr, sizeof serr) == 0)
                printf("%sdefault model saved: %s%s\n", C_DIM,
                       st->model, C_RESET);
            else
                fprintf(stderr, "piki: %s\n", serr);
        }
    } else if (strcmp(cmd, "/models") == 0) {
        buf_t list;

        buf_init(&list);
        rc = api_models(st->pv, &list, err, sizeof err);
        if (rc == -2) {
            net_interrupt = 0;
            fputs("piki: interrupted\n", stderr);
        } else if (rc != 0) {
            fprintf(stderr, "piki: %s\n", err);
        } else {
            fwrite(list.data, 1, list.len, stdout);
        }
        buf_free(&list);
    } else if (strcmp(cmd, "/save") == 0) {
        if (!arg || !*arg) {
            fputs("usage: /save <file>\n", stderr);
        } else if (chat_save(st->chat, arg, err, sizeof err) == 0) {
            printf("%ssaved to %s%s\n", C_DIM, arg, C_RESET);
        } else {
            fprintf(stderr, "piki: %s\n", err);
        }
    } else if (strcmp(cmd, "/export") == 0) {
        if (!arg || !*arg) {
            fputs("usage: /export <file.md>\n", stderr);
        } else if (chat_export_md(st->chat, arg, err, sizeof err) == 0) {
            printf("%sexported to %s%s\n", C_DIM, arg, C_RESET);
        } else {
            fprintf(stderr, "piki: %s\n", err);
        }
    } else if (strcmp(cmd, "/load") == 0) {
        if (!arg || !*arg) {
            fputs("usage: /load <file>\n", stderr);
        } else if (chat_load(st->chat, arg, err, sizeof err) == 0) {
            session_autosave(st);
            printf("%sloaded %s (%lu messages)%s\n", C_DIM, arg,
                   (unsigned long)st->chat->n, C_RESET);
        } else {
            fprintf(stderr, "piki: %s\n", err);
        }
    } else {
        fprintf(stderr, "piki: unknown command %s (/help)\n", cmd);
    }
    return 1;
}

/* Runs cmd through the shell, streaming its output (stdout+stderr) live to
 * the terminal. If capture is non-NULL, also appends the output there.
 * Returns the command's exit code, or -1 if it could not be started. */
static int run_shell(const char *cmd, buf_t *capture)
{
    buf_t full;
    FILE *p;
    char tmp[4096];
    size_t n;
    int status;

    buf_init(&full);
    buf_printf(&full, "%s 2>&1", cmd);   /* merge stderr so it is captured too */
    p = popen(full.data, "r");
    buf_free(&full);
    if (!p) {
        fprintf(stderr, "piki: could not run the command\n");
        return -1;
    }
    while ((n = fread(tmp, 1, sizeof tmp, p)) > 0) {
        fwrite(tmp, 1, n, stdout);
        fflush(stdout);
        if (capture)
            buf_append(capture, tmp, n);
    }
    status = pclose(p);
    if (status != -1 && WIFEXITED(status))
        return WEXITSTATUS(status);
    return status;
}

/* Handles a REPL line starting with '!'. '!cmd' runs and shows the output;
 * '!!cmd' also appends it to the conversation as a user message. */
static void handle_bang(repl_state *st, char *line)
{
    int to_context = 0;
    char *cmd = line + 1;   /* skip the first '!' */
    buf_t out;
    int rc;

    if (*cmd == '!') {      /* '!!' -> also into the chat context */
        to_context = 1;
        cmd++;
    }
    while (*cmd == ' ')
        cmd++;
    if (!*cmd) {
        fputs("usage: !cmd (run) or !!cmd (run and add output to the chat)\n",
              stderr);
        return;
    }

    buf_init(&out);
    rc = run_shell(cmd, to_context ? &out : NULL);

    if (to_context) {
        buf_t msg;

        buf_init(&msg);
        buf_printf(&msg, "$ %s\n%.*s\n[exit %d]",
                   cmd, (int)out.len, out.data ? out.data : "", rc);
        chat_add(st->chat, "user", msg.data);
        printf("%s[output added to the chat]%s\n", C_DIM, C_RESET);
        buf_free(&msg);
    }
    buf_free(&out);
    net_interrupt = 0;   /* clear in case Ctrl-C hit the command */
}

/* REPL commands offered by Tab completion. */
static const char *const REPL_COMMANDS[] = {
    "/model", "/models", "/default", "/save-model", "/tools", "/web",
    "/system", "/trim", "/retry", "/undo", "/copy", "/paste",
    "/chats", "/switch", "/rename", "/delete",
    "/save", "/load", "/export", "/new", "/help", "/quit", NULL
};

static void complete_files(const char *prefix, char ***out, size_t *n)
{
    const char *slash = strrchr(prefix, '/');
    char dir[1024], base[1024];
    DIR *d;
    struct dirent *e;

    if (slash) {
        size_t dl = (size_t)(slash - prefix) + 1;
        if (dl >= sizeof dir) return;
        memcpy(dir, prefix, dl);
        dir[dl] = '\0';
        snprintf(base, sizeof base, "%s", slash + 1);
    } else {
        snprintf(dir, sizeof dir, ".");
        snprintf(base, sizeof base, "%s", prefix);
    }
    d = opendir(dir[0] ? dir : ".");
    if (!d) return;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && base[0] != '.') continue;
        if (strncmp(e->d_name, base, strlen(base)) != 0) continue;
        char full[2048];
        if (slash) snprintf(full, sizeof full, "%.*s%s", (int)(slash - prefix + 1), prefix, e->d_name);
        else snprintf(full, sizeof full, "%s", e->d_name);
        /* append slash for directories */
        char path[2048];
        if (dir[0]) snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        else snprintf(path, sizeof path, "%s", e->d_name);
        struct stat st;
        int isdir = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
        char *cand;
        if (isdir) {
            size_t l = strlen(full);
            cand = malloc(l + 2);
            if (!cand) continue;
            memcpy(cand, full, l);
            cand[l] = '/';
            cand[l+1] = '\0';
        } else {
            cand = strdup(full);
            if (!cand) continue;
        }
        char **tmp = realloc(*out, (*n + 1) * sizeof **out);
        if (!tmp) { free(cand); continue; }
        *out = tmp;
        (*out)[(*n)++] = cand;
    }
    closedir(d);
}

static void complete_chats(const char *prefix, char ***out, size_t *n)
{
    char dir[1200];
    DIR *d;
    struct dirent *e;
    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_CONFIG_HOME");

    if (xdg && *xdg) snprintf(dir, sizeof dir, "%s/piki/chats", xdg);
    else if (home) snprintf(dir, sizeof dir, "%s/.config/piki/chats", home);
    else return;
    d = opendir(dir);
    if (!d) return;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        size_t l = strlen(e->d_name);
        if (l > 5 && strcmp(e->d_name + l - 5, ".json") == 0) {
            char name[256];
            size_t nl = l - 5;
            if (nl >= sizeof name) continue;
            memcpy(name, e->d_name, nl);
            name[nl] = '\0';
            if (strncmp(name, prefix, strlen(prefix)) != 0) continue;
            char *cand = strdup(name);
            if (!cand) continue;
            char **tmp = realloc(*out, (*n + 1) * sizeof **out);
            if (!tmp) { free(cand); continue; }
            *out = tmp;
            (*out)[(*n)++] = cand;
        }
    }
    closedir(d);
}

/* Tab completer: bare command or second token (path/chat). */
static void complete_command(const char *line, char ***out, size_t *n,
                             void *user)
{
    const char *sp;
    size_t len;

    (void)user;
    *out = NULL;
    *n = 0;
    if (!line || line[0] != '/') return;
    sp = strchr(line, ' ');
    if (!sp) {
        len = strlen(line);
        for (size_t i = 0; REPL_COMMANDS[i]; i++) {
            if (strncmp(REPL_COMMANDS[i], line, len) != 0) continue;
            char **tmp = realloc(*out, (*n + 1) * sizeof **out);
            if (!tmp) { *n = 0; return; }
            *out = tmp;
            (*out)[*n] = strdup(REPL_COMMANDS[i]);
            if ((*out)[*n]) (*n)++;
        }
        return;
    }
    /* second token — dispatch by command */
    size_t cmdlen = (size_t)(sp - line);
    char cmd[32];
    if (cmdlen >= sizeof cmd) return;
    memcpy(cmd, line, cmdlen);
    cmd[cmdlen] = '\0';
    const char *arg = sp + 1;
    while (*arg == ' ') arg++;
    if (strcmp(cmd, "/save") == 0 || strcmp(cmd, "/load") == 0 || strcmp(cmd, "/export") == 0) {
        complete_files(arg, out, n);
    } else if (strcmp(cmd, "/switch") == 0 || strcmp(cmd, "/rename") == 0 || strcmp(cmd, "/delete") == 0) {
        complete_chats(arg, out, n);
    }
}

/* Compact count: exact below 10k, then 12.3k / 1.2M. Integer math only. */
static void fmt_count(char *out, size_t cap, long v)
{
    if (v < 10000)
        snprintf(out, cap, "%ld", v);
    else if (v < 1000000)
        snprintf(out, cap, "%ld.%ldk", v / 1000, (v % 1000) / 100);
    else
        snprintf(out, cap, "%ld.%ldM", v / 1000000, (v % 1000000) / 100000);
}

/* Last `keep` components of path, prefixed with "..."; falls back to the
 * original when that would not actually be shorter. */
static const char *tail_path(const char *path, int keep,
                             char *buf, size_t cap)
{
    const char *p = path + strlen(path);
    int seen = 0;

    while (p > path) {
        p--;
        if (*p == '/' && ++seen == keep) {
            snprintf(buf, cap, "...%s", p);
            return strlen(buf) < strlen(path) ? buf : path;
        }
    }
    return path;
}

/* Status line above each prompt: cwd, model, active modes, context use and
 * session tokens. Degrades (model vendor, then path depth) until it fits in
 * one terminal line -- wrapping would eat two rows of an 80x25 console. */
static void print_status(const repl_state *st)
{
    char cwd[1024], rel[1040], p2[1040], p1[1040];
    char up[24], dn[24], ctx[96];
    const char *home = getenv("HOME");
    const char *dir, *dirs[3], *models[2], *est;
    buf_t line;
    int width = term_width();
    size_t i, j, k, ctx_invis, vis;

    if (!getcwd(cwd, sizeof cwd))
        snprintf(cwd, sizeof cwd, "?");
    dir = cwd;
    if (home && *home) {
        size_t hl = strlen(home);

        if (strncmp(cwd, home, hl) == 0 &&
            (cwd[hl] == '/' || cwd[hl] == '\0')) {
            snprintf(rel, sizeof rel, "~%s", cwd + hl);
            dir = rel;
        }
    }
    dirs[0] = dir;
    dirs[1] = tail_path(dir, 2, p2, sizeof p2);
    dirs[2] = tail_path(dir, 1, p1, sizeof p1);

    models[0] = st->model;
    models[1] = strchr(st->model, '/') ? strchr(st->model, '/') + 1
                                       : st->model;

    /* '~' marks totals that include estimated counts (provider sent none) */
    est = st->usage_est ? "~" : "";
    fmt_count(up, sizeof up, st->sent_total);
    fmt_count(dn, sizeof dn, st->recv_total);

    /* How full the context is, so truncation never comes as a surprise.
     * Near the limit the gauge turns yellow (75%) then red (90%); the
     * escape codes add invisible bytes that the width math must ignore. */
    ctx[0] = '\0';
    ctx_invis = 0;
    if (st->lim.max_bytes) {
        char used[24], budget[24];
        const char *warn = "";

        if (use_color && st->chat->bytes >= st->lim.max_bytes / 10 * 9)
            warn = "\033[31m";                    /* red */
        else if (use_color && st->chat->bytes >= st->lim.max_bytes / 4 * 3)
            warn = "\033[33m";                    /* yellow */
        fmt_count(used, sizeof used, (long)(st->chat->bytes / 4));
        fmt_count(budget, sizeof budget, (long)(st->lim.max_bytes / 4));
        if (*warn) {
            /* reset+dim afterwards, back to the rest of the line's look */
            snprintf(ctx, sizeof ctx, " | %sctx %s/%s\033[0m\033[2m",
                     warn, used, budget);
            ctx_invis = strlen(warn) + strlen("\033[0m\033[2m");
        } else {
            snprintf(ctx, sizeof ctx, " | ctx %s/%s", used, budget);
        }
    }

    /* Degrade in order of what we can most afford to lose: path depth,
     * then the model vendor, and only then the context gauge. */
    buf_init(&line);
    vis = 0;
    for (k = 0; k < 2; k++) {
        for (i = 0; i < 2; i++) {
            for (j = 0; j < 3; j++) {
                buf_reset(&line);
                buf_printf(&line, "%s | %s%s%s%s | up %s%s dn %s%s",
                           dirs[j], models[i],
                           st->tools_on ? " [tools]" : "",
                           st->web ? " [web]" : "",
                           k ? "" : ctx, est, up, est, dn);
                vis = line.len - (k ? 0 : ctx_invis);
                if ((int)vis <= width)
                    goto fits;
            }
        }
    }
fits:
    /* Last resort: hard-truncate rather than wrap (only reachable with the
     * gauge already dropped, so no escape code can be cut in half). */
    printf("%s%.*s%s\n", C_DIM,
           vis <= (size_t)width ? (int)line.len : width,
           line.data, C_RESET);
    buf_free(&line);
}

static void run_repl(repl_state *st, history *h)
{
    buf_t line;

    buf_init(&line);
    if (term_is_tty())
        printf("%spiki %s — %s via %s%s%s — /help%s\n",
               C_DIM, PIKI_VERSION, st->model, st->pv->host,
               st->tools_on ? " [tools]" : "",
               st->web ? " [web]" : "", C_RESET);

    for (;;) {
        const char *prompt = st->web
            ? (use_color ? "\033[1;36mweb> \033[0m" : "web> ")
            : C_PROMPT;
        int rc;

        if (term_is_tty())
            print_status(st);
        rc = term_readline(prompt, &line, h, complete_command, NULL);

        if (rc == 0)
            break;               /* Ctrl-D */
        if (rc < 0) {
            net_interrupt = 0;
            continue;            /* Ctrl-C at the prompt */
        }
        if (!line.len)
            continue;

        if (line.data[0] == '!') {
            hist_add(h, line.data);
            handle_bang(st, line.data);
            continue;
        }

        if (line.data[0] == '/') {
            hist_add(h, line.data);
            if (!handle_command(st, line.data))
                break;
            continue;
        }

        hist_add(h, line.data);
        chat_add(st->chat, "user", line.data);
        send_user_turn(st);
    }
    buf_free(&line);
}

/* --- update check ----------------------------------------------------- */

/* Parses "x.y.z". Returns 0 and fills the parts, or -1 if malformed. */
static int version_parse(const char *s, long v[3])
{
    char dot1, dot2, end;

    if (sscanf(s, "%ld%c%ld%c%ld%c", &v[0], &dot1, &v[1], &dot2, &v[2],
               &end) != 5 || dot1 != '.' || dot2 != '.')
        return -1;
    return 0;
}

/* >0 if a is newer than b, 0 equal, <0 older. Malformed counts as older. */
static int version_cmp(const char *a, const char *b)
{
    long va[3], vb[3];
    int i;

    if (version_parse(a, va) < 0)
        return -1;
    if (version_parse(b, vb) < 0)
        return 1;
    for (i = 0; i < 3; i++) {
        if (va[i] != vb[i])
            return va[i] > vb[i] ? 1 : -1;
    }
    return 0;
}

/* Prints a dim notice when the last release seen is newer than this build.
 * The releases page is only consulted by a detached child process, at most
 * once a day, caching the answer in <config>/latest-release: startup never
 * waits on the network, and with no internet nothing is ever shown. */
static void update_check(void)
{
    char path[1060], latest[32] = "";
    struct stat sb;
    FILE *f;
    pid_t pid;

    config_path(path, sizeof path, "latest-release");
    if (!path[0])
        return;

    f = fopen(path, "r");
    if (f) {
        if (fgets(latest, sizeof latest, f))
            latest[strcspn(latest, "\n")] = '\0';
        fclose(f);
        if (version_cmp(latest, PIKI_VERSION) > 0)
            printf("%spiki %s is available: %s/releases%s\n",
                   C_DIM, latest, PIKI_REPO, C_RESET);
    }

    /* refresh at most once a day; mtime moves even on failure, so a
     * machine without internet retries daily, not on every start */
    if (stat(path, &sb) == 0 && time(NULL) - sb.st_mtime < 24 * 3600)
        return;

    /* double fork: the grandchild is orphaned (init reaps it), so the
     * REPL neither waits for the network nor leaves a zombie behind */
    pid = fork();
    if (pid < 0)
        return;
    if (pid == 0) {
        if (fork() == 0) {
            char ver[32], err[256];

            /* detach from the terminal: the fetch must survive the user
             * closing piki (and the session) before it finishes */
            setsid();
            signal(SIGHUP, SIG_IGN);
            freopen("/dev/null", "w", stderr);   /* silence, always */
            if (api_latest_version("juanknebel/piki-ai", ver, sizeof ver,
                                   err, sizeof err) != 0)
                snprintf(ver, sizeof ver, "%s", latest);   /* keep old */
            f = fopen(path, "w");
            if (f) {
                fprintf(f, "%s\n", ver);
                fclose(f);
            }
        }
        _exit(0);
    }
    waitpid(pid, NULL, 0);
}

/* --- history path ---------------------------------------------------- */

/* Builds the path of a file under the config directory. Empty on failure. */
static void config_path(char *out, size_t cap, const char *name)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");

    if (xdg && *xdg)
        snprintf(out, cap, "%s/piki/%s", xdg, name);
    else if (home && *home)
        snprintf(out, cap, "%s/.config/piki/%s", home, name);
    else
        out[0] = '\0';
}

/* Creates the config directory if needed, so the history and session files
 * can be written even when the user has no config file. Best effort. */
static void ensure_config_dir(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char path[512];

    if (xdg && *xdg) {
        mkdir(xdg, 0700);       /* the parent may not exist either */
        snprintf(path, sizeof path, "%s/piki", xdg);
    } else if (home && *home) {
        snprintf(path, sizeof path, "%s/.config", home);
        mkdir(path, 0700);
        snprintf(path, sizeof path, "%s/.config/piki", home);
    } else {
        return;
    }
    mkdir(path, 0700);
}

/* Slurps piped stdin up to cap bytes (truncates with a warning past
 * that; the send window truncates further anyway). -1 on read error. */
static int read_stdin_all(buf_t *out, size_t cap)
{
    char tmp[8192];
    size_t n;

    while ((n = fread(tmp, 1, sizeof tmp, stdin)) > 0) {
        if (out->len + n >= cap) {
            buf_append(out, tmp, cap - out->len);
            fprintf(stderr, "piki: stdin truncated to %lu KB\n",
                    (unsigned long)(cap / 1024));
            break;
        }
        buf_append(out, tmp, n);
    }
    return ferror(stdin) ? -1 : 0;
}

int main(int argc, char **argv)
{
    static char model[128] = "";
    const char *system_prompt = NULL;
    const char *provider_name = NULL;
    const char *question = NULL;
    const char *env_key, *base_url;
    const char *prov_model;
    const char *prov_web;
    char prov_name[32];
    config_t cfg;
    provider_t pv;
    chat_t chat;
    buf_t pipe_in;
    send_limits lim;
    struct sigaction sa;
    char err[512];
    int tools_on = -1;   /* unset: on in the REPL, off in one-shot mode */
    int web = 0;
    int resume = 0;
    int i, rc;

    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("piki %s\n"
                   "Lightweight terminal LLM chat client.\n"
                   "Author: %s\n"
                   "%s\n",
                   PIKI_VERSION, PIKI_AUTHOR, PIKI_REPO);
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            snprintf(model, sizeof model, "%s", argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            system_prompt = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            provider_name = argv[++i];
        } else if (strcmp(argv[i], "-t") == 0) {
            tools_on = 1;
        } else if (strcmp(argv[i], "-w") == 0) {
            web = 1;
        } else if (strcmp(argv[i], "--resume") == 0) {
            resume = 1;
        } else {
            usage();
            return 2;
        }
    }
    if (i == argc - 1)
        question = argv[i];
    else if (i != argc) {
        usage();
        return 2;
    }

    /* Piped stdin is one-shot input: alone it IS the question; with an
     * argv question it is appended as context ("explain this" < log). */
    buf_init(&pipe_in);
    if (!isatty(STDIN_FILENO)) {
        if (read_stdin_all(&pipe_in, 1024 * 1024) < 0) {
            fputs("piki: error reading stdin\n", stderr);
            return 1;
        }
        while (pipe_in.len && pipe_in.data[pipe_in.len - 1] == '\n')
            pipe_in.data[--pipe_in.len] = '\0';
        if (pipe_in.len && question) {
            buf_t q;

            buf_init(&q);
            buf_puts(&q, question);
            buf_puts(&q, "\n\n");
            buf_append(&q, pipe_in.data, pipe_in.len);
            buf_free(&pipe_in);
            pipe_in = q;
        }
        if (pipe_in.len)
            question = pipe_in.data;
        else if (!question) {
            usage();
            return 2;
        }
    }

    {
        /* no-color.org: any non-empty NO_COLOR disables color output */
        const char *nc = getenv("NO_COLOR");

        use_color = term_is_tty() && (!nc || !*nc);
    }

    config_defaults(&cfg);
    rc = config_load(&cfg, err, sizeof err);
    if (rc < 0) {
        fprintf(stderr, "piki: config error: %s\n", err);
        return 1;
    }

    env_key = getenv("OPENROUTER_API_KEY");
    base_url = getenv("PIKI_BASE_URL");

    pv.host = "openrouter.ai";
    pv.port = 443;
    pv.use_tls = 1;
    pv.base_path = "/api/v1";
    pv.api_key = NULL;

    prov_model = NULL;
    prov_web = NULL;
    prov_name[0] = '\0';
    if (base_url && *base_url) {
        if (parse_base_url(base_url, &pv) < 0) {
            fputs("piki: invalid PIKI_BASE_URL\n", stderr);
            return 1;
        }
        if (env_key && *env_key)
            pv.api_key = env_key;
    } else {
        const char *name = provider_name ? provider_name :
                           cfg.default_provider[0] ?
                           cfg.default_provider : "openrouter";
        cfg_provider *cp = config_provider(&cfg, name);

        snprintf(prov_name, sizeof prov_name, "%s", name);

        if (cp) {
            if (parse_base_url(cp->url, &pv) < 0) {
                fprintf(stderr, "piki: invalid url for provider %s\n",
                        name);
                return 1;
            }
            if (cp->key[0])
                pv.api_key = cp->key;
            if (cp->model[0])
                prov_model = cp->model;
            if (cp->web_search[0])
                prov_web = cp->web_search;
        } else if (strcmp(name, "openrouter") != 0) {
            fprintf(stderr, "piki: provider %s is not in the config\n",
                    name);
            return 1;
        }
        if (strcmp(name, "openrouter") == 0 && env_key && *env_key)
            pv.api_key = env_key;

        if (pv.use_tls && !pv.api_key) {
            fprintf(stderr, "piki: missing API key for provider %s "
                    "(config or OPENROUTER_API_KEY)\n", name);
            return 1;
        }
    }

    pv.web_kind = provider_web_kind(pv.host, prov_web);

    /* model precedence: -m > provider's model > built-in */
    if (!model[0])
        snprintf(model, sizeof model, "%s",
                 prov_model ? prov_model : DEFAULT_MODEL);

    if (web && pv.web_kind == API_WEB_NONE) {
        fprintf(stderr, "piki: -w ignored: %s has no server-side "
                "web search\n", pv.host);
        web = 0;
    }

    /* quick health probe for local providers: 1s connect, non-blocking */
    {
        int fd = -1;
        struct addrinfo hints, *res = NULL;
        char portstr[16];
        int ok = 0;

        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        snprintf(portstr, sizeof portstr, "%d", pv.port);
        if (getaddrinfo(pv.host, portstr, &hints, &res) == 0 && res) {
            fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            if (fd >= 0) {
                int flags = fcntl(fd, F_GETFL, 0);
                fcntl(fd, F_SETFL, flags | O_NONBLOCK);
                if (connect(fd, res->ai_addr, res->ai_addrlen) == 0) ok = 1;
                else if (errno == EINPROGRESS) {
                    fd_set wf;
                    struct timeval tv = {1, 0};
                    FD_ZERO(&wf); FD_SET(fd, &wf);
                    if (select(fd + 1, NULL, &wf, NULL, &tv) > 0) {
                        int err = 0; socklen_t el = sizeof err;
                        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
                        if (err == 0) ok = 1;
                    }
                }
                close(fd);
            }
            freeaddrinfo(res);
        }
        if (!ok && term_is_tty()) {
            fprintf(stderr, "%swarning: provider %s:%d unreachable%s\n",
                    C_DIM, pv.host, pv.port, C_RESET);
        }
    }

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);

    chat_init(&chat);
    /* cap what we KEEP in RAM (matters on 1 GB machines) */
    chat_set_max_bytes(&chat, (size_t)cfg.max_memory * 1024);
    /* cap what we SEND: no tokenizer, so estimate per-model bytes per token */
    lim.max_msgs = (size_t)cfg.max_history;
    lim.max_bytes = (size_t)cfg.max_context_tokens * (size_t)bytes_per_token(model);

    if (system_prompt)
        chat_set_system(&chat, system_prompt);
    else if (cfg.system[0])
        chat_set_system(&chat, cfg.system);

    if (question) {
        /* one-shot: tools stay off unless -t (keeps streaming and avoids
         * y/N prompts inside scripts/pipes) */
        if (tools_on < 0)
            tools_on = 0;
        chat_add(&chat, "user", question);
        buf_free(&pipe_in);   /* chat_add copied it */
        if (web && pv.web_kind == API_WEB_RESPONSES) {
            buf_t final;

            buf_init(&final);
            rc = responses_turn(&pv, model, &chat, lim, NULL, &final,
                                err, sizeof err);
            buf_free(&final);
        } else if (tools_on) {
            buf_t final;

            buf_init(&final);
            rc = agent_turn(&pv, model, &chat, lim,
                            web && pv.web_kind == API_WEB_PLUGIN,
                            cfg.max_agent_steps, NULL, &final,
                            err, sizeof err);
            buf_free(&final);
        } else {
            print_ctx pc;

            rc = stream_turn(&pv, model, &chat, lim, web,
                             NULL, &pc, err, sizeof err);
            buf_free(&pc.acc);
        }
        chat_free(&chat);
        if (rc == -2) {
            fputs("piki: interrupted\n", stderr);
            return 130;
        }
        if (rc != 0) {
            fprintf(stderr, "piki: %s\n", err);
            return 1;
        }
        return 0;
    }

    {
        repl_state st;
        history h;
        char hpath[512], spath[512];

        if (tools_on < 0)
            tools_on = 1;    /* REPL default: tools on (/tools turns off) */
        ensure_config_dir();
        if (term_is_tty() && cfg.check_updates)
            update_check();
        hist_init(&h);
        config_path(hpath, sizeof hpath, "history");
        if (hpath[0])
            hist_load(&h, hpath);
        config_path(spath, sizeof spath, "session.json");

        if (resume && spath[0]) {
            struct stat sb;

            /* no saved session yet is not an error: just start fresh */
            if (stat(spath, &sb) == 0) {
                if (chat_load(&chat, spath, err, sizeof err) == 0) {
                    /* the file also restores its system prompt; an
                     * explicit -s is a deliberate override, so re-apply it */
                    if (system_prompt)
                        chat_set_system(&chat, system_prompt);
                    printf("%sresumed %lu messages%s\n", C_DIM,
                           (unsigned long)chat.n, C_RESET);
                } else {
                    fprintf(stderr, "piki: could not resume: %s\n", err);
                }
            }
        }

        st.pv = &pv;
        st.provider_name = prov_name;
        st.model = model;
        st.modelcap = sizeof model;
        st.chat = &chat;
        st.lim = lim;
        st.max_agent_steps = cfg.max_agent_steps;
        st.tools_on = tools_on;
        st.web = web;
        st.sent_total = 0;
        st.recv_total = 0;
        st.usage_est = 0;
        st.chat_name[0] = '\0';
        st.session_path = spath[0] ? spath : NULL;
        st.save_warned = 0;
        run_repl(&st, &h);

        if (hpath[0])
            hist_save(&h, hpath, HISTORY_MAX);
        hist_free(&h);
    }
    chat_free(&chat);
    return 0;
}

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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "api.h"
#include "buf.h"
#include "chat.h"
#include "config.h"
#include "edit.h"
#include "json.h"
#include "net.h"
#include "term.h"
#include "tools.h"
#include "version.h"

#define PIKI_AUTHOR   "Juan Knebel <juanknebel@gmail.com>"
#define PIKI_REPO     "https://github.com/juanknebel/piki-ai"
#define DEFAULT_MODEL "anthropic/claude-haiku-4.5"
#define MAX_AGENT_STEPS 12
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
          "[-t] [-w] [\"question\"]\n"
          "     with no question it enters interactive mode (/help for "
          "commands)\n"
          "     -t enables tool use (read/write files, run commands)\n"
          "     -w enables OpenRouter web search\n"
          "config: ~/.config/piki/config\n"
          "env: OPENROUTER_API_KEY, PIKI_BASE_URL\n"
          "\n" PIKI_REPO "\n", stderr);
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

/* --- streaming of a simple turn (no tools) --------------------------- */

typedef struct {
    int last_was_nl;
    int got_any;
    buf_t acc;
} print_ctx;

static int print_delta(const char *text, void *user)
{
    print_ctx *pc = user;
    size_t n = strlen(text);

    fwrite(text, 1, n, stdout);
    fflush(stdout);
    buf_puts(&pc->acc, text);
    if (n) {
        pc->last_was_nl = text[n - 1] == '\n';
        pc->got_any = 1;
    }
    return 0;
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
    rc = api_chat_stream(pv, model, win, wn, web, usage, print_delta, pc,
                         err, errlen);
    if (pc->got_any && !pc->last_was_nl) {
        putchar('\n');
        fflush(stdout);
    }
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

/* Asks y/N on stdin (line). Returns 1 if yes. */
static int confirm(const char *what)
{
    char resp[16];

    printf("%srun %s? [y/N] %s", C_BOLD, what, C_RESET);
    fflush(stdout);
    if (!fgets(resp, sizeof resp, stdin))
        return 0;
    return resp[0] == 'y' || resp[0] == 'Y' || resp[0] == 's' ||
           resp[0] == 'S';
}

/* Runs the agent loop for the last user message already added to chat.
 * Returns 0 ok, -1 error (err), -2 interrupted. Leaves the final response
 * in final (for the history). */
static int agent_turn(const provider_t *pv, const char *model,
                      const chat_t *chat, send_limits lim, int web,
                      token_usage *usage, buf_t *final,
                      char *err, size_t errlen)
{
    buf_t msgs;
    chat_msg *win;
    size_t wn, i;
    int step, ret = -1;
    int first = 1;

    if (usage) {
        usage->prompt_tokens = 0;
        usage->completion_tokens = 0;
    }

    /* initial message array from the history window */
    buf_init(&msgs);
    buf_putc(&msgs, '[');
    win = malloc((chat->n + 1) * sizeof *win);
    if (!win) {
        snprintf(err, errlen, "out of memory");
        buf_free(&msgs);
        return -1;
    }
    wn = chat_window(chat, lim.max_msgs, lim.max_bytes, win, chat->n + 1);
    for (i = 0; i < wn; i++) {
        append_msg_json(&msgs, first, win[i].role, win[i].content);
        first = 0;
    }
    free(win);

    for (step = 0; step < MAX_AGENT_STEPS; step++) {
        api_turn turn;
        size_t j;
        int rc;

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
            printf("%.*s\n", (int)turn.content.len, turn.content.data);
            fflush(stdout);
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

            if (tool_is_dangerous(tc->name))
                allowed = confirm(desc.data);

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
    snprintf(err, errlen, "the agent exceeded %d steps", MAX_AGENT_STEPS);

done:
    buf_free(&msgs);
    return ret;
}

/* --- REPL ------------------------------------------------------------- */

static void repl_help(int tools_on, int web_on)
{
    printf("commands:\n"
           "  /model [id]    show or change the model\n"
           "  /models        list the provider's models\n"
           "  /tools         toggle tool use (now: %s)\n"
           "  /web           toggle web search (now: %s)\n"
           "  /save <file>   save the conversation\n"
           "  /load <file>   load a conversation\n"
           "  /new           start a new conversation\n"
           "  /help          this help\n"
           "  /quit          quit (also Ctrl-D)\n"
           "  !cmd           run a shell command (output shown to you)\n"
           "  !!cmd          run a shell command, add its output to the chat\n",
           tools_on ? "on" : "off", web_on ? "on" : "off");
}

typedef struct {
    const provider_t *pv;
    char *model;
    size_t modelcap;
    chat_t *chat;
    send_limits lim;
    int tools_on;
    int web;
    long sent_total;   /* prompt tokens accumulated this session */
    long recv_total;   /* completion tokens accumulated this session */
} repl_state;

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
        printf("%snew conversation%s\n", C_DIM, C_RESET);
    } else if (strcmp(cmd, "/tools") == 0) {
        st->tools_on = !st->tools_on;
        printf("%stool use: %s%s\n", C_DIM,
               st->tools_on ? "on" : "off", C_RESET);
    } else if (strcmp(cmd, "/web") == 0) {
        st->web = !st->web;
        printf("%sweb search: %s%s\n", C_DIM,
               st->web ? "on" : "off", C_RESET);
    } else if (strcmp(cmd, "/model") == 0) {
        if (arg && *arg) {
            if (strlen(arg) < st->modelcap) {
                strcpy(st->model, arg);
                printf("%smodel: %s%s\n", C_DIM, st->model, C_RESET);
            } else {
                fputs("piki: model name too long\n",
                      stderr);
            }
        } else {
            printf("%scurrent model: %s%s\n", C_DIM, st->model, C_RESET);
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
    } else if (strcmp(cmd, "/load") == 0) {
        if (!arg || !*arg) {
            fputs("usage: /load <file>\n", stderr);
        } else if (chat_load(st->chat, arg, err, sizeof err) == 0) {
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
    "/model", "/models", "/tools", "/web", "/save", "/load",
    "/new", "/help", "/quit", NULL
};

/* Tab completer: when the line is a bare command token (starts with '/' and
 * has no space), returns the matching commands. */
static void complete_command(const char *line, char ***out, size_t *n,
                             void *user)
{
    size_t len = strlen(line), i, cap = 0;

    (void)user;
    *out = NULL;
    *n = 0;
    if (len == 0 || line[0] != '/' || strchr(line, ' '))
        return;
    for (i = 0; REPL_COMMANDS[i]; i++) {
        if (strncmp(REPL_COMMANDS[i], line, len) != 0)
            continue;
        if (*n == cap) {
            cap = cap ? cap * 2 : 8;
            *out = realloc(*out, cap * sizeof **out);
            if (!*out) {
                *n = 0;
                return;
            }
        }
        (*out)[*n] = strdup(REPL_COMMANDS[i]);
        if ((*out)[*n])
            (*n)++;
    }
}

/* Status line printed above each prompt: cwd, model, and session tokens. */
static void print_status(const repl_state *st)
{
    char cwd[1024];
    const char *home = getenv("HOME");
    const char *dir = cwd;
    char abbrev[1040];

    if (!getcwd(cwd, sizeof cwd))
        dir = "?";
    else if (home && *home) {
        size_t hl = strlen(home);

        if (strncmp(cwd, home, hl) == 0 &&
            (cwd[hl] == '/' || cwd[hl] == '\0')) {
            snprintf(abbrev, sizeof abbrev, "~%s", cwd + hl);
            dir = abbrev;
        }
    }
    printf("%s%s | %s%s | up %ld dn %ld%s\n",
           C_DIM, dir, st->model, st->web ? " [web]" : "",
           st->sent_total, st->recv_total, C_RESET);
}

static void run_repl(repl_state *st, history *h)
{
    buf_t line;
    char err[512];

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
        token_usage tu = {0, 0};
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

        if (st->tools_on) {
            buf_t final;

            buf_init(&final);
            rc = agent_turn(st->pv, st->model, st->chat,
                            st->lim, st->web, &tu, &final,
                            err, sizeof err);
            if (rc == 0) {
                chat_add(st->chat, "assistant", final.data);
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
            print_ctx pc;

            rc = stream_turn(st->pv, st->model, st->chat,
                             st->lim, st->web, &tu, &pc,
                             err, sizeof err);
            if (rc == 0) {
                chat_add(st->chat, "assistant", pc.acc.data);
            } else if (rc == -2) {
                net_interrupt = 0;
                fprintf(stderr, "%s[interrupted]%s\n", C_DIM, C_RESET);
                if (pc.got_any)
                    chat_add(st->chat, "assistant", pc.acc.data);
                else
                    chat_pop(st->chat);
            } else {
                fprintf(stderr, "piki: %s\n", err);
                chat_pop(st->chat);
            }
            buf_free(&pc.acc);
        }
        st->sent_total += tu.prompt_tokens;
        st->recv_total += tu.completion_tokens;
    }
    buf_free(&line);
}

/* --- history path ---------------------------------------------------- */

static void history_path(char *out, size_t cap)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");

    if (xdg && *xdg)
        snprintf(out, cap, "%s/piki/history", xdg);
    else if (home && *home)
        snprintf(out, cap, "%s/.config/piki/history", home);
    else
        out[0] = '\0';
}

int main(int argc, char **argv)
{
    static char model[128] = "";
    const char *system_prompt = NULL;
    const char *provider_name = NULL;
    const char *question = NULL;
    const char *env_key, *base_url;
    config_t cfg;
    provider_t pv;
    chat_t chat;
    send_limits lim;
    struct sigaction sa;
    char err[512];
    int tools_on = 0;
    int web = 0;
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

    use_color = term_is_tty();

    config_defaults(&cfg);
    rc = config_load(&cfg, err, sizeof err);
    if (rc < 0) {
        fprintf(stderr, "piki: config error: %s\n", err);
        return 1;
    }

    if (!model[0])
        snprintf(model, sizeof model, "%s",
                 cfg.model[0] ? cfg.model : DEFAULT_MODEL);

    env_key = getenv("OPENROUTER_API_KEY");
    base_url = getenv("PIKI_BASE_URL");

    pv.host = "openrouter.ai";
    pv.port = 443;
    pv.use_tls = 1;
    pv.base_path = "/api/v1";
    pv.api_key = NULL;

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

        if (cp) {
            if (parse_base_url(cp->url, &pv) < 0) {
                fprintf(stderr, "piki: invalid url for provider %s\n",
                        name);
                return 1;
            }
            if (cp->key[0])
                pv.api_key = cp->key;
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

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);

    chat_init(&chat);
    /* cap what we KEEP in RAM (matters on 1 GB machines) */
    chat_set_max_bytes(&chat, (size_t)cfg.max_memory * 1024);
    /* cap what we SEND: no tokenizer, so estimate ~4 bytes per token */
    lim.max_msgs = (size_t)cfg.max_history;
    lim.max_bytes = (size_t)cfg.max_context_tokens * 4;

    if (system_prompt)
        chat_set_system(&chat, system_prompt);
    else if (cfg.system[0])
        chat_set_system(&chat, cfg.system);

    if (question) {
        chat_add(&chat, "user", question);
        if (tools_on) {
            buf_t final;

            buf_init(&final);
            rc = agent_turn(&pv, model, &chat, lim, web,
                            NULL, &final, err, sizeof err);
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
        char hpath[512];

        hist_init(&h);
        history_path(hpath, sizeof hpath);
        if (hpath[0])
            hist_load(&h, hpath);

        st.pv = &pv;
        st.model = model;
        st.modelcap = sizeof model;
        st.chat = &chat;
        st.lim = lim;
        st.tools_on = tools_on;
        st.web = web;
        st.sent_total = 0;
        st.recv_total = 0;
        run_repl(&st, &h);

        if (hpath[0])
            hist_save(&h, hpath, HISTORY_MAX);
        hist_free(&h);
    }
    chat_free(&chat);
    return 0;
}

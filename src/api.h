#ifndef PIKI_API_H
#define PIKI_API_H

#include <stddef.h>

#include "buf.h"

/* How a provider exposes server-side web search. This is the extension
 * point for new providers: add a value here, a request builder + response
 * parser in api.c, and a detection case in main.c (or the provider's
 * web_search config key). E.g. Anthropic's server tools would become
 * API_WEB_ANTHROPIC with its own builder/parser. */
typedef enum {
    API_WEB_NONE = 0,   /* provider has no server-side web search */
    API_WEB_PLUGIN,     /* OpenRouter: "plugins":[{"id":"web"}] on
                           chat/completions (streams like normal chat) */
    API_WEB_RESPONSES   /* OpenAI/Meta Responses API: POST {base}/responses
                           with tools [{"type":"web_search"}] */
} api_web_kind;

/* An OpenAI-compatible endpoint: OpenRouter, and in the future Ollama /
 * llama-server (use_tls = 0, api_key = NULL) or others. */
typedef struct {
    const char *host;
    int port;
    int use_tls;
    const char *base_path;   /* e.g. "/api/v1" or "/v1" */
    const char *api_key;     /* NULL = no Authorization */
    int web_kind;            /* api_web_kind */
} provider_t;

typedef struct {
    const char *role;        /* "system" | "user" | "assistant" */
    const char *content;
} chat_msg;

/* Token accounting reported by the API (0 if the provider omits it). */
typedef struct {
    long prompt_tokens;
    long completion_tokens;
} token_usage;

/* Non-streaming chat: sends the conversation and appends the model's
 * response to out. 0 ok, -1 error (description in err), -2 interrupted. */
int api_chat(const provider_t *pv, const char *model,
             const chat_msg *msgs, size_t nmsgs,
             buf_t *out, char *err, size_t errlen);

/* Receives each text fragment as it arrives. Returns 0 to
 * continue; nonzero cuts the stream (api_chat_stream returns 0). */
typedef int (*api_on_delta)(const char *text, void *user);

/* Chat with SSE streaming. web enables OpenRouter's web-search plugin;
 * usage (may be NULL) receives the token counts for this call.
 * 0 ok, -1 error (err), -2 interrupted. */
int api_chat_stream(const provider_t *pv, const char *model,
                    const chat_msg *msgs, size_t nmsgs, int web,
                    token_usage *usage,
                    api_on_delta on_delta, void *user,
                    char *err, size_t errlen);

/* Lists the provider's models (GET {base}/models, supported by
 * OpenRouter, Ollama and llama-server): appends one id per line to out.
 * 0 ok, -1 error, -2 interrupted. */
int api_models(const provider_t *pv, buf_t *out,
               char *err, size_t errlen);

/* Fetches the latest release tag of owner_repo ("user/repo") from the
 * GitHub API into out, without the leading 'v' (e.g. "0.7.0"). Used by
 * the background update check; the caller decides what (if anything) to
 * show. 0 ok, -1 error (err), -2 interrupted. */
int api_latest_version(const char *owner_repo, char *out, size_t cap,
                       char *err, size_t errlen);

/* --- Responses API (server-side web search) --------------------------- */

/* Extracts from a non-streaming Responses API JSON body: the assistant
 * text (appended to out), one "title <url>" line per distinct
 * url_citation (appended to sources; NULL to skip) and the token usage
 * (input_tokens/output_tokens; NULL to skip). Exposed separately so the
 * parser is testable without a network. 0 ok, -1 malformed (err). */
int api_responses_parse(const char *body, buf_t *out, buf_t *sources,
                        token_usage *usage, char *err, size_t errlen);

/* One non-streaming turn through the Responses API ({base}/responses)
 * with the provider's web_search tool attached. Appends the answer to
 * out and the citations to sources (may be NULL).
 * 0 ok, -1 error (err), -2 interrupted. */
int api_responses_turn(const provider_t *pv, const char *model,
                       const chat_msg *msgs, size_t nmsgs,
                       token_usage *usage, buf_t *out, buf_t *sources,
                       char *err, size_t errlen);

/* --- tool use / agent ------------------------------------------------- */

typedef struct {
    char *id;
    char *name;
    char *arguments;   /* JSON string of arguments */
} api_tool_call;

typedef struct {
    buf_t content;             /* assistant text (may be empty) */
    api_tool_call *calls;
    size_t ncalls;
} api_turn;

void api_turn_init(api_turn *t);
void api_turn_free(api_turn *t);

/* One agent turn (non-streaming). messages_json is the complete JSON
 * array of messages; tools_json the array of tools (NULL to omit); web
 * enables OpenRouter's web-search plugin.
 * 0 ok, -1 error (err), -2 interrupted. */
int api_agent_turn(const provider_t *pv, const char *model,
                   const char *messages_json, const char *tools_json,
                   int web, token_usage *usage, api_turn *out,
                   char *err, size_t errlen);

#endif /* PIKI_API_H */

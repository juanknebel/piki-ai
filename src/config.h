#ifndef PIKI_CONFIG_H
#define PIKI_CONFIG_H

#include <stddef.h>

/* INI config in $XDG_CONFIG_HOME/piki/config (or ~/.config/piki/config):
 *
 *   [provider "openrouter"]
 *   url = https://openrouter.ai/api/v1
 *   key = sk-or-...
 *
 *   [provider "ollama"]
 *   url = http://192.168.1.10:11434/v1    # no key: plain HTTP
 *   model = llama3.2                      # default model when using it
 *
 *   [defaults]
 *   provider = openrouter
 *   system = Respond in English.
 *   max_history = 40           # messages sent per turn
 *   max_memory = 256           # KB of history kept in RAM
 *   max_context_tokens = 8000  # rough cap on what is sent per turn
 *   max_agent_steps = 12       # tool-call rounds before asking to continue
 *   check_updates = 1          # 0 disables the daily release check
 *
 *   [search]                   # client-side web search (web_search tool)
 *   engine = ddg               # ddg | brave (brave not implemented yet)
 *   key =                      # API key for engines that need one
 *
 * Lines starting with # or ; are comments. Unknown keys and sections
 * are ignored (forward compatibility). */

#define CFG_MAX_PROVIDERS 8

typedef struct {
    char name[32];
    char url[256];
    char key[256];
    char model[128];          /* default model for this provider ("" = none) */
    char web_search[16];      /* "" = detect by host;
                                 none|plugin|responses|local */
} cfg_provider;

typedef struct {
    cfg_provider providers[CFG_MAX_PROVIDERS];
    size_t nproviders;
    char default_provider[32];
    char system[1024];
    long max_history;         /* messages kept in the sent window */
    long max_memory;          /* KB of history kept in RAM */
    long max_context_tokens;  /* rough cap on what is sent per turn */
    long max_agent_steps;     /* tool rounds before asking to continue */
    int check_updates;        /* 0 disables the daily release check */
    char search_engine[16];   /* [search] engine: ddg|brave ("" = ddg) */
    char search_key[256];     /* [search] key (engines that need one) */
} config_t;

void config_defaults(config_t *c);

/* Parses the full text. 0 ok, -1 error (description with line in err). */
int config_parse(config_t *c, const char *text, char *err, size_t errlen);

/* Reads the standard path. 0 ok, 1 no file, -1 parse error. */
int config_load(config_t *c, char *err, size_t errlen);

/* Persists model as the default of the given provider section, preserving
 * the rest of the file. If the section is missing it is created only for
 * the built-in "openrouter" (any other provider must already be in the
 * config). 0 ok, -1 error (description in err). */
int config_save_model(const char *provider, const char *model,
                      char *err, size_t errlen);

cfg_provider *config_provider(config_t *c, const char *name);

#endif /* PIKI_CONFIG_H */

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
 *
 *   [defaults]
 *   provider = openrouter
 *   model = anthropic/claude-haiku-4.5
 *   system = Respond in English.
 *   max_history = 40
 *
 * Lines starting with # or ; are comments. Unknown keys and sections
 * are ignored (forward compatibility). */

#define CFG_MAX_PROVIDERS 8

typedef struct {
    char name[32];
    char url[256];
    char key[256];
} cfg_provider;

typedef struct {
    cfg_provider providers[CFG_MAX_PROVIDERS];
    size_t nproviders;
    char default_provider[32];
    char model[128];
    char system[1024];
    long max_history;
} config_t;

void config_defaults(config_t *c);

/* Parses the full text. 0 ok, -1 error (description with line in err). */
int config_parse(config_t *c, const char *text, char *err, size_t errlen);

/* Reads the standard path. 0 ok, 1 no file, -1 parse error. */
int config_load(config_t *c, char *err, size_t errlen);

cfg_provider *config_provider(config_t *c, const char *name);

#endif /* PIKI_CONFIG_H */

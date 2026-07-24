#include "config.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fails++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

static void expect_fail(const char *text)
{
    config_t c;
    char err[128];

    config_defaults(&c);
    if (config_parse(&c, text, err, sizeof err) == 0) {
        fails++;
        fprintf(stderr, "FAIL: accepted invalid config: %.50s\n",
                text);
    }
}

int main(void)
{
    config_t c;
    char err[128];

    /* complete config with comments and whitespace */
    config_defaults(&c);
    CHECK(config_parse(&c,
        "# comentario\n"
        "; otro comentario\n"
        "\n"
        "[provider \"openrouter\"]\n"
        "url = https://openrouter.ai/api/v1\n"
        "key = sk-or-abc123\n"
        "\n"
        "[provider \"ollama\"]\n"
        "  url   =   http://192.168.1.10:11434/v1  \n"
        "\n"
        "[defaults]\n"
        "provider = ollama\n"
        "model = llama3.2\n"
        "system = Responde en castellano rioplatense.\n"
        "max_history = 20\n", err, sizeof err) == 0);
    CHECK(c.nproviders == 2);
    CHECK(config_provider(&c, "openrouter") != NULL);
    CHECK(strcmp(config_provider(&c, "openrouter")->key,
                 "sk-or-abc123") == 0);
    CHECK(strcmp(config_provider(&c, "ollama")->url,
                 "http://192.168.1.10:11434/v1") == 0);
    CHECK(config_provider(&c, "ollama")->key[0] == '\0');
    CHECK(config_provider(&c, "inexistente") == NULL);
    CHECK(strcmp(c.default_provider, "ollama") == 0);
    CHECK(strcmp(c.model, "llama3.2") == 0);
    CHECK(strcmp(c.system, "Responde en castellano rioplatense.") == 0);
    CHECK(c.max_history == 20);

    /* empty: defaults remain */
    config_defaults(&c);
    CHECK(config_parse(&c, "", err, sizeof err) == 0);
    CHECK(c.nproviders == 0 && c.max_history == 40 && !c.model[0]);

    /* unknown keys and sections are ignored */
    config_defaults(&c);
    CHECK(config_parse(&c,
        "[defaults]\n"
        "clave_del_futuro = x\n"
        "[seccion_rara]\n"
        "a = b\n"
        "[defaults]\n"
        "model = gpt-x\n", err, sizeof err) == 0);
    CHECK(strcmp(c.model, "gpt-x") == 0);

    /* value with '=' inside (only splits on the first) */
    config_defaults(&c);
    CHECK(config_parse(&c,
        "[defaults]\nsystem = usa a=b como ejemplo\n",
        err, sizeof err) == 0);
    CHECK(strcmp(c.system, "usa a=b como ejemplo") == 0);

    /* malformed */
    expect_fail("clave = fuera de seccion\n");
    expect_fail("[defaults\nmodel = x\n");
    expect_fail("[provider]\nurl = x\n");
    expect_fail("[provider \"\"]\n");
    expect_fail("[defaults]\nsin igual\n");
    expect_fail("[defaults]\nmax_history = cero\n");
    expect_fail("[defaults]\nmax_history = 0\n");
    expect_fail("[provider \"nombre-absurdamente-largo-que-no-entra-"
                "en-el-campo\"]\n");

    if (fails) {
        fprintf(stderr, "test_config: %d failures\n", fails);
        return 1;
    }
    puts("test_config: OK");
    return 0;
}

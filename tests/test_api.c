#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/api.h"
#include "../src/chat.h"

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while(0)

int main(void)
{
    chat_t c;
    char err[256];
    char path[] = "/tmp/piki-test-api-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { fprintf(stderr, "mkstemp fail\n"); return 1; }
    close(fd);

    chat_init(&c);
    chat_set_system(&c, "sys prompt");
    chat_add(&c, "user", "hello");
    chat_add(&c, "assistant", "world **bold**");

    if (chat_export_md(&c, path, err, sizeof err) != 0) {
        fprintf(stderr, "export fail: %s\n", err);
        fails++;
    } else {
        FILE *f = fopen(path, "r");
        if (!f) { fprintf(stderr, "open fail %s\n", path); fails++; }
        else {
            char buf[4096];
            size_t n = fread(buf, 1, sizeof buf - 1, f);
            buf[n] = '\0';
            fclose(f);
            CHECK(strstr(buf, "# System") != NULL, "has system header");
            CHECK(strstr(buf, "## user") != NULL, "has user header");
            CHECK(strstr(buf, "## assistant") != NULL, "has assistant header");
            CHECK(strstr(buf, "hello") != NULL, "has hello");
            CHECK(strstr(buf, "world") != NULL, "has world");
        }
    }
    unlink(path);
    chat_free(&c);

    /* empty chat export */
    strcpy(path, "/tmp/piki-test-api-XXXXXX");
    fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp2"); return 1; }
    close(fd);
    chat_init(&c);
    if (chat_export_md(&c, path, err, sizeof err) != 0) {
        fprintf(stderr, "empty export fail: %s\n", err);
        fails++;
    }
    unlink(path);
    chat_free(&c);

    /* --- api_responses_parse ------------------------------------------ */
    {
        buf_t out, sources;
        token_usage u = {0, 0};

        /* text + citations + usage; web_search_call items are skipped */
        buf_init(&out);
        buf_init(&sources);
        CHECK(api_responses_parse(
            "{\"output\":["
            "{\"type\":\"web_search_call\",\"status\":\"completed\"},"
            "{\"type\":\"message\",\"role\":\"assistant\",\"content\":["
            "{\"type\":\"output_text\",\"text\":\"hello \",\"annotations\":["
            "{\"type\":\"url_citation\",\"url\":\"https://a.com\","
            "\"title\":\"A\"}]},"
            "{\"type\":\"output_text\",\"text\":\"world\",\"annotations\":["
            "{\"type\":\"url_citation\",\"url\":\"https://a.com\","
            "\"title\":\"A\"},"
            "{\"type\":\"url_citation\",\"url\":\"https://b.com\","
            "\"title\":\"B\"}]}]}],"
            "\"usage\":{\"input_tokens\":11,\"output_tokens\":7}}",
            &out, &sources, &u, err, sizeof err) == 0, "parse ok");
        CHECK(strcmp(out.data, "hello world") == 0, "joins output_text");
        CHECK(strstr(sources.data, "A <https://a.com>") != NULL,
              "has citation A");
        CHECK(strstr(sources.data, "B <https://b.com>") != NULL,
              "has citation B");
        {
            const char *first = strstr(sources.data, "https://a.com");

            CHECK(first && strstr(first + 1, "https://a.com") == NULL,
                  "citation A not duplicated");
        }
        CHECK(u.prompt_tokens == 11 && u.completion_tokens == 7,
              "usage mapped");
        buf_free(&out);
        buf_free(&sources);

        /* no output text -> error */
        buf_init(&out);
        CHECK(api_responses_parse(
            "{\"output\":[{\"type\":\"web_search_call\"}]}",
            &out, NULL, NULL, err, sizeof err) == -1, "no text is error");
        buf_free(&out);

        /* malformed json -> error */
        buf_init(&out);
        CHECK(api_responses_parse("{oops", &out, NULL, NULL,
                                  err, sizeof err) == -1,
              "malformed is error");
        buf_free(&out);
    }

    if (fails) fprintf(stderr, "test_api: %d fails\n", fails);
    else printf("test_api: OK\n");
    return fails ? 1 : 0;
}

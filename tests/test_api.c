#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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

    if (fails) fprintf(stderr, "test_api: %d fails\n", fails);
    else printf("test_api: OK\n");
    return fails ? 1 : 0;
}

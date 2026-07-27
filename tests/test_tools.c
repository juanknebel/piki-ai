#include "tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fails++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

static char dir[256];

static void path_in(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s/%s", dir, name);
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");

    if (f) {
        fwrite(content, 1, strlen(content), f);
        fclose(f);
    }
}

/* Reads a whole file into a fixed buffer; "" if it cannot be read. */
static void read_back(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n = 0;

    out[0] = '\0';
    if (!f)
        return;
    n = fread(out, 1, cap - 1, f);
    out[n] = '\0';
    fclose(f);
}

/* Runs a tool with the given JSON args and returns its output buffer. */
static int run(const char *name, const char *args, buf_t *out)
{
    buf_init(out);
    return tool_run(name, args, out);
}

int main(void)
{
    char p[512], args[1024], back[512];
    buf_t out;

    snprintf(dir, sizeof dir, "/tmp/piki_test_tools_%ld", (long)getpid());
    if (mkdir(dir, 0700) != 0) {
        fprintf(stderr, "test_tools: cannot create %s\n", dir);
        return 1;
    }

    /* --- classification: only these three may touch the system --- */
    CHECK(tool_is_dangerous("write_file") == 1);
    CHECK(tool_is_dangerous("edit_file") == 1);
    CHECK(tool_is_dangerous("run_command") == 1);
    CHECK(tool_is_dangerous("read_file") == 0);
    CHECK(tool_is_dangerous("list_files") == 0);
    CHECK(tool_is_dangerous("search_files") == 0);

    /* --- read_file --- */
    path_in(p, sizeof p, "a.txt");
    write_file(p, "hello\nworld\n");
    snprintf(args, sizeof args, "{\"path\":\"%s\"}", p);
    CHECK(run("read_file", args, &out) == 0);
    CHECK(strcmp(out.data, "hello\nworld\n") == 0);
    buf_free(&out);

    CHECK(run("read_file", "{\"path\":\"/nope/nope\"}", &out) == -1);
    buf_free(&out);
    CHECK(run("read_file", "{}", &out) == -1);   /* missing arg */
    buf_free(&out);

    /* --- list_files --- */
    path_in(p, sizeof p, "sub");
    mkdir(p, 0700);
    snprintf(args, sizeof args, "{\"path\":\"%s\"}", dir);
    CHECK(run("list_files", args, &out) == 0);
    CHECK(strstr(out.data, "a.txt") != NULL);
    CHECK(strstr(out.data, "sub/") != NULL);     /* dirs marked */
    buf_free(&out);

    CHECK(run("list_files", "{\"path\":\"/nope/nope\"}", &out) == -1);
    buf_free(&out);

    /* --- search_files --- */
    snprintf(args, sizeof args, "{\"pattern\":\"world\",\"path\":\"%s\"}",
             dir);
    CHECK(run("search_files", args, &out) == 0);
    CHECK(strstr(out.data, "a.txt:2:") != NULL); /* right line number */
    buf_free(&out);

    snprintf(args, sizeof args, "{\"pattern\":\"zzz\",\"path\":\"%s\"}", dir);
    CHECK(run("search_files", args, &out) == 0);
    CHECK(strstr(out.data, "no matches") != NULL);
    buf_free(&out);

    CHECK(run("search_files", "{\"path\":\".\"}", &out) == -1);  /* no pattern */
    buf_free(&out);

    /* binary files are skipped */
    {
        FILE *f;

        path_in(p, sizeof p, "bin.dat");
        f = fopen(p, "wb");
        if (f) {
            fwrite("nee\0dle", 1, 7, f);
            fclose(f);
        }
        snprintf(args, sizeof args,
                 "{\"pattern\":\"nee\",\"path\":\"%s\"}", dir);
        CHECK(run("search_files", args, &out) == 0);
        CHECK(strstr(out.data, "bin.dat") == NULL);
        buf_free(&out);
    }

    /* --- edit_file --- */
    path_in(p, sizeof p, "e.txt");

    /* unique match: applied */
    write_file(p, "one two three\n");
    snprintf(args, sizeof args,
             "{\"path\":\"%s\",\"old_string\":\"two\","
             "\"new_string\":\"TWO\"}", p);
    CHECK(run("edit_file", args, &out) == 0);
    buf_free(&out);
    read_back(p, back, sizeof back);
    CHECK(strcmp(back, "one TWO three\n") == 0);

    /* zero matches: error, file untouched */
    write_file(p, "one two three\n");
    snprintf(args, sizeof args,
             "{\"path\":\"%s\",\"old_string\":\"nope\","
             "\"new_string\":\"X\"}", p);
    CHECK(run("edit_file", args, &out) == -1);
    CHECK(strstr(out.data, "not found") != NULL);
    buf_free(&out);
    read_back(p, back, sizeof back);
    CHECK(strcmp(back, "one two three\n") == 0);

    /* several matches: error, file untouched */
    write_file(p, "dup dup\n");
    snprintf(args, sizeof args,
             "{\"path\":\"%s\",\"old_string\":\"dup\","
             "\"new_string\":\"X\"}", p);
    CHECK(run("edit_file", args, &out) == -1);
    CHECK(strstr(out.data, "appears 2 times") != NULL);
    buf_free(&out);
    read_back(p, back, sizeof back);
    CHECK(strcmp(back, "dup dup\n") == 0);

    /* empty old_string is refused */
    snprintf(args, sizeof args,
             "{\"path\":\"%s\",\"old_string\":\"\",\"new_string\":\"X\"}", p);
    CHECK(run("edit_file", args, &out) == -1);
    buf_free(&out);

    /* missing file */
    CHECK(run("edit_file",
              "{\"path\":\"/nope/nope\",\"old_string\":\"a\","
              "\"new_string\":\"b\"}", &out) == -1);
    buf_free(&out);

    /* a file past the read cap is refused, NOT silently truncated */
    {
        FILE *f;
        size_t i;

        path_in(p, sizeof p, "big.txt");
        f = fopen(p, "wb");
        if (f) {
            fputs("NEEDLE\n", f);
            for (i = 0; i < 300 * 1024 / 16; i++)
                fputs("0123456789abcde\n", f);
            fclose(f);
        }
        snprintf(args, sizeof args,
                 "{\"path\":\"%s\",\"old_string\":\"NEEDLE\","
                 "\"new_string\":\"X\"}", p);
        CHECK(run("edit_file", args, &out) == -1);
        CHECK(strstr(out.data, "too large") != NULL);
        buf_free(&out);
        /* and the file kept its full size */
        {
            struct stat st;

            CHECK(stat(p, &st) == 0 && st.st_size > 300 * 1024);
        }
        remove(p);
    }

    /* multi-line replacement works */
    write_file(p, "a\nb\nc\n");
    snprintf(args, sizeof args,
             "{\"path\":\"%s\",\"old_string\":\"a\\nb\","
             "\"new_string\":\"A\\nB\\nB2\"}", p);
    CHECK(run("edit_file", args, &out) == 0);
    buf_free(&out);
    read_back(p, back, sizeof back);
    CHECK(strcmp(back, "A\nB\nB2\nc\n") == 0);

    /* --- unknown tool / bad JSON --- */
    CHECK(run("nope_tool", "{}", &out) == -2);
    buf_free(&out);
    CHECK(run("read_file", "{not json", &out) == -1);
    buf_free(&out);

    /* --- tool_describe --- */
    snprintf(args, sizeof args,
             "{\"path\":\"f.c\",\"old_string\":\"aaa\","
             "\"new_string\":\"bbb\"}");
    buf_init(&out);
    tool_describe("edit_file", args, &out);
    CHECK(strstr(out.data, "f.c") != NULL);
    CHECK(strstr(out.data, "aaa -> bbb") != NULL);
    buf_free(&out);

    buf_init(&out);
    tool_describe("search_files", "{\"pattern\":\"x\"}", &out);
    CHECK(strstr(out.data, "'x'") != NULL);
    buf_free(&out);

    /* cleanup */
    path_in(p, sizeof p, "a.txt");   remove(p);
    path_in(p, sizeof p, "e.txt");   remove(p);
    path_in(p, sizeof p, "bin.dat"); remove(p);
    path_in(p, sizeof p, "sub");     rmdir(p);
    rmdir(dir);

    if (fails) {
        fprintf(stderr, "test_tools: %d failures\n", fails);
        return 1;
    }
    puts("test_tools: OK");
    return 0;
}

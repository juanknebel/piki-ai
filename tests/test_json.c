#include "json.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fails++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

static json_val *parse_ok(const char *text, json_doc **doc)
{
    char err[128];
    json_val *v = json_parse(text, doc, err, sizeof err);

    if (!v) {
        fails++;
        fprintf(stderr, "FAIL parse of %.40s: %s\n", text, err);
    }
    return v;
}

static void expect_fail(const char *text)
{
    json_doc *doc;
    json_val *v = json_parse(text, &doc, NULL, 0);

    if (v) {
        fails++;
        fprintf(stderr, "FAIL: accepted invalid JSON: %.60s\n", text);
        json_doc_free(doc);
    }
}

int main(void)
{
    json_doc *doc;
    json_val *v;

    /* scalars */
    v = parse_ok("null", &doc);
    CHECK(v && v->type == JSON_NULL);
    json_doc_free(doc);

    v = parse_ok(" true ", &doc);
    CHECK(v && v->type == JSON_BOOL && v->u.b == 1);
    json_doc_free(doc);

    v = parse_ok("false", &doc);
    CHECK(v && v->type == JSON_BOOL && v->u.b == 0);
    json_doc_free(doc);

    v = parse_ok("42", &doc);
    CHECK(v && v->type == JSON_NUM && v->u.num == 42.0);
    json_doc_free(doc);

    v = parse_ok("-3.5e2", &doc);
    CHECK(v && v->type == JSON_NUM && v->u.num == -350.0);
    json_doc_free(doc);

    /* strings and escapes */
    v = parse_ok("\"hola\"", &doc);
    CHECK(json_str(v) && strcmp(json_str(v), "hola") == 0);
    json_doc_free(doc);

    v = parse_ok("\"a\\\"b\\\\c\\/d\\n\\te\"", &doc);
    CHECK(json_str(v) && strcmp(json_str(v), "a\"b\\c/d\n\te") == 0);
    json_doc_free(doc);

    v = parse_ok("\"caf\\u00e9\"", &doc);
    CHECK(json_str(v) && strcmp(json_str(v), "caf\xc3\xa9") == 0);
    json_doc_free(doc);

    /* surrogate pair: U+1F600 */
    v = parse_ok("\"\\ud83d\\ude00\"", &doc);
    CHECK(json_str(v) && strcmp(json_str(v), "\xf0\x9f\x98\x80") == 0);
    json_doc_free(doc);

    /* lone surrogate -> U+FFFD */
    v = parse_ok("\"x\\ud83dy\"", &doc);
    CHECK(json_str(v) && strcmp(json_str(v), "x\xef\xbf\xbdy") == 0);
    json_doc_free(doc);

    /* embedded NUL via escape */
    v = parse_ok("\"a\\u0000b\"", &doc);
    CHECK(v && v->type == JSON_STR && v->u.str.len == 3 &&
          memcmp(v->u.str.ptr, "a\0b\0", 4) == 0);
    json_doc_free(doc);

    /* empty containers */
    v = parse_ok("{}", &doc);
    CHECK(v && v->type == JSON_OBJ && v->u.obj.n == 0);
    json_doc_free(doc);

    v = parse_ok("[]", &doc);
    CHECK(v && v->type == JSON_ARR && v->u.arr.n == 0);
    json_doc_free(doc);

    /* OpenRouter-style response + json_get */
    v = parse_ok("{\"id\":\"gen-1\",\"choices\":[{\"index\":0,"
                 "\"message\":{\"role\":\"assistant\","
                 "\"content\":\"hola!\"}}],"
                 "\"usage\":{\"total_tokens\":10}}", &doc);
    CHECK(v != NULL);
    if (v) {
        const char *s = json_str(json_get(v, "choices.0.message.content"));

        CHECK(s && strcmp(s, "hola!") == 0);
        CHECK(json_num(json_get(v, "usage.total_tokens"), -1) == 10.0);
        CHECK(json_get(v, "choices.1") == NULL);
        CHECK(json_get(v, "choices.0.inexistente") == NULL);
        CHECK(json_get(v, "id.0") == NULL);
    }
    json_doc_free(doc);

    /* malformed: must not crash or be accepted */
    expect_fail("");
    expect_fail("   ");
    expect_fail("{");
    expect_fail("[1,");
    expect_fail("[1,]");
    expect_fail("\"abc");
    expect_fail("{\"a\":}");
    expect_fail("{\"a\" 1}");
    expect_fail("{1:2}");
    expect_fail("tru");
    expect_fail("nulll");
    expect_fail("1 2");
    expect_fail("\"\\u12\"");
    expect_fail("\"\\q\"");

    /* hostile nesting: bounded, no stack overflow */
    {
        char deep[512];
        int i;

        for (i = 0; i < 256; i++)
            deep[i] = '[';
        deep[256] = '\0';
        expect_fail(deep);
    }

    /* writer: escaping */
    {
        buf_t b;

        buf_init(&b);
        json_escape(&b, "a\"b\\c\nd\x01" "e");
        CHECK(strcmp(b.data, "\"a\\\"b\\\\c\\nd\\u0001e\"") == 0);
        buf_reset(&b);
        json_escape(&b, "ñandú");
        CHECK(strcmp(b.data, "\"ñandú\"") == 0);
        buf_free(&b);
    }

    if (fails) {
        fprintf(stderr, "test_json: %d failures\n", fails);
        return 1;
    }
    puts("test_json: OK");
    return 0;
}

#ifndef PIKI_JSON_H
#define PIKI_JSON_H

#include <stddef.h>

#include "buf.h"

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUM,
    JSON_STR,
    JSON_ARR,
    JSON_OBJ
} json_type;

typedef struct json_val json_val;

struct json_val {
    json_type type;
    union {
        int b;
        double num;
        struct { const char *ptr; size_t len; } str; /* NUL-terminated; may
                                                        have embedded NULs */
        struct { json_val **items; size_t n; } arr;
        struct { const char **keys; json_val **vals; size_t n; } obj;
    } u;
};

/* Arena: all the document's memory is freed with a single
 * json_doc_free. The json_val and strings point inside it. */
typedef struct json_doc json_doc;

/* Parses text (NUL-terminated). Returns the root and leaves the arena in *docp,
 * or NULL with the description in err (errlen 0 / err NULL to omit it).
 * Never crashes on malformed input; bounded depth. */
json_val *json_parse(const char *text, json_doc **docp,
                     char *err, size_t errlen);
void json_doc_free(json_doc *doc);

/* Path navigation: "choices.0.message.content" (object keys and
 * array indices separated by dots). NULL if it does not exist. */
json_val *json_get(const json_val *v, const char *path);

const char *json_str(const json_val *v);          /* NULL if not a string */
double json_num(const json_val *v, double dflt);

/* Appends to out the string s as a JSON literal with quotes and escapes. */
void json_escape(buf_t *out, const char *s);

#endif /* PIKI_JSON_H */

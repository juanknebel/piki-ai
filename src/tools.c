#include "tools.h"

#include <stdio.h>
#include <string.h>

#include "json.h"

#define MAX_READ (256 * 1024)
#define MAX_OUT  (64 * 1024)

const char *const TOOLS_SCHEMA =
"[{\"type\":\"function\",\"function\":{"
"\"name\":\"read_file\",\"description\":\"Reads a text file and "
"returns its content.\",\"parameters\":{\"type\":\"object\","
"\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File "
"path\"}},\"required\":[\"path\"]}}},"
"{\"type\":\"function\",\"function\":{"
"\"name\":\"write_file\",\"description\":\"Writes (or overwrites) a "
"text file.\",\"parameters\":{\"type\":\"object\","
"\"properties\":{\"path\":{\"type\":\"string\"},"
"\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}}},"
"{\"type\":\"function\",\"function\":{"
"\"name\":\"run_command\",\"description\":\"Runs a shell command "
"and returns its combined output (stdout+stderr).\",\"parameters\":"
"{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},"
"\"required\":[\"command\"]}}}]";

int tool_is_dangerous(const char *name)
{
    return strcmp(name, "write_file") == 0 ||
           strcmp(name, "run_command") == 0;
}

/* Extracts a string from args_json into a temporary doc. */
static const char *arg_str(json_doc *doc, json_val *root, const char *key)
{
    (void)doc;
    return json_str(json_get(root, key));
}

static int do_read_file(json_val *root, buf_t *out)
{
    const char *path = json_str(json_get(root, "path"));
    FILE *f;
    char tmp[8192];
    size_t n, total = 0;

    if (!path) {
        buf_puts(out, "error: missing 'path' argument");
        return -1;
    }
    f = fopen(path, "r");
    if (!f) {
        buf_printf(out, "error: could not open %s", path);
        return -1;
    }
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) {
        if (total + n > MAX_READ) {
            buf_append(out, tmp, MAX_READ - total);
            buf_puts(out, "\n[...truncated...]");
            total = MAX_READ;
            break;
        }
        buf_append(out, tmp, n);
        total += n;
    }
    fclose(f);
    if (total == 0)
        buf_puts(out, "(empty file)");
    return 0;
}

static int do_write_file(json_val *root, buf_t *out)
{
    const char *path = json_str(json_get(root, "path"));
    json_val *cv = json_get(root, "content");
    const char *content = json_str(cv);
    size_t clen = (cv && cv->type == JSON_STR) ? cv->u.str.len : 0;
    FILE *f;

    if (!path || !content) {
        buf_puts(out, "error: missing 'path'/'content' arguments");
        return -1;
    }
    f = fopen(path, "w");
    if (!f) {
        buf_printf(out, "error: could not write %s", path);
        return -1;
    }
    if (fwrite(content, 1, clen, f) != clen || fclose(f) != 0) {
        buf_printf(out, "error: failed to write %s", path);
        return -1;
    }
    buf_printf(out, "wrote %s (%lu bytes)", path, (unsigned long)clen);
    return 0;
}

static int do_run_command(json_val *root, buf_t *out)
{
    const char *cmd = json_str(json_get(root, "command"));
    FILE *p;
    char tmp[4096];
    size_t n, total = 0;
    int rc;

    if (!cmd) {
        buf_puts(out, "error: missing 'command' argument");
        return -1;
    }
    p = popen(cmd, "r");
    if (!p) {
        buf_puts(out, "error: could not run the command");
        return -1;
    }
    while ((n = fread(tmp, 1, sizeof tmp, p)) > 0) {
        if (total + n > MAX_OUT) {
            buf_append(out, tmp, MAX_OUT - total);
            buf_puts(out, "\n[...output truncated...]");
            total = MAX_OUT;
            /* keep reading so as not to break the pipe */
            while (fread(tmp, 1, sizeof tmp, p) > 0)
                ;
            break;
        }
        buf_append(out, tmp, n);
        total += n;
    }
    rc = pclose(p);
    if (total == 0)
        buf_puts(out, "(no output)");
    buf_printf(out, "\n[exit %d]", rc);
    return 0;
}

int tool_run(const char *name, const char *args_json, buf_t *out)
{
    json_doc *doc = NULL;
    json_val *root;
    int rc;

    root = json_parse(args_json && *args_json ? args_json : "{}",
                      &doc, NULL, 0);
    if (!root) {
        buf_puts(out, "error: invalid JSON arguments");
        return -1;
    }
    (void)arg_str;
    if (strcmp(name, "read_file") == 0)
        rc = do_read_file(root, out);
    else if (strcmp(name, "write_file") == 0)
        rc = do_write_file(root, out);
    else if (strcmp(name, "run_command") == 0)
        rc = do_run_command(root, out);
    else {
        buf_printf(out, "error: unknown tool '%s'", name);
        rc = -2;
    }
    json_doc_free(doc);
    return rc;
}

void tool_describe(const char *name, const char *args_json, buf_t *out)
{
    json_doc *doc = NULL;
    json_val *root = json_parse(args_json && *args_json ? args_json : "{}",
                                &doc, NULL, 0);

    if (strcmp(name, "read_file") == 0)
        buf_printf(out, "read_file: %s",
                   root ? (json_str(json_get(root, "path")) ?
                           json_str(json_get(root, "path")) : "?") : "?");
    else if (strcmp(name, "write_file") == 0) {
        const char *path = root ? json_str(json_get(root, "path")) : NULL;
        json_val *cv = root ? json_get(root, "content") : NULL;
        size_t clen = (cv && cv->type == JSON_STR) ? cv->u.str.len : 0;

        buf_printf(out, "write_file: %s (%lu bytes)",
                   path ? path : "?", (unsigned long)clen);
    } else if (strcmp(name, "run_command") == 0)
        buf_printf(out, "run_command: %s",
                   root ? (json_str(json_get(root, "command")) ?
                           json_str(json_get(root, "command")) : "?") : "?");
    else
        buf_printf(out, "%s: %s", name, args_json ? args_json : "");
    json_doc_free(doc);
}

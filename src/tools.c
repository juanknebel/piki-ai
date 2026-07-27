#include "tools.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "json.h"

#define MAX_READ    (256 * 1024)
#define MAX_OUT     (64 * 1024)
#define MAX_ENTRIES 500      /* list_files */
#define MAX_MATCHES 100      /* search_files */
#define MAX_DEPTH   8        /* search_files recursion */

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
"\"name\":\"list_files\",\"description\":\"Lists the entries of a "
"directory; directories are marked with a trailing slash.\","
"\"parameters\":{\"type\":\"object\",\"properties\":"
"{\"path\":{\"type\":\"string\",\"description\":\"Directory, defaults "
"to the current one\"}},\"required\":[]}}},"
"{\"type\":\"function\",\"function\":{"
"\"name\":\"search_files\",\"description\":\"Searches for a literal "
"substring (not a regex) in a file or recursively in a directory, "
"skipping hidden and binary files. Returns path:line: text.\","
"\"parameters\":{\"type\":\"object\",\"properties\":"
"{\"pattern\":{\"type\":\"string\"},"
"\"path\":{\"type\":\"string\",\"description\":\"File or directory, "
"defaults to the current one\"}},\"required\":[\"pattern\"]}}},"
"{\"type\":\"function\",\"function\":{"
"\"name\":\"edit_file\",\"description\":\"Replaces old_string with "
"new_string in a file. old_string must appear exactly once; if it does "
"not, quote more surrounding text. Prefer this over write_file for "
"changes to an existing file.\",\"parameters\":{\"type\":\"object\","
"\"properties\":{\"path\":{\"type\":\"string\"},"
"\"old_string\":{\"type\":\"string\"},"
"\"new_string\":{\"type\":\"string\"}},"
"\"required\":[\"path\",\"old_string\",\"new_string\"]}}},"
"{\"type\":\"function\",\"function\":{"
"\"name\":\"run_command\",\"description\":\"Runs a shell command "
"and returns its combined output (stdout+stderr).\",\"parameters\":"
"{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},"
"\"required\":[\"command\"]}}}]";

int tool_is_dangerous(const char *name)
{
    return strcmp(name, "write_file") == 0 ||
           strcmp(name, "edit_file") == 0 ||
           strcmp(name, "run_command") == 0;
}

/* Reads a file into out, capped at MAX_READ. 0 ok, -1 could not open. */
static int slurp(const char *path, buf_t *out)
{
    FILE *f = fopen(path, "rb");
    char tmp[8192];
    size_t n;

    if (!f)
        return -1;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) {
        buf_append(out, tmp, n);
        if (out->len >= MAX_READ)
            break;
    }
    fclose(f);
    return 0;
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

static int do_list_files(json_val *root, buf_t *out)
{
    const char *path = json_str(json_get(root, "path"));
    DIR *d;
    struct dirent *e;
    size_t n = 0;

    if (!path || !*path)
        path = ".";
    d = opendir(path);
    if (!d) {
        buf_printf(out, "error: could not open directory %s", path);
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        buf_t full;
        struct stat st;

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (n >= MAX_ENTRIES) {
            buf_puts(out, "[...truncated...]\n");
            break;
        }
        /* stat rather than d_type: d_type is not portable (Haiku) */
        buf_init(&full);
        buf_printf(&full, "%s/%s", path, e->d_name);
        buf_puts(out, e->d_name);
        if (stat(full.data, &st) == 0 && S_ISDIR(st.st_mode))
            buf_putc(out, '/');
        buf_putc(out, '\n');
        buf_free(&full);
        n++;
    }
    closedir(d);
    if (n == 0)
        buf_puts(out, "(empty directory)");
    return 0;
}

/* Literal substring search over one text file. */
static void search_one(const char *path, const char *pattern,
                       buf_t *out, size_t *matches)
{
    buf_t data;
    char *p;
    long lineno = 1;

    buf_init(&data);
    if (slurp(path, &data) < 0) {
        buf_free(&data);
        return;
    }
    if (memchr(data.data, '\0', data.len)) {   /* binary: skip */
        buf_free(&data);
        return;
    }
    p = data.data;
    while (p && *p && *matches < MAX_MATCHES) {
        char *nl = strchr(p, '\n');

        if (nl)
            *nl = '\0';
        if (strstr(p, pattern)) {
            buf_printf(out, "%s:%ld: %.200s\n", path, lineno, p);
            (*matches)++;
        }
        lineno++;
        if (!nl)
            break;
        p = nl + 1;
    }
    buf_free(&data);
}

static void search_dir(const char *path, const char *pattern, int depth,
                       buf_t *out, size_t *matches)
{
    DIR *d;
    struct dirent *e;

    if (depth > MAX_DEPTH || *matches >= MAX_MATCHES)
        return;
    d = opendir(path);
    if (!d)
        return;
    while ((e = readdir(d)) != NULL && *matches < MAX_MATCHES) {
        buf_t full;
        struct stat st;

        if (e->d_name[0] == '.')      /* skips . .. and .git */
            continue;
        buf_init(&full);
        buf_printf(&full, "%s/%s", path, e->d_name);
        if (stat(full.data, &st) == 0) {
            if (S_ISDIR(st.st_mode))
                search_dir(full.data, pattern, depth + 1, out, matches);
            else if (S_ISREG(st.st_mode))
                search_one(full.data, pattern, out, matches);
        }
        buf_free(&full);
    }
    closedir(d);
}

static int do_search_files(json_val *root, buf_t *out)
{
    const char *pattern = json_str(json_get(root, "pattern"));
    const char *path = json_str(json_get(root, "path"));
    struct stat st;
    size_t matches = 0;

    if (!pattern || !*pattern) {
        buf_puts(out, "error: missing 'pattern' argument");
        return -1;
    }
    if (!path || !*path)
        path = ".";
    if (stat(path, &st) != 0) {
        buf_printf(out, "error: %s does not exist", path);
        return -1;
    }
    if (S_ISDIR(st.st_mode))
        search_dir(path, pattern, 0, out, &matches);
    else
        search_one(path, pattern, out, &matches);

    if (matches == 0)
        buf_printf(out, "no matches for '%s' in %s", pattern, path);
    else if (matches >= MAX_MATCHES)
        buf_puts(out, "[...more matches truncated...]");
    return 0;
}

/* Replaces old_string with new_string; old_string must occur exactly once.
 * Refusing ambiguous edits pushes the model to quote more context instead
 * of guessing which occurrence it meant. */
static int do_edit_file(json_val *root, buf_t *out)
{
    const char *path = json_str(json_get(root, "path"));
    json_val *ov = json_get(root, "old_string");
    json_val *nv = json_get(root, "new_string");
    const char *olds = json_str(ov), *news = json_str(nv);
    buf_t data, result;
    const char *p, *hit = NULL;
    size_t count = 0, oldlen;
    FILE *f;
    int ret = -1;

    if (!path || !olds || !news) {
        buf_puts(out, "error: missing 'path'/'old_string'/'new_string'");
        return -1;
    }
    oldlen = ov->u.str.len;
    if (oldlen == 0) {
        buf_puts(out, "error: 'old_string' must not be empty");
        return -1;
    }

    buf_init(&data);
    buf_init(&result);
    {
        struct stat st;

        /* slurp() caps at MAX_READ; writing back a truncated read would
         * destroy the tail of the file, so refuse instead. */
        if (stat(path, &st) == 0 && st.st_size > (off_t)MAX_READ) {
            buf_printf(out, "error: %s is too large to edit (%ld bytes, "
                       "limit %d)", path, (long)st.st_size, MAX_READ);
            goto done;
        }
    }
    if (slurp(path, &data) < 0) {
        buf_printf(out, "error: could not open %s", path);
        goto done;
    }
    for (p = data.data; (p = strstr(p, olds)) != NULL; p += oldlen) {
        if (!hit)
            hit = p;
        count++;
    }
    if (count == 0) {
        buf_printf(out, "error: 'old_string' not found in %s", path);
        goto done;
    }
    if (count > 1) {
        buf_printf(out, "error: 'old_string' appears %lu times in %s; "
                   "quote more surrounding text to make it unique",
                   (unsigned long)count, path);
        goto done;
    }

    buf_append(&result, data.data, (size_t)(hit - data.data));
    buf_append(&result, news, nv->u.str.len);
    buf_append(&result, hit + oldlen,
               data.len - (size_t)(hit - data.data) - oldlen);

    f = fopen(path, "wb");
    if (!f) {
        buf_printf(out, "error: could not write %s", path);
        goto done;
    }
    if (fwrite(result.data, 1, result.len, f) != result.len ||
        fclose(f) != 0) {
        buf_printf(out, "error: failed while writing %s", path);
        goto done;
    }
    buf_printf(out, "edited %s (%lu bytes -> %lu bytes)", path,
               (unsigned long)data.len, (unsigned long)result.len);
    ret = 0;

done:
    buf_free(&data);
    buf_free(&result);
    return ret;
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
    if (strcmp(name, "read_file") == 0)
        rc = do_read_file(root, out);
    else if (strcmp(name, "list_files") == 0)
        rc = do_list_files(root, out);
    else if (strcmp(name, "search_files") == 0)
        rc = do_search_files(root, out);
    else if (strcmp(name, "write_file") == 0)
        rc = do_write_file(root, out);
    else if (strcmp(name, "edit_file") == 0)
        rc = do_edit_file(root, out);
    else if (strcmp(name, "run_command") == 0)
        rc = do_run_command(root, out);
    else {
        buf_printf(out, "error: unknown tool '%s'", name);
        rc = -2;
    }
    json_doc_free(doc);
    return rc;
}

static const char *str_or(json_val *root, const char *key, const char *dflt)
{
    const char *s = root ? json_str(json_get(root, key)) : NULL;

    return s ? s : dflt;
}

/* Appends s up to the first newline or max chars, marking a cut with "..." */
static void put_snippet(buf_t *out, const char *s, size_t max)
{
    size_t i;

    for (i = 0; i < max && s[i] && s[i] != '\n'; i++)
        buf_putc(out, s[i]);
    if (s[i])
        buf_puts(out, "...");
}

void tool_describe(const char *name, const char *args_json, buf_t *out)
{
    json_doc *doc = NULL;
    json_val *root = json_parse(args_json && *args_json ? args_json : "{}",
                                &doc, NULL, 0);

    if (strcmp(name, "read_file") == 0) {
        buf_printf(out, "read_file: %s", str_or(root, "path", "?"));
    } else if (strcmp(name, "list_files") == 0) {
        buf_printf(out, "list_files: %s", str_or(root, "path", "."));
    } else if (strcmp(name, "search_files") == 0) {
        buf_printf(out, "search_files: '%s' in %s",
                   str_or(root, "pattern", "?"), str_or(root, "path", "."));
    } else if (strcmp(name, "write_file") == 0) {
        json_val *cv = root ? json_get(root, "content") : NULL;
        size_t clen = (cv && cv->type == JSON_STR) ? cv->u.str.len : 0;

        buf_printf(out, "write_file: %s (%lu bytes)",
                   str_or(root, "path", "?"), (unsigned long)clen);
    } else if (strcmp(name, "edit_file") == 0) {
        buf_printf(out, "edit_file: %s | ", str_or(root, "path", "?"));
        put_snippet(out, str_or(root, "old_string", "?"), 40);
        buf_puts(out, " -> ");
        put_snippet(out, str_or(root, "new_string", "?"), 40);
    } else if (strcmp(name, "run_command") == 0) {
        buf_printf(out, "run_command: %s", str_or(root, "command", "?"));
    } else {
        buf_printf(out, "%s: %s", name, args_json ? args_json : "");
    }
    json_doc_free(doc);
}

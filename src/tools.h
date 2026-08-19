#ifndef PIKI_TOOLS_H
#define PIKI_TOOLS_H

#include "buf.h"

/* Tools the model can invoke (function calling). Each one receives its
 * already-parsed arguments and writes the result (text that goes back to
 * the model) into out. They return 0 ok, -1 error (the error text is sent
 * to the model anyway). */

/* The tool definitions in OpenAI format, as array items WITHOUT the
 * surrounding brackets: the caller assembles the "tools" array per turn
 * (optionally appending the web tools from web.h). */
extern const char *const TOOLS_ITEMS;

/* Runs the tool `name` with `args_json` (the argument string the model
 * sends). Writes the result into out. requires_confirm is set to 1 if the
 * action is dangerous (write/command) and the caller must have confirmed.
 * Returns 0 ok, -1 execution error, -2 unknown tool. */
int tool_run(const char *name, const char *args_json, buf_t *out);

/* 1 if the tool modifies the system and confirmation should be requested. */
int tool_is_dangerous(const char *name);

/* Readable summary of the invocation, to show the user before
 * confirming (e.g. `run_command: ls -la`). */
void tool_describe(const char *name, const char *args_json, buf_t *out);

#endif /* PIKI_TOOLS_H */

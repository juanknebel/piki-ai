#ifndef PIKI_CHAT_H
#define PIKI_CHAT_H

#include <stddef.h>

#include "api.h"

/* Conversation history. Contents are copied (owned); roles are
 * static literals.
 *
 * Two independent limits, do not confuse them:
 *   max_bytes  caps what is KEPT in RAM here (oldest messages are evicted
 *              and freed) -- this is what keeps a long session from
 *              swapping on a 1 GB machine.
 *   chat_window's budget caps what is SENT on a given turn. */
typedef struct {
    chat_msg *msgs;
    size_t n, cap;
    size_t bytes;      /* sum of strlen(content) over stored messages */
    size_t max_bytes;  /* 0 = unlimited */
    char *system;      /* system prompt, optional; never evicted */
} chat_t;

void chat_init(chat_t *c);
void chat_free(chat_t *c);

/* Sets the RAM cap and evicts right away if already over it. The newest
 * message is never evicted, even if it alone exceeds the cap. */
void chat_set_max_bytes(chat_t *c, size_t max_bytes);

void chat_set_system(chat_t *c, const char *s);   /* NULL removes it */
void chat_add(chat_t *c, const char *role, const char *content);
void chat_pop(chat_t *c);      /* discards the last message */
void chat_clear(chat_t *c);    /* clears the history, keeps the system */
void chat_trim(chat_t *c, size_t keep);  /* keeps only the newest n messages */

/* Builds the window to send: system (if any) + the newest messages that
 * fit within BOTH max_msgs and max_bytes (0 = no byte limit), and within
 * outcap. The newest message is always included. Returns the number
 * written. */
size_t chat_window(const chat_t *c, size_t max_msgs, size_t max_bytes,
                   chat_msg *out, size_t outcap);

/* Persistence as JSON {"system":..,"messages":[{role,content}..]}.
 * chat_load replaces the current content. 0 ok, -1 error (err). */
int chat_save(const chat_t *c, const char *path, char *err, size_t errlen);
int chat_load(chat_t *c, const char *path, char *err, size_t errlen);

/* Export as Markdown: system as header + role sections. 0 ok, -1 error. */
int chat_export_md(const chat_t *c, const char *path, char *err, size_t errlen);

#endif /* PIKI_CHAT_H */

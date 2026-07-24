#ifndef PIKI_CHAT_H
#define PIKI_CHAT_H

#include <stddef.h>

#include "api.h"

/* Conversation history. Contents are copied (owned); roles are
 * static literals. */
typedef struct {
    chat_msg *msgs;
    size_t n, cap;
    char *system;      /* system prompt, optional */
} chat_t;

void chat_init(chat_t *c);
void chat_free(chat_t *c);

void chat_set_system(chat_t *c, const char *s);   /* NULL removes it */
void chat_add(chat_t *c, const char *role, const char *content);
void chat_pop(chat_t *c);      /* discards the last message */
void chat_clear(chat_t *c);    /* clears the history, keeps the system */

/* Builds the window to send: system (if any) + the last max_msgs
 * messages that fit in out. Returns the number written. */
size_t chat_window(const chat_t *c, size_t max_msgs,
                   chat_msg *out, size_t outcap);

/* Persistence as JSON {"system":..,"messages":[{role,content}..]}.
 * chat_load replaces the current content. 0 ok, -1 error (err). */
int chat_save(const chat_t *c, const char *path, char *err, size_t errlen);
int chat_load(chat_t *c, const char *path, char *err, size_t errlen);

#endif /* PIKI_CHAT_H */

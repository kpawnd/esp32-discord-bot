#pragma once
#include "jsmn.h"
#include <stdbool.h>
#include <stddef.h>

#define JP_MAX_TOKENS 512

/* Returns true if token t's string content equals s */
bool jp_tok_eq(const char *js, const jsmntok_t *t, const char *s);

/* Copy a token's string into buf (NUL-terminated). Returns false on truncation. */
bool jp_tok_str(const char *js, const jsmntok_t *t, char *buf, size_t bufsz);

/*
 * Find the VALUE token index for key in the OBJECT at obj_idx.
 * Returns -1 if not found. Only searches direct children (not nested).
 */
int jp_find(const char *js, const jsmntok_t *toks, int ntoks,
            int obj_idx, const char *key);

/* Convenience: copy string value for key into buf. Returns false if not found. */
bool jp_str(const char *js, const jsmntok_t *toks, int ntoks,
            int obj_idx, const char *key, char *buf, size_t bufsz);

/* Convenience: get integer value for key. Returns false if not found. */
bool jp_int(const char *js, const jsmntok_t *toks, int ntoks,
            int obj_idx, const char *key, int *out);

/* Convenience: get boolean value for key. Returns false if not found. */
bool jp_bool(const char *js, const jsmntok_t *toks, int ntoks,
             int obj_idx, const char *key, bool *out);

/*
 * Tokenize js into a static token buffer.
 * Returns number of tokens on success, ≤0 on error.
 * toks_out must point to an array of JP_MAX_TOKENS jsmntok_t.
 */
int jp_tokenize(const char *js, int len, jsmntok_t *toks_out);

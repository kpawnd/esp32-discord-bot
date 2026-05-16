#include "json_parse.h"
#include <string.h>
#include <stdlib.h>

/* Returns index just past the subtree rooted at toks[i] */
static int jp_skip(const jsmntok_t *toks, int ntoks, int i) {
    if (i >= ntoks) return ntoks;
    int end = toks[i].end;
    i++;
    while (i < ntoks && toks[i].start < end) i++;
    return i;
}

bool jp_tok_eq(const char *js, const jsmntok_t *t, const char *s) {
    int len = t->end - t->start;
    return t->type == JSMN_STRING &&
           (int)strlen(s) == len &&
           strncmp(js + t->start, s, (size_t)len) == 0;
}

bool jp_tok_str(const char *js, const jsmntok_t *t, char *buf, size_t bufsz) {
    int len = t->end - t->start;
    if (len <= 0 || bufsz == 0) { if (bufsz) buf[0] = '\0'; return false; }
    size_t copy = (size_t)len < bufsz - 1 ? (size_t)len : bufsz - 1;
    memcpy(buf, js + t->start, copy);
    buf[copy] = '\0';
    return (size_t)len < bufsz;
}

int jp_find(const char *js, const jsmntok_t *toks, int ntoks,
            int obj_idx, const char *key) {
    if (obj_idx < 0 || obj_idx >= ntoks) return -1;
    if (toks[obj_idx].type != JSMN_OBJECT) return -1;
    int n = toks[obj_idx].size; /* number of keys */
    int i = obj_idx + 1;
    for (int k = 0; k < n && i + 1 < ntoks; k++) {
        int val_idx = i + 1;
        if (jp_tok_eq(js, &toks[i], key)) return val_idx;
        i = jp_skip(toks, ntoks, val_idx);
    }
    return -1;
}

bool jp_str(const char *js, const jsmntok_t *toks, int ntoks,
            int obj_idx, const char *key, char *buf, size_t bufsz) {
    int vi = jp_find(js, toks, ntoks, obj_idx, key);
    if (vi < 0) { if (bufsz) buf[0] = '\0'; return false; }
    return jp_tok_str(js, &toks[vi], buf, bufsz);
}

bool jp_int(const char *js, const jsmntok_t *toks, int ntoks,
            int obj_idx, const char *key, int *out) {
    int vi = jp_find(js, toks, ntoks, obj_idx, key);
    if (vi < 0) return false;
    if (toks[vi].type != JSMN_PRIMITIVE) return false;
    *out = atoi(js + toks[vi].start);
    return true;
}

bool jp_bool(const char *js, const jsmntok_t *toks, int ntoks,
             int obj_idx, const char *key, bool *out) {
    int vi = jp_find(js, toks, ntoks, obj_idx, key);
    if (vi < 0) return false;
    if (toks[vi].type != JSMN_PRIMITIVE) return false;
    char c = js[toks[vi].start];
    *out = (c == 't'); /* "true" vs "false" or "null" */
    return true;
}

int jp_tokenize(const char *js, int len, jsmntok_t *toks_out) {
    jsmn_parser p;
    jsmn_init(&p);
    return jsmn_parse(&p, js, (size_t)len, toks_out, JP_MAX_TOKENS);
}

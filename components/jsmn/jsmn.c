#include "jsmn.h"

static jsmntok_t *jsmn_alloc_token(jsmn_parser *p, jsmntok_t *toks, size_t num) {
    if (p->toknext >= (unsigned)num) return NULL;
    jsmntok_t *t = &toks[p->toknext++];
    t->start = t->end = -1;
    t->size  = 0;
    return t;
}

static void jsmn_fill(jsmntok_t *t, jsmntype_t type, int start, int end) {
    t->type  = type;
    t->start = start;
    t->end   = end;
    t->size  = 0;
}

static int jsmn_parse_prim(jsmn_parser *p, const char *js, size_t len,
                            jsmntok_t *toks, size_t num) {
    int start = (int)p->pos;
    for (; p->pos < len && js[p->pos] != '\0'; p->pos++) {
        switch (js[p->pos]) {
        case ':': case ' ': case '\t': case '\r': case '\n':
        case ',': case ']': case '}':
            goto found;
        default:
            if ((unsigned char)js[p->pos] < 0x20) { p->pos = (unsigned)start; return JSMN_ERROR_INVAL; }
        }
    }
    p->pos = (unsigned)start;
    return JSMN_ERROR_PART;
found:
    if (!toks) { p->pos--; return 0; }
    jsmntok_t *t = jsmn_alloc_token(p, toks, num);
    if (!t) { p->pos = (unsigned)start; return JSMN_ERROR_NOMEM; }
    jsmn_fill(t, JSMN_PRIMITIVE, start, (int)p->pos);
    p->pos--;
    return 0;
}

static int jsmn_parse_str(jsmn_parser *p, const char *js, size_t len,
                           jsmntok_t *toks, size_t num) {
    int start = (int)p->pos;
    p->pos++;
    for (; p->pos < len && js[p->pos] != '\0'; p->pos++) {
        char c = js[p->pos];
        if (c == '"') {
            if (!toks) return 0;
            jsmntok_t *t = jsmn_alloc_token(p, toks, num);
            if (!t) { p->pos = (unsigned)start; return JSMN_ERROR_NOMEM; }
            jsmn_fill(t, JSMN_STRING, start + 1, (int)p->pos);
            return 0;
        }
        if (c == '\\' && p->pos + 1 < len) {
            p->pos++;
            switch (js[p->pos]) {
            case '"': case '/': case '\\': case 'b':
            case 'f': case 'r': case 'n':  case 't': break;
            case 'u':
                for (int i = 0; i < 4 && p->pos < len; i++, p->pos++) {
                    char h = js[p->pos + 1];
                    if (!((h >= '0' && h <= '9') || (h >= 'A' && h <= 'F') || (h >= 'a' && h <= 'f')))
                        { p->pos = (unsigned)start; return JSMN_ERROR_INVAL; }
                }
                break;
            default: p->pos = (unsigned)start; return JSMN_ERROR_INVAL;
            }
        }
    }
    p->pos = (unsigned)start;
    return JSMN_ERROR_PART;
}

void jsmn_init(jsmn_parser *p) {
    p->pos      = 0;
    p->toknext  = 0;
    p->toksuper = -1;
}

int jsmn_parse(jsmn_parser *p, const char *js, size_t len,
               jsmntok_t *toks, unsigned int num_tokens) {
    int count = (int)p->toknext;
    for (; p->pos < len && js[p->pos] != '\0'; p->pos++) {
        char c = js[p->pos];
        jsmntok_t *t;
        int r;
        switch (c) {
        case '{': case '[':
            count++;
            if (toks) {
                t = jsmn_alloc_token(p, toks, num_tokens);
                if (!t) return JSMN_ERROR_NOMEM;
                if (p->toksuper != -1) toks[p->toksuper].size++;
                t->type  = (c == '{') ? JSMN_OBJECT : JSMN_ARRAY;
                t->start = (int)p->pos;
                p->toksuper = (int)(p->toknext - 1);
            }
            break;
        case '}': case ']': {
            jsmntype_t want = (c == '}') ? JSMN_OBJECT : JSMN_ARRAY;
            if (!toks) break;
            for (int i = (int)p->toknext - 1; i >= 0; i--) {
                t = &toks[i];
                if (t->start != -1 && t->end == -1) {
                    if (t->type != want) return JSMN_ERROR_INVAL;
                    p->toksuper = -1;
                    t->end = (int)p->pos + 1;
                    break;
                }
            }
            for (int i = (int)p->toknext - 1; i >= 0; i--) {
                t = &toks[i];
                if (t->start != -1 && t->end == -1) { p->toksuper = i; break; }
            }
            break;
        }
        case '"':
            r = jsmn_parse_str(p, js, len, toks, num_tokens);
            if (r < 0) return r;
            count++;
            if (p->toksuper != -1 && toks) toks[p->toksuper].size++;
            break;
        case '\t': case '\r': case '\n': case ' ': break;
        case ':':
            p->toksuper = (int)(p->toknext - 1);
            break;
        case ',':
            if (toks && p->toksuper != -1 &&
                toks[p->toksuper].type != JSMN_ARRAY &&
                toks[p->toksuper].type != JSMN_OBJECT) {
                for (int i = (int)p->toknext - 1; i >= 0; i--) {
                    if (toks[i].type == JSMN_ARRAY || toks[i].type == JSMN_OBJECT) {
                        if (toks[i].start != -1 && toks[i].end == -1) { p->toksuper = i; break; }
                    }
                }
            }
            break;
        default:
            r = jsmn_parse_prim(p, js, len, toks, num_tokens);
            if (r < 0) return r;
            count++;
            if (p->toksuper != -1 && toks) toks[p->toksuper].size++;
            break;
        }
    }
    if (toks) {
        for (int i = (int)p->toknext - 1; i >= 0; i--)
            if (toks[i].start != -1 && toks[i].end == -1) return JSMN_ERROR_PART;
    }
    return count;
}

// Minimal JSON tokenizer (jsmn-style): no allocation, tokens point into the input.
#include "paneld.h"

#include <stdlib.h>
#include <string.h>

static int new_tok(jtok *t, int *n, int max, int type, int start, int parent) {
    if (*n >= max) return -1;
    t[*n] = (jtok){.type = type, .start = start, .end = -1, .size = 0, .parent = parent};
    return (*n)++;
}

/* Returns the number of tokens, or <0 on malformed input / too many tokens. */
int json_parse(const char *s, int len, jtok *t, int max) {
    int n = 0, parent = -1;
    for (int i = 0; i < len; i++) {
        char c = s[i];
        switch (c) {
        case '{': case '[': {
            int k = new_tok(t, &n, max, c == '{' ? J_OBJ : J_ARR, i, parent);
            if (k < 0) return -1;
            if (parent >= 0) t[parent].size++;
            parent = k;
            break;
        }
        case '}': case ']': {
            int want = c == '}' ? J_OBJ : J_ARR;
            if (parent < 0 || t[parent].type != want) return -2;
            t[parent].end = i + 1;
            parent = t[parent].parent;
            break;
        }
        case '"': {
            int k = new_tok(t, &n, max, J_STR, i + 1, parent);
            if (k < 0) return -1;
            for (i++; i < len && s[i] != '"'; i++)
                if (s[i] == '\\' && i + 1 < len) i++;
            if (i >= len) return -3;
            t[k].end = i;
            if (parent >= 0) t[parent].size++;
            break;
        }
        case ' ': case '\t': case '\r': case '\n': case ':': case ',': break;
        default: {
            int k = new_tok(t, &n, max, J_PRIM, i, parent);
            if (k < 0) return -1;
            while (i < len && !strchr(" \t\r\n,:]}", s[i])) i++;
            t[k].end = i;
            i--;
            if (parent >= 0) t[parent].size++;
        }
        }
    }
    if (parent >= 0) return -4; /* unterminated */
    return n;
}

/* Index of the token after the subtree rooted at i. */
int json_skip(const jtok *t, int n, int i) {
    int end = t[i].end;
    int j = i + 1;
    while (j < n && t[j].start < end && (t[i].type == J_OBJ || t[i].type == J_ARR)) j++;
    return j;
}

/* Value token for `key` in object `obj`, or -1. */
int json_get(const char *s, const jtok *t, int n, int obj, const char *key) {
    if (obj < 0 || obj >= n || t[obj].type != J_OBJ) return -1;
    size_t kl = strlen(key);
    int j = obj + 1;
    for (int k = 0; k < t[obj].size / 2 && j < n; k++) {
        int v = j + 1;
        if (t[j].type == J_STR && (size_t)(t[j].end - t[j].start) == kl && !memcmp(s + t[j].start, key, kl))
            return v < n ? v : -1;
        j = v < n ? json_skip(t, n, v) : n;
    }
    return -1;
}

/* Copies a string/primitive value with basic unescaping. Returns length, or -1. */
int json_str(const char *s, const jtok *t, int i, char *out, size_t len) {
    if (i < 0 || !len) return -1;
    size_t o = 0;
    for (int p = t[i].start; p < t[i].end && o + 1 < len; p++) {
        char c = s[p];
        if (c == '\\' && p + 1 < t[i].end) {
            c = s[++p];
            if (c == 'n') c = ' ';
            else if (c == 't') c = ' ';
            else if (c == 'u') { p += 4; c = '?'; }
        }
        out[o++] = c;
    }
    out[o] = 0;
    return (int)o;
}

double json_num(const char *s, const jtok *t, int i, double dflt) {
    if (i < 0 || t[i].type != J_PRIM) return dflt;
    char b[32];
    if (json_str(s, t, i, b, sizeof b) <= 0) return dflt;
    if (!strcmp(b, "true")) return 1;
    if (!strcmp(b, "false") || !strcmp(b, "null")) return 0;
    char *e;
    double v = strtod(b, &e);
    return e == b ? dflt : v;
}

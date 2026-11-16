// Host-side fuzz loop for paneld's JSON tokenizer (the first thing every PPJ/1 / phonebox byte goes through).
// Build with sanitizers and run: random bytes, mutated valid JSON, deep nesting, huge numbers.
//   cc -fsanitize=address,undefined -g -Ipaneld tests/fuzz_json.c paneld/json.c -o fuzz && ./fuzz 200000
#include "paneld.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static jtok t[1024];

static void exercise(const char *s, int len) {
    int n = json_parse(s, len, t, 1024);
    if (n <= 0) return;
    char buf[64];
    for (int i = 0; i < n; i++) {
        json_str(s, t, i, buf, sizeof buf);
        json_num(s, t, i, 0);
        if (t[i].type == J_OBJ) { json_get(s, t, n, i, "op"); json_get(s, t, n, i, "widgets"); }
        json_skip(t, n, i);
    }
}

int main(int argc, char **argv) {
    long iters = argc > 1 ? atol(argv[1]) : 100000;
    const char *seeds[] = {"{\"op\":\"layout\",\"widgets\":[{\"id\":\"a\",\"type\":\"gauge\",\"x\":1}]}",
                           "{\"op\":\"set\",\"id\":\"g\",\"value\":1e309,\"text\":\"\\u00e9\\\"x\"}",
                           "[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[", "{\"a\":{\"b\":{\"c\":[1,2,{\"d\":null}]}}}", "\"\\"};
    char buf[4096];
    srand(7);
    for (long it = 0; it < iters; it++) {
        int len;
        if (it % 3 == 0) { len = rand() % (int)sizeof buf; for (int i = 0; i < len; i++) buf[i] = (char)(rand() % 256); }
        else {
            const char *s = seeds[rand() % 5];
            len = (int)strlen(s);
            memcpy(buf, s, (size_t)len);
            for (int k = rand() % 6; k > 0 && len; k--) buf[rand() % len] = "{}[]\",:\\x0 "[rand() % 12];
            if (rand() % 4 == 0 && len) len = rand() % len;
        }
        char *heap = malloc((size_t)len + 1); /* exact-size copy so ASan sees any overread */
        memcpy(heap, buf, (size_t)len);
        exercise(heap, len);
        free(heap);
    }
    printf("fuzz_json: %ld inputs, no crash\n", iters);
    return 0;
}

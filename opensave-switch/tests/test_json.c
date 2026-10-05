#include <stdlib.h>
#include "../core/json.h"
#include "testutil.h"

static os_json *P(const char *s) {
    char err[64];
    return os_json_parse(s, strlen(s), err, sizeof err);
}

static void test_values(void) {
    os_json *d = P(" {\"a\": [1, 2.5, -3, 1e3, true, false, null, \"x\"], \"s\": \"h\\u00e9\\n\\ud83d\\ude00\\\"\", \"n\":{}} ");
    os_jn root, a, it;
    int k = 0;
    CHECK(d != NULL);
    if (!d) return;
    root = os_json_root(d);
    CHECK(os_json_type(d, root) == J_OBJ && os_json_count(d, root) == 3);
    a = os_json_get(d, root, "a");
    CHECK(os_json_count(d, a) == 8);
    for (it = os_json_first(d, a); it >= 0; it = os_json_next(d, it), k++) {
        switch (k) {
        case 0: CHECK(os_json_int(d, it, 0) == 1); break;
        case 1: CHECK(os_json_num(d, it, 0) == 2.5 && os_json_int(d, it, 0) == 2); break;
        case 2: CHECK(os_json_int(d, it, 0) == -3); break;
        case 3: CHECK(os_json_int(d, it, 0) == 1000); break;
        case 4: CHECK(os_json_bool(d, it, 0) == 1); break;
        case 5: CHECK(os_json_bool(d, it, 1) == 0); break;
        case 6: CHECK(os_json_type(d, it) == J_NULL); break;
        case 7: CHECK_STR(os_json_str(d, it), "x"); break;
        }
    }
    CHECK_STR(os_json_get_str(d, root, "s"), "h\xc3\xa9\n\xf0\x9f\x98\x80\"");
    CHECK(os_json_get(d, root, "missing") == OS_JN_NONE);
    CHECK(os_json_int(d, OS_JN_NONE, 7) == 7);
    CHECK(os_json_str(d, a) == NULL);
    os_json_free(d);
}

static void test_node_style_numbers(void) {
    /* Node's fractional mtimeMs, which the Go side also has to accept. */
    os_json *d = P("{\"mtime\":1783279365872.0251,\"big\":9007199254740993,\"size\":4294967296}");
    CHECK(d != NULL);
    if (!d) return;
    CHECK(os_json_int(d, os_json_get(d, 0, "mtime"), -1) == 1783279365872LL);
    CHECK(os_json_int(d, os_json_get(d, 0, "big"), -1) == 9007199254740993LL); /* exact, not via double */
    CHECK(os_json_int(d, os_json_get(d, 0, "size"), -1) == 4294967296LL);
    os_json_free(d);
}

static void test_rejects(void) {
    const char *bad[] = {"", "{", "[1,]", "{\"a\":1,}", "{a:1}", "[1 2]", "01", "1.", "+1", "\"abc",
                         "\"\\x\"", "\"\\ud800\"", "\"\\udc00\"", "\"a\nb\"", "tru", "nul", "{} x",
                         "[", "{\"a\"}", "-", "1e", "\"\\u12\"", "[1]]", "\"a\\u0000b\"", NULL};
    int i;
    for (i = 0; bad[i]; i++) {
        os_json *d = P(bad[i]);
        if (d) fprintf(stderr, "accepted bad json: %s\n", bad[i]);
        CHECK(d == NULL);
        os_json_free(d);
    }
    {
        /* Nesting beyond the limit is refused, not recursed into. */
        char deep[256];
        int n = 0, i2;
        for (i2 = 0; i2 < 100; i2++) deep[n++] = '[';
        for (i2 = 0; i2 < 100; i2++) deep[n++] = ']';
        deep[n] = 0;
        CHECK(P(deep) == NULL);
    }
}

static void test_writer(void) {
    os_sb s;
    char *out;
    size_t n;
    os_sb_init(&s);
    os_sb_putc(&s, '{');
    os_sb_json_str(&s, "k\"\\\n\x01");
    os_sb_printf(&s, ":%d,\"u\":", 42);
    os_sb_json_str(&s, "h\xc3\xa9");
    os_sb_putc(&s, '}');
    out = os_sb_take(&s, &n);
    CHECK(out != NULL);
    if (out) {
        CHECK_STR(out, "{\"k\\\"\\\\\\n\\u0001\":42,\"u\":\"h\xc3\xa9\"}");
        CHECK(n == strlen(out));
        {   /* and it reads back */
            os_json *d = P(out);
            CHECK(d != NULL);
            if (d) {
                CHECK(os_json_int(d, os_json_get(d, 0, "k\"\\\n\x01"), 0) == 42);
                os_json_free(d);
            }
        }
        free(out);
    }
    /* Empty builder still yields a valid empty string. */
    os_sb_init(&s);
    out = os_sb_take(&s, &n);
    CHECK(out != NULL && n == 0 && out[0] == 0);
    free(out);
}

int main(void) {
    test_values();
    test_node_style_numbers();
    test_rejects();
    test_writer();
    return t_finish("json");
}

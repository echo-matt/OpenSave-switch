#include "json.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    os_jtype type;
    int next;       /* next sibling, or -1 */
    int first;      /* first child (arrays and objects), or -1 */
    int count;      /* number of children */
    long key;       /* offset of the member key in the pool, or -1 */
    long str;       /* offset of the string value in the pool (J_STR) */
    int is_int;     /* J_NUM holds an exact integer */
    int64_t i;
    double d;
    int b;
} node;

struct os_json {
    node *nodes;
    int n, cap;
    char *pool;
    size_t plen, pcap;
    const char *p, *end;
    char *err;
    size_t errlen;
    int failed;
};

static void fail(os_json *d, const char *msg) {
    if (!d->failed && d->err && d->errlen) snprintf(d->err, d->errlen, "%s", msg);
    d->failed = 1;
}

static int new_node(os_json *d, os_jtype t) {
    node *nd;
    if (d->n == d->cap) {
        int nc = d->cap ? d->cap * 2 : 64;
        node *nn = (node *)realloc(d->nodes, (size_t)nc * sizeof(node));
        if (!nn) {
            fail(d, "out of memory");
            return -1;
        }
        d->nodes = nn;
        d->cap = nc;
    }
    nd = &d->nodes[d->n];
    memset(nd, 0, sizeof *nd);
    nd->type = t;
    nd->next = -1;
    nd->first = -1;
    nd->key = -1;
    nd->str = -1;
    return d->n++;
}

static long pool_reserve(os_json *d, size_t n) {
    long at = (long)d->plen;
    if (d->plen + n > d->pcap) {
        size_t nc = d->pcap ? d->pcap * 2 : 1024;
        char *np;
        while (nc < d->plen + n) nc *= 2;
        np = (char *)realloc(d->pool, nc);
        if (!np) {
            fail(d, "out of memory");
            return -1;
        }
        d->pool = np;
        d->pcap = nc;
    }
    d->plen += n;
    return at;
}

static void skip_ws(os_json *d) {
    while (d->p < d->end && (*d->p == ' ' || *d->p == '\t' || *d->p == '\n' || *d->p == '\r')) d->p++;
}

static int hex4(const char *p, unsigned *out) {
    unsigned v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

static size_t utf8_put(char *o, unsigned cp) {
    if (cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (char)(0xC0 | (cp >> 6));
        o[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Parses a string literal starting at the opening quote; returns the pool
 * offset of the NUL-terminated decoded text, or -1. */
static long parse_string(os_json *d) {
    const char *p = d->p + 1;
    long at;
    size_t w = 0;
    /* The decoded text is never longer than the escaped text. */
    const char *q = p;
    while (q < d->end && *q != '"') {
        if (*q == '\\') q++;
        q++;
    }
    if (q >= d->end) {
        fail(d, "unterminated string");
        return -1;
    }
    at = pool_reserve(d, (size_t)(q - p) + 1);
    if (at < 0) return -1;
    while (p < q) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20) {
            fail(d, "control character in string");
            return -1;
        }
        if (c != '\\') {
            d->pool[at + (long)w++] = (char)c;
            p++;
            continue;
        }
        p++;
        switch (*p) {
        case '"': d->pool[at + (long)w++] = '"'; p++; break;
        case '\\': d->pool[at + (long)w++] = '\\'; p++; break;
        case '/': d->pool[at + (long)w++] = '/'; p++; break;
        case 'b': d->pool[at + (long)w++] = '\b'; p++; break;
        case 'f': d->pool[at + (long)w++] = '\f'; p++; break;
        case 'n': d->pool[at + (long)w++] = '\n'; p++; break;
        case 'r': d->pool[at + (long)w++] = '\r'; p++; break;
        case 't': d->pool[at + (long)w++] = '\t'; p++; break;
        case 'u': {
            unsigned cp, lo;
            char tmp[4];
            size_t k;
            if (q - p < 5 || hex4(p + 1, &cp) != 0) {
                fail(d, "bad \\u escape");
                return -1;
            }
            p += 5;
            if (cp == 0) {
                /* Every string here ends up in C code that stops at a NUL, so
                 * "a\u0000b" would be read as "a". The protocol has no use for
                 * one; refusing it is what keeps a path from being truncated
                 * into a different, apparently-valid one. */
                fail(d, "NUL character in string");
                return -1;
            }
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (q - p < 6 || p[0] != '\\' || p[1] != 'u' || hex4(p + 2, &lo) != 0 ||
                    lo < 0xDC00 || lo > 0xDFFF) {
                    fail(d, "lone surrogate in string");
                    return -1;
                }
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                p += 6;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                fail(d, "lone surrogate in string");
                return -1;
            }
            k = utf8_put(tmp, cp);
            memcpy(d->pool + at + (long)w, tmp, k);
            w += k;
            break;
        }
        default:
            fail(d, "bad escape");
            return -1;
        }
    }
    d->pool[at + (long)w] = '\0';
    d->p = q + 1;
    return at;
}

static int parse_value(os_json *d, int depth);

static int parse_number(os_json *d) {
    const char *s = d->p, *p = s;
    int isint = 1, id;
    char buf[64];
    size_t n;
    if (p < d->end && *p == '-') p++;
    if (p >= d->end) { fail(d, "bad number"); return -1; }
    if (*p == '0') p++;
    else if (*p >= '1' && *p <= '9') while (p < d->end && *p >= '0' && *p <= '9') p++;
    else { fail(d, "bad number"); return -1; }
    if (p < d->end && *p == '.') {
        isint = 0;
        p++;
        if (p >= d->end || *p < '0' || *p > '9') { fail(d, "bad number"); return -1; }
        while (p < d->end && *p >= '0' && *p <= '9') p++;
    }
    if (p < d->end && (*p == 'e' || *p == 'E')) {
        isint = 0;
        p++;
        if (p < d->end && (*p == '+' || *p == '-')) p++;
        if (p >= d->end || *p < '0' || *p > '9') { fail(d, "bad number"); return -1; }
        while (p < d->end && *p >= '0' && *p <= '9') p++;
    }
    n = (size_t)(p - s);
    if (n >= sizeof buf) { fail(d, "number too long"); return -1; }
    memcpy(buf, s, n);
    buf[n] = '\0';
    id = new_node(d, J_NUM);
    if (id < 0) return -1;
    errno = 0;
    d->nodes[id].d = strtod(buf, NULL);
    if (isint) {
        long long v = strtoll(buf, NULL, 10);
        if (errno == 0) {
            d->nodes[id].is_int = 1;
            d->nodes[id].i = v;
        }
    }
    d->p = p;
    return id;
}

static int parse_container(os_json *d, int depth, int is_obj) {
    int id = new_node(d, is_obj ? J_OBJ : J_ARR), last = -1;
    char close = is_obj ? '}' : ']';
    if (id < 0) return -1;
    d->p++; /* opening bracket */
    skip_ws(d);
    if (d->p < d->end && *d->p == close) {
        d->p++;
        return id;
    }
    for (;;) {
        long key = -1;
        int child;
        skip_ws(d);
        if (is_obj) {
            if (d->p >= d->end || *d->p != '"') { fail(d, "expected object key"); return -1; }
            key = parse_string(d);
            if (key < 0) return -1;
            skip_ws(d);
            if (d->p >= d->end || *d->p != ':') { fail(d, "expected ':'"); return -1; }
            d->p++;
        }
        child = parse_value(d, depth + 1);
        if (child < 0) return -1;
        d->nodes[child].key = key;
        if (last < 0) d->nodes[id].first = child;
        else d->nodes[last].next = child;
        last = child;
        d->nodes[id].count++;
        skip_ws(d);
        if (d->p >= d->end) { fail(d, "unterminated value"); return -1; }
        if (*d->p == ',') { d->p++; continue; }
        if (*d->p == close) { d->p++; return id; }
        fail(d, "expected ',' or closing bracket");
        return -1;
    }
}

static int parse_value(os_json *d, int depth) {
    int id;
    skip_ws(d);
    if (depth > OS_JSON_MAX_DEPTH) { fail(d, "nested too deeply"); return -1; }
    if (d->p >= d->end) { fail(d, "unexpected end of input"); return -1; }
    switch (*d->p) {
    case '{': return parse_container(d, depth, 1);
    case '[': return parse_container(d, depth, 0);
    case '"': {
        long s = parse_string(d);
        if (s < 0) return -1;
        id = new_node(d, J_STR);
        if (id < 0) return -1;
        d->nodes[id].str = s;
        return id;
    }
    case 't':
        if (d->end - d->p >= 4 && !memcmp(d->p, "true", 4)) {
            d->p += 4;
            id = new_node(d, J_BOOL);
            if (id >= 0) d->nodes[id].b = 1;
            return id;
        }
        break;
    case 'f':
        if (d->end - d->p >= 5 && !memcmp(d->p, "false", 5)) {
            d->p += 5;
            return new_node(d, J_BOOL);
        }
        break;
    case 'n':
        if (d->end - d->p >= 4 && !memcmp(d->p, "null", 4)) {
            d->p += 4;
            return new_node(d, J_NULL);
        }
        break;
    default:
        if (*d->p == '-' || (*d->p >= '0' && *d->p <= '9')) return parse_number(d);
    }
    fail(d, "unexpected character");
    return -1;
}

os_json *os_json_parse(const char *text, size_t len, char *err, size_t errlen) {
    os_json *d = (os_json *)calloc(1, sizeof *d);
    if (!d) {
        if (err && errlen) snprintf(err, errlen, "out of memory");
        return NULL;
    }
    d->p = text;
    d->end = text + len;
    d->err = err;
    d->errlen = errlen;
    if (err && errlen) err[0] = '\0';
    if (parse_value(d, 0) < 0) {
        os_json_free(d);
        return NULL;
    }
    skip_ws(d);
    if (d->p != d->end) {
        fail(d, "trailing data after value");
        os_json_free(d);
        return NULL;
    }
    return d;
}

void os_json_free(os_json *d) {
    if (!d) return;
    free(d->nodes);
    free(d->pool);
    free(d);
}

static const node *nd(const os_json *d, os_jn n) {
    return (n >= 0 && n < d->n) ? &d->nodes[n] : NULL;
}

os_jn os_json_root(const os_json *d) { return d->n > 0 ? 0 : OS_JN_NONE; }

os_jtype os_json_type(const os_json *d, os_jn n) {
    const node *x = nd(d, n);
    return x ? x->type : J_NULL;
}

os_jn os_json_first(const os_json *d, os_jn n) {
    const node *x = nd(d, n);
    return (x && (x->type == J_ARR || x->type == J_OBJ)) ? x->first : OS_JN_NONE;
}

os_jn os_json_next(const os_json *d, os_jn n) {
    const node *x = nd(d, n);
    return x ? x->next : OS_JN_NONE;
}

size_t os_json_count(const os_json *d, os_jn n) {
    const node *x = nd(d, n);
    return (x && (x->type == J_ARR || x->type == J_OBJ)) ? (size_t)x->count : 0;
}

const char *os_json_key(const os_json *d, os_jn member) {
    const node *x = nd(d, member);
    return (x && x->key >= 0) ? d->pool + x->key : NULL;
}

os_jn os_json_get(const os_json *d, os_jn obj, const char *key) {
    const node *o = nd(d, obj);
    os_jn c;
    if (!o || o->type != J_OBJ) return OS_JN_NONE;
    for (c = o->first; c >= 0; c = d->nodes[c].next)
        if (d->nodes[c].key >= 0 && strcmp(d->pool + d->nodes[c].key, key) == 0) return c;
    return OS_JN_NONE;
}

const char *os_json_str(const os_json *d, os_jn n) {
    const node *x = nd(d, n);
    return (x && x->type == J_STR) ? d->pool + x->str : NULL;
}

const char *os_json_get_str(const os_json *d, os_jn obj, const char *key) {
    return os_json_str(d, os_json_get(d, obj, key));
}

int64_t os_json_int(const os_json *d, os_jn n, int64_t def) {
    const node *x = nd(d, n);
    if (!x || x->type != J_NUM) return def;
    if (x->is_int) return x->i;
    /* A fractional number (Node's fractional mtime) truncates; one outside
     * the int64 range is not a value this can hold. */
    if (x->d >= 9.2e18 || x->d <= -9.2e18) return def;
    return (int64_t)x->d;
}

double os_json_num(const os_json *d, os_jn n, double def) {
    const node *x = nd(d, n);
    return (x && x->type == J_NUM) ? x->d : def;
}

int os_json_bool(const os_json *d, os_jn n, int def) {
    const node *x = nd(d, n);
    return (x && x->type == J_BOOL) ? x->b : def;
}

/* ------------------------------------------------------------------ writer */

void os_sb_init(os_sb *s) { memset(s, 0, sizeof *s); }

void os_sb_free(os_sb *s) {
    free(s->data);
    memset(s, 0, sizeof *s);
}

static int sb_grow(os_sb *s, size_t need) {
    if (s->oom) return -1;
    if (s->len + need + 1 > s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 256;
        char *np;
        while (nc < s->len + need + 1) nc *= 2;
        np = (char *)realloc(s->data, nc);
        if (!np) {
            s->oom = 1;
            return -1;
        }
        s->data = np;
        s->cap = nc;
    }
    return 0;
}

void os_sb_put(os_sb *s, const char *p, size_t n) {
    if (sb_grow(s, n) != 0) return;
    memcpy(s->data + s->len, p, n);
    s->len += n;
    s->data[s->len] = '\0';
}

void os_sb_putc(os_sb *s, char c) { os_sb_put(s, &c, 1); }
void os_sb_puts(os_sb *s, const char *p) { os_sb_put(s, p, strlen(p)); }

void os_sb_printf(os_sb *s, const char *fmt, ...) {
    va_list ap, ap2;
    int n;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n >= 0 && sb_grow(s, (size_t)n) == 0) {
        vsnprintf(s->data + s->len, (size_t)n + 1, fmt, ap2);
        s->len += (size_t)n;
    }
    va_end(ap2);
}

void os_sb_json_str(os_sb *s, const char *p) {
    os_sb_putc(s, '"');
    for (; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"': os_sb_puts(s, "\\\""); break;
        case '\\': os_sb_puts(s, "\\\\"); break;
        case '\n': os_sb_puts(s, "\\n"); break;
        case '\r': os_sb_puts(s, "\\r"); break;
        case '\t': os_sb_puts(s, "\\t"); break;
        default:
            if (c < 0x20) os_sb_printf(s, "\\u%04x", c);
            else os_sb_putc(s, (char)c);
        }
    }
    os_sb_putc(s, '"');
}

char *os_sb_take(os_sb *s, size_t *len) {
    char *r;
    if (s->oom) {
        os_sb_free(s);
        return NULL;
    }
    if (!s->data) {
        if (sb_grow(s, 0) != 0) return NULL;
        s->data[0] = '\0'; /* an empty builder still yields a valid empty string */
    }
    if (len) *len = s->len;
    r = s->data;
    memset(s, 0, sizeof *s);
    return r;
}

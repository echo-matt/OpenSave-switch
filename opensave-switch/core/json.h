/* Minimal JSON for the OpenSave wire protocol.
 *
 * The parser builds a flat node pool that the document owns, so there is one
 * allocation to free and no pointer into the input to outlive. It is strict
 * (RFC 8259), bounded in nesting depth, and decodes string escapes — including
 * \u surrogate pairs — to UTF-8. The writer is a growable string builder with
 * the escaping the protocol needs.
 */
#ifndef OPENSAVE_JSON_H
#define OPENSAVE_JSON_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } os_jtype;

typedef struct os_json os_json;
typedef int os_jn; /* a node in a document; OS_JN_NONE is "no such node" */
#define OS_JN_NONE (-1)
#define OS_JSON_MAX_DEPTH 24

/* Parses text. On failure returns NULL and, when err is non-NULL, writes a
 * short reason to it. */
os_json *os_json_parse(const char *text, size_t len, char *err, size_t errlen);
void os_json_free(os_json *d);

os_jn os_json_root(const os_json *d);
os_jtype os_json_type(const os_json *d, os_jn n);

/* Object member by key; OS_JN_NONE if n is not an object or lacks the key. */
os_jn os_json_get(const os_json *d, os_jn obj, const char *key);

/* Children of an array or object, in order. For an object member,
 * os_json_key gives its key. */
os_jn os_json_first(const os_json *d, os_jn n);
os_jn os_json_next(const os_json *d, os_jn n);
size_t os_json_count(const os_json *d, os_jn n);
const char *os_json_key(const os_json *d, os_jn member);

/* Accessors tolerate OS_JN_NONE and a wrong type, returning the default, so a
 * missing optional field reads as absent rather than needing a check each. */
const char *os_json_str(const os_json *d, os_jn n);                 /* NULL if not a string */
const char *os_json_get_str(const os_json *d, os_jn obj, const char *key); /* NULL if absent */
int64_t os_json_int(const os_json *d, os_jn n, int64_t def);        /* truncates fractions */
double os_json_num(const os_json *d, os_jn n, double def);
int os_json_bool(const os_json *d, os_jn n, int def);

/* ----------------------------------------------------------------- writer */

typedef struct {
    char *data;
    size_t len, cap;
    int oom;
} os_sb;

void os_sb_init(os_sb *s);
void os_sb_free(os_sb *s);
void os_sb_putc(os_sb *s, char c);
void os_sb_put(os_sb *s, const char *p, size_t n);
void os_sb_puts(os_sb *s, const char *p);
void os_sb_printf(os_sb *s, const char *fmt, ...);
/* Writes a JSON string literal, quotes and escapes included. */
void os_sb_json_str(os_sb *s, const char *p);
/* Hands over the buffer (NUL-terminated, to be free()d); NULL if out of memory. */
char *os_sb_take(os_sb *s, size_t *len);

#endif

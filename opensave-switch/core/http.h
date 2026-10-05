/* A small HTTP/1.1 client and server over BSD sockets.
 *
 * Both are deliberately minimal: IPv4 literals only, one request per
 * connection ("Connection: close"), bodies held in memory with a caller-set
 * ceiling, and a single-threaded server that the UI loop polls between frames.
 * That is all the LAN protocol needs, and it keeps one implementation working
 * on a host (for the tests) and on the console, where libnx provides the same
 * socket API.
 */
#ifndef OPENSAVE_HTTP_H
#define OPENSAVE_HTTP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    const char *value;
} os_hdr;

typedef struct {
    int status;
    char *body; /* NUL-terminated for convenience; bodylen excludes the NUL */
    size_t bodylen;
    int64_t server_time; /* unix seconds from the Date header; 0 if absent */
} os_http_resp;

void os_http_resp_free(os_http_resp *r);

/* Performs one request. Returns 0 if a response was received (check
 * out->status), -1 on a transport failure with a reason in err. host must be a
 * dotted IPv4 address. */
int os_http_request(const char *host, int port, const char *method, const char *target,
                    const os_hdr *hdrs, int nhdrs, const void *body, size_t bodylen,
                    size_t max_body, int timeout_ms, os_http_resp *out, char *err,
                    size_t errlen);

/* The same over TLS when tls is nonzero. host may be a name (resolved to IPv4),
 * and it is what TLS checks the server's certificate against. The Host header
 * carries the name alone; callers that use a non-default port are talking to
 * OpenSave devices, which do not care. */
int os_http_request_tls(const char *host, int port, int tls, const char *method, const char *target,
                        const os_hdr *hdrs, int nhdrs, const void *body, size_t bodylen, size_t max_body,
                        int timeout_ms, os_http_resp *out, char *err, size_t errlen);

/* TLS is supplied by the platform (the console has a system service for it; a PC
 * test can supply a stand-in). Without it, https requests fail with a clear error. */
typedef struct {
    /* Starts a session over an already-connected socket and completes the
     * handshake, verifying the server's certificate for host. NULL on failure,
     * with a reason in err. */
    void *(*open)(int fd, const char *host, char *err, size_t errlen);
    int (*send)(void *tls, const void *buf, size_t len); /* bytes sent, or < 0 */
    int (*recv)(void *tls, void *buf, size_t len);       /* bytes, 0 at the end, < 0 on error */
    void (*close)(void *tls);
} os_tls_hooks;
void os_http_set_tls(const os_tls_hooks *h);

/* Parses an HTTP-date ("Mon, 02 Jan 2006 15:04:05 GMT") to unix seconds, or 0. */
int64_t os_http_parse_date(const char *s);

/* ------------------------------------------------------------------ server */

#define OS_HTTP_MAX_HEADER 16384
#define OS_HTTP_MAX_REQ_BODY (1024 * 1024)

typedef struct {
    char method[8];
    char target[2048]; /* request-target exactly as received: path and query */
    char path[1024];   /* the path part alone */
    char client_ip[48];
    char *headers; /* raw header lines, CRLF separated; for os_http_req_header */
    char *body;
    size_t bodylen;
} os_http_req;

/* Case-insensitive header lookup into the raw header block. The value is
 * copied into out; returns 0 if present, -1 if not. */
int os_http_req_header(const os_http_req *r, const char *name, char *out, size_t outlen);

/* Handler: fill *status, and *body with a malloc'd response (or NULL for none).
 * The server frees it. Return 0. */
typedef int (*os_http_handler)(void *ctx, const os_http_req *req, int *status, char **body,
                               size_t *bodylen);

typedef struct os_http_server os_http_server;

/* Listens on all interfaces. port 0 picks a free one. */
os_http_server *os_http_server_start(int port, char *err, size_t errlen);
int os_http_server_port(const os_http_server *s);
/* Waits up to timeout_ms for a connection and serves it to completion.
 * Returns 1 if a request was served, 0 if none arrived, -1 on a listener error. */
int os_http_server_poll(os_http_server *s, int timeout_ms, os_http_handler h, void *ctx);
void os_http_server_stop(os_http_server *s);

#endif

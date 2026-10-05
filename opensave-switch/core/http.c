#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static void seterr(char *err, size_t n, const char *msg) {
    if (err && n) snprintf(err, n, "%s", msg);
}

void os_http_resp_free(os_http_resp *r) {
    if (!r) return;
    free(r->body);
    r->body = NULL;
    r->bodylen = 0;
}

/* ------------------------------------------------------------------- dates */

static int month_index(const char *m) {
    static const char *names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int i;
    for (i = 0; i < 12; i++)
        if (strncmp(m, names[i], 3) == 0) return i + 1;
    return 0;
}

/* Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant). */
static int64_t days_from_civil(int64_t y, int m, int d) {
    int64_t era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int64_t os_http_parse_date(const char *s) {
    int d, y, hh, mm, ss, mon;
    char mname[4];
    /* "Mon, 02 Jan 2006 15:04:05 GMT" */
    if (!s || sscanf(s, "%*3s, %d %3s %d %d:%d:%d", &d, mname, &y, &hh, &mm, &ss) != 6) return 0;
    mon = month_index(mname);
    if (!mon || d < 1 || d > 31 || hh > 23 || mm > 59 || ss > 60 || y < 1970) return 0;
    return days_from_civil(y, mon, d) * 86400 + hh * 3600 + mm * 60 + ss;
}

/* ----------------------------------------------------------------- sockets */

static int set_nonblock(int fd, int on) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

static void set_timeouts(int fd, int timeout_ms) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

static int wait_fd(int fd, int for_write, int timeout_ms) {
    fd_set set;
    struct timeval tv;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return select(fd + 1, for_write ? NULL : &set, for_write ? &set : NULL, NULL, &tv);
}

/* A connection: a socket, and a TLS session over it when there is one. */
typedef struct {
    int fd;
    void *tls;
} conn;

static os_tls_hooks g_tls;
static int g_have_tls;

void os_http_set_tls(const os_tls_hooks *h) {
    if (h) {
        g_tls = *h;
        g_have_tls = 1;
    } else {
        g_have_tls = 0;
    }
}

static int send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int connect_to(const char *host, int port, int timeout_ms, char *err, size_t errlen) {
    struct sockaddr_in sa;
    int fd, rc;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        /* Not a dotted address: a name, which needs resolving (IPv4 only). */
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
            seterr(err, errlen, "could not look up that name (is the network up?)");
            return -1;
        }
        sa.sin_addr = ((struct sockaddr_in *)(void *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        seterr(err, errlen, "could not create a socket");
        return -1;
    }
    /* A connect that never answers must not hang the console, so it runs
     * non-blocking and is waited on. */
    set_nonblock(fd, 1);
    rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    if (rc < 0 && errno != EINPROGRESS && errno != EWOULDBLOCK) {
        close(fd);
        seterr(err, errlen, "could not connect");
        return -1;
    }
    if (rc < 0) {
        int so = 0;
        socklen_t sl = sizeof so;
        if (wait_fd(fd, 1, timeout_ms) <= 0) {
            close(fd);
            seterr(err, errlen, "connection timed out");
            return -1;
        }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl) != 0 || so != 0) {
            close(fd);
            seterr(err, errlen, so == ECONNREFUSED ? "connection refused — is OpenSave running there?"
                                                   : "could not connect");
            return -1;
        }
    }
    set_nonblock(fd, 0);
    set_timeouts(fd, timeout_ms);
    return fd;
}

/* ------------------------------------------------------------ message parse */

typedef struct {
    char *d;
    size_t len, cap;
} buf;

static int buf_append(buf *b, const char *p, size_t n, size_t max) {
    if (b->len + n > max) return -1;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        char *np;
        while (nc < b->len + n + 1) nc *= 2;
        np = (char *)realloc(b->d, nc);
        if (!np) return -1;
        b->d = np;
        b->cap = nc;
    }
    memcpy(b->d + b->len, p, n);
    b->len += n;
    b->d[b->len] = '\0';
    return 0;
}

static char *find_crlfcrlf(char *p, size_t len) {
    size_t i;
    for (i = 0; i + 3 < len; i++)
        if (p[i] == '\r' && p[i + 1] == '\n' && p[i + 2] == '\r' && p[i + 3] == '\n') return p + i;
    return NULL;
}

/* Finds header `name` in a CRLF-separated block, copying its trimmed value. */
static int header_in(const char *block, const char *name, char *out, size_t outlen) {
    size_t nl = strlen(name);
    const char *p = block;
    while (*p) {
        const char *eol = strstr(p, "\r\n");
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        if (linelen > nl && p[nl] == ':' && strncasecmp(p, name, nl) == 0) {
            const char *v = p + nl + 1, *e = p + linelen;
            size_t n;
            while (v < e && (*v == ' ' || *v == '\t')) v++;
            while (e > v && (e[-1] == ' ' || e[-1] == '\t')) e--;
            n = (size_t)(e - v);
            if (n >= outlen) n = outlen - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            return 0;
        }
        if (!eol) break;
        p = eol + 2;
    }
    return -1;
}

/* Reads from fd into b until `done` says the message is complete or the peer
 * closes. Returns 0 on EOF/complete, -1 on error or oversize. */
static int recv_more(conn *c, buf *b, size_t max) {
    char tmp[8192];
    ssize_t n = c->tls ? (ssize_t)g_tls.recv(c->tls, tmp, sizeof tmp) : recv(c->fd, tmp, sizeof tmp, 0);
    if (n < 0) {
        if (!c->tls && errno == EINTR) return 1;
        return -1;
    }
    if (n == 0) return 0;
    if (buf_append(b, tmp, (size_t)n, max) != 0) return -1;
    return 1;
}

/* Decodes a chunked body in place from b->d+start. Returns the decoded length,
 * -1 if malformed, -2 if more data is needed. */
static long dechunk(char *src, size_t srclen, char *dst) {
    size_t pos = 0, out = 0;
    for (;;) {
        char *eol;
        unsigned long sz;
        char *endp;
        if (pos >= srclen) return -2;
        eol = memchr(src + pos, '\n', srclen - pos);
        if (!eol) return -2;
        sz = strtoul(src + pos, &endp, 16);
        if (endp == src + pos) return -1;
        pos = (size_t)(eol - src) + 1;
        if (sz == 0) return (long)out; /* trailers, if any, are ignored */
        if (pos + sz + 2 > srclen) return -2;
        memmove(dst + out, src + pos, sz);
        out += sz;
        pos += sz;
        if (src[pos] != '\r' || src[pos + 1] != '\n') return -1;
        pos += 2;
    }
}

int os_http_request(const char *host, int port, const char *method, const char *target,
                    const os_hdr *hdrs, int nhdrs, const void *body, size_t bodylen,
                    size_t max_body, int timeout_ms, os_http_resp *out, char *err,
                    size_t errlen) {
    return os_http_request_tls(host, port, 0, method, target, hdrs, nhdrs, body, bodylen, max_body, timeout_ms, out,
                               err, errlen);
}

static void conn_close(conn *c) {
    if (c->tls) g_tls.close(c->tls);
    close(c->fd);
}

static int conn_send(conn *c, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    if (!c->tls) return send_all(c->fd, buf, len);
    while (len) {
        int n = g_tls.send(c->tls, p, len);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int os_http_request_tls(const char *host, int port, int tls, const char *method, const char *target,
                        const os_hdr *hdrs, int nhdrs, const void *body, size_t bodylen, size_t max_body,
                        int timeout_ms, os_http_resp *out, char *err, size_t errlen) {
    int fd, i, rc;
    conn cc;
    char head[4096];
    int hn;
    buf in = {0, 0, 0};
    char *hend = NULL;
    size_t max_total = max_body + OS_HTTP_MAX_HEADER;
    char clen[32] = "", te[32] = "", datehdr[64] = "";
    char *bodystart;
    long want = -1;
    int chunked = 0;

    memset(out, 0, sizeof *out);
    if (tls && !g_have_tls) {
        seterr(err, errlen, "this build has no TLS, so it cannot reach an https address");
        return -1;
    }
    fd = connect_to(host, port, timeout_ms, err, errlen);
    if (fd < 0) return -1;
    cc.fd = fd;
    cc.tls = NULL;
    if (tls) {
        char terr2[120] = "";
        cc.tls = g_tls.open(fd, host, terr2, sizeof terr2);
        if (!cc.tls) {
            conn_close(&cc);
            seterr(err, errlen, terr2[0] ? terr2 : "the secure connection could not be set up");
            return -1;
        }
    }

    hn = snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n",
                  method, target, host);
    for (i = 0; i < nhdrs && hn > 0 && (size_t)hn < sizeof head; i++)
        hn += snprintf(head + hn, sizeof head - (size_t)hn, "%s: %s\r\n", hdrs[i].name, hdrs[i].value);
    if (body || strcmp(method, "POST") == 0)
        hn += snprintf(head + hn, sizeof head - (size_t)hn,
                       "Content-Type: application/json\r\nContent-Length: %zu\r\n", bodylen);
    if (hn <= 0 || (size_t)hn + 2 >= sizeof head) {
        conn_close(&cc);
        seterr(err, errlen, "request headers too large");
        return -1;
    }
    hn += snprintf(head + hn, sizeof head - (size_t)hn, "\r\n");
    if (conn_send(&cc, head, (size_t)hn) != 0 || (bodylen && conn_send(&cc, body, bodylen) != 0)) {
        conn_close(&cc);
        seterr(err, errlen, "connection lost while sending");
        return -1;
    }

    /* Headers. */
    for (;;) {
        hend = find_crlfcrlf(in.d ? in.d : "", in.len);
        if (hend) break;
        rc = recv_more(&cc, &in, OS_HTTP_MAX_HEADER);
        if (rc <= 0) {
            conn_close(&cc);
            free(in.d);
            seterr(err, errlen, rc == 0 ? "connection closed before a reply" : "no reply (timed out)");
            return -1;
        }
    }
    *hend = '\0'; /* terminate the header block */
    if (sscanf(in.d, "HTTP/%*d.%*d %d", &out->status) != 1) {
        conn_close(&cc);
        free(in.d);
        seterr(err, errlen, "malformed reply");
        return -1;
    }
    header_in(in.d, "Content-Length", clen, sizeof clen);
    header_in(in.d, "Transfer-Encoding", te, sizeof te);
    if (header_in(in.d, "Date", datehdr, sizeof datehdr) == 0) out->server_time = os_http_parse_date(datehdr);
    chunked = strncasecmp(te, "chunked", 7) == 0;
    if (clen[0]) {
        char *e;
        want = strtol(clen, &e, 10);
        if (*e || want < 0 || (size_t)want > max_body) {
            conn_close(&cc);
            free(in.d);
            seterr(err, errlen, "reply body too large");
            return -1;
        }
    }
    bodystart = hend + 4;

    /* Body. in.d may be reallocated while reading, so re-derive pointers. */
    {
        size_t off = (size_t)(bodystart - in.d);
        for (;;) {
            size_t have = in.len - off;
            if (!chunked && want >= 0 && have >= (size_t)want) break;
            if (chunked) {
                /* Try decoding what is here; -2 means wait for more. */
                char *tmp = (char *)malloc(have + 1);
                long r;
                if (!tmp) { conn_close(&cc); free(in.d); seterr(err, errlen, "out of memory"); return -1; }
                r = dechunk(in.d + off, have, tmp);
                if (r >= 0) {
                    out->body = tmp;
                    out->bodylen = (size_t)r;
                    out->body[r] = '\0';
                    conn_close(&cc);
                    free(in.d);
                    return 0;
                }
                free(tmp);
                if (r == -1) { conn_close(&cc); free(in.d); seterr(err, errlen, "malformed chunked reply"); return -1; }
            }
            rc = recv_more(&cc, &in, max_total);
            if (rc < 0) { conn_close(&cc); free(in.d); seterr(err, errlen, "reply too large or interrupted"); return -1; }
            if (rc == 0) {
                if (chunked || want >= 0) {
                    /* Ended early: a truncated body must not be taken for a whole one. */
                    conn_close(&cc);
                    free(in.d);
                    seterr(err, errlen, "connection closed mid-reply");
                    return -1;
                }
                break; /* no length: the body runs to the end of the stream */
            }
        }
        {
            size_t have = in.len - off;
            size_t n = (want >= 0) ? (size_t)want : have;
            out->body = (char *)malloc(n + 1);
            if (!out->body) { conn_close(&cc); free(in.d); seterr(err, errlen, "out of memory"); return -1; }
            memcpy(out->body, in.d + off, n);
            out->body[n] = '\0';
            out->bodylen = n;
        }
    }
    conn_close(&cc);
    free(in.d);
    return 0;
}

/* ------------------------------------------------------------------ server */

struct os_http_server {
    int fd;
    int port;
};

os_http_server *os_http_server_start(int port, char *err, size_t errlen) {
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    int one = 1;
    os_http_server *s = (os_http_server *)calloc(1, sizeof *s);
    if (!s) { seterr(err, errlen, "out of memory"); return NULL; }
    s->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->fd < 0) { free(s); seterr(err, errlen, "could not create a socket"); return NULL; }
    setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)port);
    if (bind(s->fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(s->fd, 8) != 0) {
        close(s->fd);
        free(s);
        seterr(err, errlen, "could not listen on that port (is it in use?)");
        return NULL;
    }
    set_nonblock(s->fd, 1);
    if (getsockname(s->fd, (struct sockaddr *)&sa, &sl) == 0) s->port = ntohs(sa.sin_port);
    else s->port = port;
    return s;
}

int os_http_server_port(const os_http_server *s) { return s->port; }

void os_http_server_stop(os_http_server *s) {
    if (!s) return;
    close(s->fd);
    free(s);
}

int os_http_req_header(const os_http_req *r, const char *name, char *out, size_t outlen) {
    if (!r->headers) return -1;
    return header_in(r->headers, name, out, outlen);
}

static void write_response(int fd, int status, const char *body, size_t bodylen) {
    char head[256];
    const char *reason = status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 401 ? "Unauthorized"
                       : status == 404 ? "Not Found" : status == 413 ? "Payload Too Large"
                       : status == 503 ? "Service Unavailable" : "Error";
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\n\r\n", status, reason, bodylen);
    if (send_all(fd, head, (size_t)n) == 0 && bodylen) send_all(fd, body, bodylen);
}

static void write_error(int fd, int status, const char *msg) {
    {
        char body[256];
        int n = snprintf(body, sizeof body, "{\"error\":\"%s\"}", msg);
        write_response(fd, status, body, (size_t)n);
    }
}

/* How long one client may take to send its whole request. The per-read
 * timeout alone is not enough: a client sending a byte every second would
 * keep resetting it, and a server that serves one connection at a time — as
 * this one does, from the interface's own loop — would be held for as long as
 * it liked. */
#define OS_REQUEST_DEADLINE_S 5

int os_http_server_poll(os_http_server *s, int timeout_ms, os_http_handler h, void *ctx) {
    struct sockaddr_in ca;
    socklen_t cl = sizeof ca;
    int fd;
    buf in = {0, 0, 0};
    char *hend;
    os_http_req req;
    char clen[24] = "";
    long want = 0;
    int status = 500;
    char *rbody = NULL;
    size_t rlen = 0;
    int rc;
    time_t deadline;
    conn sc;

    rc = wait_fd(s->fd, 0, timeout_ms);
    if (rc < 0) return errno == EINTR ? 0 : -1;
    if (rc == 0) return 0;
    fd = accept(s->fd, (struct sockaddr *)&ca, &cl);
    if (fd < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
    set_nonblock(fd, 0);
    /* A client that connects and says nothing must not freeze the screen. */
    set_timeouts(fd, 3000);
    sc.fd = fd;
    sc.tls = NULL;
    deadline = time(NULL) + OS_REQUEST_DEADLINE_S;

    memset(&req, 0, sizeof req);
    inet_ntop(AF_INET, &ca.sin_addr, req.client_ip, sizeof req.client_ip);

    for (;;) {
        hend = find_crlfcrlf(in.d ? in.d : "", in.len);
        if (hend) break;
        if (time(NULL) > deadline) goto done;
        rc = recv_more(&sc, &in, OS_HTTP_MAX_HEADER);
        if (rc <= 0) {
            if (rc < 0 && in.len >= OS_HTTP_MAX_HEADER) write_error(fd, 400, "headers too large");
            goto done;
        }
    }
    *hend = '\0';
    {
        char *eol = strstr(in.d, "\r\n");
        char version[16];
        char line[2300];
        size_t ll = eol ? (size_t)(eol - in.d) : strlen(in.d);
        if (ll >= sizeof line) { write_error(fd, 400, "request line too long"); goto done; }
        memcpy(line, in.d, ll);
        line[ll] = '\0';
        if (sscanf(line, "%7s %2047s %15s", req.method, req.target, version) != 3 ||
            strncmp(version, "HTTP/1.", 7) != 0) {
            write_error(fd, 400, "bad request line");
            goto done;
        }
        req.headers = eol ? eol + 2 : (char *)"";
    }
    {
        char *q = strchr(req.target, '?');
        size_t pl = q ? (size_t)(q - req.target) : strlen(req.target);
        if (pl >= sizeof req.path) { write_error(fd, 400, "path too long"); goto done; }
        memcpy(req.path, req.target, pl);
        req.path[pl] = '\0';
    }
    if (header_in(req.headers, "Transfer-Encoding", clen, sizeof clen) == 0) {
        write_error(fd, 400, "chunked request bodies are not supported");
        goto done;
    }
    clen[0] = '\0';
    if (header_in(req.headers, "Content-Length", clen, sizeof clen) == 0) {
        char *e;
        want = strtol(clen, &e, 10);
        if (*e || want < 0) { write_error(fd, 400, "bad Content-Length"); goto done; }
        if ((size_t)want > OS_HTTP_MAX_REQ_BODY) { write_error(fd, 413, "request body too large"); goto done; }
    }
    {
        size_t off = (size_t)(hend + 4 - in.d);
        while (in.len - off < (size_t)want) {
            if (time(NULL) > deadline) { write_error(fd, 400, "request too slow"); goto done; }
            rc = recv_more(&sc, &in, OS_HTTP_MAX_HEADER + OS_HTTP_MAX_REQ_BODY);
            if (rc <= 0) { write_error(fd, 400, "incomplete request body"); goto done; }
        }
        req.body = in.d + off;
        req.bodylen = (size_t)want;
        /* The body is NUL-terminated for the handler's convenience: bytes past
         * the declared length are not part of this request. */
        req.body[req.bodylen] = '\0';
    }
    if (h(ctx, &req, &status, &rbody, &rlen) != 0) status = 500;
    write_response(fd, status, rbody ? rbody : "", rbody ? rlen : 0);
    free(rbody);
    close(fd);
    free(in.d);
    return 1;
done:
    close(fd);
    free(in.d);
    return 1;
}

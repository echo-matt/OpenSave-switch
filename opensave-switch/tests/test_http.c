#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../core/http.h"
#include "testutil.h"

/* ---- an echo server on a thread ---- */

static int echo(void *ctx, const os_http_req *r, int *status, char **body, size_t *len) {
    char hv[64] = "none";
    char *out;
    (void)ctx;
    os_http_req_header(r, "x-opensave-auth", hv, sizeof hv);
    if (strcmp(r->path, "/big") == 0) {
        size_t n = 3 * 1024 * 1024, i;
        out = malloc(n);
        for (i = 0; i < n; i++) out[i] = (char)('a' + i % 26);
        *body = out;
        *len = n;
        *status = 200;
        return 0;
    }
    out = malloc(r->bodylen + 512);
    *len = (size_t)sprintf(out, "%s|%s|%s|%s|", r->method, r->path, r->target, hv);
    memcpy(out + *len, r->body, r->bodylen);
    *len += r->bodylen;
    *status = strcmp(r->path, "/missing") == 0 ? 404 : 200;
    *body = out;
    return 0;
}

static volatile int stop_server;

static void *serve(void *p) {
    os_http_server *s = p;
    while (!stop_server) os_http_server_poll(s, 50, echo, NULL);
    return NULL;
}

/* ---- a raw server that sends a canned reply, to test odd framings ---- */

typedef struct {
    int fd;
    const char *parts[8];
    int nparts;
} raw_t;

static void *raw_serve(void *p) {
    raw_t *r = p;
    int c = accept(r->fd, NULL, NULL), i;
    char tmp[4096];
    if (c < 0) return NULL;
    (void)!read(c, tmp, sizeof tmp);
    for (i = 0; i < r->nparts; i++) {
        (void)!write(c, r->parts[i], strlen(r->parts[i]));
        usleep(20000); /* force separate TCP segments */
    }
    close(c);
    return NULL;
}

static int raw_listen(int *port) {
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) || listen(fd, 1)) return -1;
    getsockname(fd, (struct sockaddr *)&sa, &sl);
    *port = ntohs(sa.sin_port);
    return fd;
}

static int raw_exchange(const char **parts, int n, os_http_resp *resp, char *err) {
    raw_t r;
    pthread_t th;
    int port, rc;
    r.fd = raw_listen(&port);
    r.nparts = n;
    memcpy(r.parts, parts, (size_t)n * sizeof(char *));
    pthread_create(&th, NULL, raw_serve, &r);
    rc = os_http_request("127.0.0.1", port, "GET", "/x", NULL, 0, NULL, 0, 1 << 20, 2000, resp, err, 128);
    pthread_join(th, NULL);
    close(r.fd);
    return rc;
}

int main(void) {
    char err[128];
    os_http_resp r;
    os_http_server *s = os_http_server_start(0, err, sizeof err);
    pthread_t th;
    int port;

    CHECK(s != NULL);
    if (!s) return 1;
    port = os_http_server_port(s);
    pthread_create(&th, NULL, serve, s);

    {   /* request line, query, custom header and body round trip */
        os_hdr h[1] = {{"X-Opensave-Auth", "sig=="}};
        CHECK(os_http_request("127.0.0.1", port, "POST", "/a/b?x=1&y=%20z", h, 1, "{\"k\":1}", 7,
                              1 << 20, 2000, &r, err, sizeof err) == 0);
        CHECK(r.status == 200);
        CHECK_STR(r.body, "POST|/a/b|/a/b?x=1&y=%20z|sig==|{\"k\":1}");
        CHECK(r.server_time == 0); /* this server sends no Date */
        os_http_resp_free(&r);
    }
    {   /* a 404 is a response, not a transport error */
        CHECK(os_http_request("127.0.0.1", port, "GET", "/missing", NULL, 0, NULL, 0, 1 << 20, 2000, &r,
                              err, sizeof err) == 0);
        CHECK(r.status == 404);
        os_http_resp_free(&r);
    }
    {   /* a multi-megabyte body */
        size_t i;
        int ok = 1;
        CHECK(os_http_request("127.0.0.1", port, "GET", "/big", NULL, 0, NULL, 0, 8 << 20, 5000, &r, err,
                              sizeof err) == 0);
        CHECK(r.bodylen == 3u * 1024 * 1024);
        for (i = 0; i < r.bodylen && ok; i++) ok = r.body[i] == (char)('a' + i % 26);
        CHECK(ok);
        os_http_resp_free(&r);
    }
    {   /* the caller's body ceiling is enforced */
        CHECK(os_http_request("127.0.0.1", port, "GET", "/big", NULL, 0, NULL, 0, 1000, 2000, &r, err,
                              sizeof err) == -1);
        CHECK(strstr(err, "too large") != NULL);
    }
    {   /* a request body over the server's limit is never accepted: the server
         * answers 413, or drops the connection before the client has finished
         * sending, which the client sees as a transport error */
        size_t n = OS_HTTP_MAX_REQ_BODY + 10;
        char *big = calloc(1, n);
        int rc;
        memset(big, 'x', n);
        rc = os_http_request("127.0.0.1", port, "POST", "/x", NULL, 0, big, n, 1 << 20, 4000, &r, err,
                             sizeof err);
        CHECK(rc == -1 || r.status == 413);
        if (rc == 0) os_http_resp_free(&r);
        free(big);
    }
    {   /* a client that connects and sends nothing does not wedge the server */
        struct sockaddr_in sa;
        int c = socket(AF_INET, SOCK_STREAM, 0);
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)port);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(connect(c, (struct sockaddr *)&sa, sizeof sa) == 0);
        /* the server gives up on it after its read timeout, then serves us */
        CHECK(os_http_request("127.0.0.1", port, "GET", "/ok", NULL, 0, NULL, 0, 1 << 20, 8000, &r, err,
                              sizeof err) == 0);
        CHECK(r.status == 200);
        os_http_resp_free(&r);
        close(c);
    }

    {   /* a client that drips one byte at a time cannot hold the server past
         * its deadline, however often it resets the per-read timeout */
        struct sockaddr_in sa;
        int c = socket(AF_INET, SOCK_STREAM, 0), i;
        time_t t0 = time(NULL);
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)port);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(connect(c, (struct sockaddr *)&sa, sizeof sa) == 0);
        for (i = 0; i < 30; i++) {
            if (send(c, "G", 1, MSG_NOSIGNAL) != 1) break; /* the server hung up on us */
            usleep(500000);
            if (time(NULL) - t0 > 12) break;
        }
        CHECK(time(NULL) - t0 <= 9); /* dropped at about 5 s, not held for the 15 s we were prepared to drip */
        close(c);
        /* and the server is serving again */
        CHECK(os_http_request("127.0.0.1", port, "GET", "/ok", NULL, 0, NULL, 0, 1 << 20, 8000, &r, err,
                              sizeof err) == 0);
        os_http_resp_free(&r);
    }

    stop_server = 1;
    pthread_join(th, NULL);
    os_http_server_stop(s);

    {   /* chunked, split mid-chunk across segments, with a Date header */
        const char *p[] = {"HTTP/1.1 200 OK\r\nDate: Mon, 02 Jan 2006 15:04:05 GMT\r\n"
                           "Transfer-Encoding: chunked\r\n\r\n5\r\nhel",
                           "lo\r\n6\r\n world\r\n0\r\n\r\n"};
        CHECK(raw_exchange(p, 2, &r, err) == 0);
        CHECK(r.status == 200);
        CHECK_STR(r.body, "hello world");
        CHECK(r.server_time == 1136214245);
        os_http_resp_free(&r);
    }
    {   /* a reply cut short must be an error, never a short body taken as whole */
        const char *p[] = {"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly a little"};
        CHECK(raw_exchange(p, 1, &r, err) == -1);
        CHECK(strstr(err, "mid-reply") != NULL);
    }
    {   /* ...and so must a chunked one that never terminates */
        const char *p[] = {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n"};
        CHECK(raw_exchange(p, 1, &r, err) == -1);
    }
    {   /* no length at all: the body runs to the close */
        const char *p[] = {"HTTP/1.0 200 OK\r\n\r\nuntil close"};
        CHECK(raw_exchange(p, 1, &r, err) == 0);
        CHECK_STR(r.body, "until close");
        os_http_resp_free(&r);
    }
    {   /* garbage is rejected */
        const char *p[] = {"not http at all\r\n\r\n"};
        CHECK(raw_exchange(p, 1, &r, err) == -1);
    }
    {   /* nothing is listening */
        int p2, fd = raw_listen(&p2);
        close(fd);
        CHECK(os_http_request("127.0.0.1", p2, "GET", "/", NULL, 0, NULL, 0, 1000, 1000, &r, err,
                              sizeof err) == -1);
        CHECK(strstr(err, "refused") != NULL);
    }
    CHECK(os_http_request("not-an-ip", 1, "GET", "/", NULL, 0, NULL, 0, 1000, 1000, &r, err, sizeof err) == -1);

    CHECK(os_http_parse_date("Mon, 02 Jan 2006 15:04:05 GMT") == 1136214245);
    CHECK(os_http_parse_date("Thu, 01 Jan 1970 00:00:00 GMT") == 0);
    CHECK(os_http_parse_date("Sat, 29 Feb 2020 12:00:00 GMT") == 1582977600);
    CHECK(os_http_parse_date("garbage") == 0);
    return t_finish("http");
}

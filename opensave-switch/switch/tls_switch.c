/* TLS for the core's HTTPS requests, over the console's own ssl service.
 *
 * The system service does the handshake and checks the server's certificate
 * against the console's built-in authorities, so this app carries no
 * certificates of its own. It is used for one thing: reaching the conversion
 * service. The Switch's date and time must be right for a certificate to be
 * accepted. */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "../core/http.h"
#include "tls_switch.h"

typedef struct {
    SslConnection conn;
    int open;
    int out_fd; /* the descriptor the ssl service gave back; closed before the connection */
} session;

static SslContext g_ctx;
static int g_ready;

/* A failure that says which step failed, since a bare result code is of little help. */
static void fail(char *err, size_t errlen, const char *step, Result rc) {
    snprintf(err, errlen, "the secure connection failed at %s (0x%x). Check that the Switch is online and its date and time are right.",
             step, (unsigned)rc);
}

static void *tls_open(int fd, const char *host, char *err, size_t errlen) {
    session *s = (session *)calloc(1, sizeof *s);
    Result rc;
    u32 out_size = 0, total_certs = 0;

    if (!s) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    s->out_fd = -1;
    rc = sslContextCreateConnection(&g_ctx, &s->conn);
    if (R_FAILED(rc)) {
        fail(err, errlen, "starting the connection", rc);
        free(s);
        return NULL;
    }
    s->open = 1;

    /* The ssl service does not take a C library descriptor: libnx's own wrapper
     * turns it into the socket layer's and hands back a second descriptor, which
     * must be closed before the connection is. (-1 with ENOENT means it returned
     * none, which is fine.) */
    s->out_fd = socketSslConnectionSetSocketDescriptor(&s->conn, fd);
    if (s->out_fd < 0 && errno != ENOENT) {
        fail(err, errlen, "attaching the socket", socketGetLastResult());
        goto bad;
    }
    rc = sslConnectionSetHostName(&s->conn, host, (u32)strlen(host));
    if (R_FAILED(rc)) {
        fail(err, errlen, "setting the host name", rc);
        goto bad;
    }
    /* Check the certificate chain AND that it is for this host name. */
    rc = sslConnectionSetVerifyOption(&s->conn, SslVerifyOption_PeerCa | SslVerifyOption_HostName);
    if (R_FAILED(rc)) {
        fail(err, errlen, "setting certificate checks", rc);
        goto bad;
    }
    rc = sslConnectionSetIoMode(&s->conn, SslIoMode_Blocking);
    if (R_FAILED(rc)) {
        fail(err, errlen, "setting the I/O mode", rc);
        goto bad;
    }
    rc = sslConnectionDoHandshake(&s->conn, &out_size, &total_certs, NULL, 0);
    if (R_FAILED(rc)) {
        fail(err, errlen, "the handshake (the server's certificate may not be trusted by this console)", rc);
        goto bad;
    }
    return s;

bad:
    if (s->out_fd >= 0) close(s->out_fd);
    sslConnectionClose(&s->conn);
    free(s);
    return NULL;
}

static int tls_send(void *t, const void *buf, size_t len) {
    session *s = (session *)t;
    u32 n = 0;
    if (len > 0x10000) len = 0x10000; /* the service takes 32-bit sizes; keep each call modest */
    if (R_FAILED(sslConnectionWrite(&s->conn, buf, (u32)len, &n))) return -1;
    return (int)n;
}

static int tls_recv(void *t, void *buf, size_t len) {
    session *s = (session *)t;
    u32 n = 0;
    if (len > 0x10000) len = 0x10000;
    if (R_FAILED(sslConnectionRead(&s->conn, buf, (u32)len, &n))) return -1;
    return (int)n;
}

static void tls_close(void *t) {
    session *s = (session *)t;
    if (s->out_fd >= 0) close(s->out_fd); /* before the connection, as the service requires */
    if (s->open) sslConnectionClose(&s->conn);
    free(s);
}

int tls_switch_init(void) {
    static const os_tls_hooks hooks = {tls_open, tls_send, tls_recv, tls_close};
    if (g_ready) return 0;
    if (R_FAILED(sslInitialize(2))) return -1;
    if (R_FAILED(sslCreateContext(&g_ctx, SslVersion_Auto))) {
        sslExit();
        return -1;
    }
    os_http_set_tls(&hooks);
    g_ready = 1;
    return 0;
}

void tls_switch_exit(void) {
    if (!g_ready) return;
    sslContextClose(&g_ctx);
    sslExit();
    g_ready = 0;
}

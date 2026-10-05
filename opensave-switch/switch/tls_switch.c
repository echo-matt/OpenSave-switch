/* TLS for the core's HTTPS requests, over the console's own ssl service.
 *
 * The system service does the handshake and checks the server's certificate
 * against the console's built-in authorities, so this app carries no
 * certificates of its own. It is used for one thing: reaching the conversion
 * service. The Switch's date and time must be right for a certificate to be
 * accepted. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "../core/http.h"
#include "tls_switch.h"

typedef struct {
    SslConnection conn;
    int open;
} session;

static SslContext g_ctx;
static int g_ready;

static void *tls_open(int fd, const char *host, char *err, size_t errlen) {
    session *s = (session *)calloc(1, sizeof *s);
    Result rc;
    u32 out_size = 0, total_certs = 0;

    if (!s) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    rc = sslContextCreateConnection(&g_ctx, &s->conn);
    if (R_FAILED(rc)) {
        snprintf(err, errlen, "could not start a secure connection (0x%x)", (unsigned)rc);
        free(s);
        return NULL;
    }
    s->open = 1;
    /* Check the certificate chain AND that it is for this host name. */
    rc = sslConnectionSetSocketDescriptor(&s->conn, fd);
    if (R_SUCCEEDED(rc)) rc = sslConnectionSetHostName(&s->conn, host, (u32)strlen(host));
    if (R_SUCCEEDED(rc)) rc = sslConnectionSetVerifyOption(&s->conn, SslVerifyOption_PeerCa | SslVerifyOption_HostName);
    if (R_SUCCEEDED(rc)) rc = sslConnectionSetIoMode(&s->conn, SslIoMode_Blocking);
    if (R_SUCCEEDED(rc)) rc = sslConnectionDoHandshake(&s->conn, &out_size, &total_certs, NULL, 0);
    if (R_FAILED(rc)) {
        snprintf(err, errlen,
                 "the secure connection failed (0x%x). Check the Switch's date and time, and that it is online.",
                 (unsigned)rc);
        sslConnectionClose(&s->conn);
        free(s);
        return NULL;
    }
    return s;
}

static int tls_send(void *t, const void *buf, size_t len) {
    session *s = (session *)t;
    s32 n = 0;
    if (R_FAILED(sslConnectionWrite(&s->conn, buf, len, &n))) return -1;
    return n;
}

static int tls_recv(void *t, void *buf, size_t len) {
    session *s = (session *)t;
    s32 n = 0;
    if (R_FAILED(sslConnectionRead(&s->conn, buf, len, &n))) return -1;
    return n;
}

static void tls_close(void *t) {
    session *s = (session *)t;
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

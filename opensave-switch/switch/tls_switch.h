#ifndef OPENSAVE_TLS_SWITCH_H
#define OPENSAVE_TLS_SWITCH_H

/* Registers the console's TLS with the HTTP client. Returns 0 on success; on
 * failure https requests simply report that there is no TLS. */
int tls_switch_init(void);
void tls_switch_exit(void);

#endif

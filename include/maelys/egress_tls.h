#ifndef MAELYS_EGRESS_TLS_H
#define MAELYS_EGRESS_TLS_H

#include <stddef.h>
#include <stdint.h>

#include "maelys/egress.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAELYS_EGRESS_TLS_ABI_VERSION 1u

typedef struct maelys_egress_tls_provider maelys_egress_tls_provider_t;

typedef enum maelys_egress_tls_role {
    MAELYS_EGRESS_TLS_CLIENT = 1,
    MAELYS_EGRESS_TLS_SERVER = 2
} maelys_egress_tls_role_t;

typedef enum maelys_egress_tls_step {
    /* The requested operation completed, or read/write transferred bytes. */
    MAELYS_EGRESS_TLS_COMPLETE = 0,
    MAELYS_EGRESS_TLS_WANT_READ,
    MAELYS_EGRESS_TLS_WANT_WRITE,
    /* A clean TLS close was observed while reading or writing. */
    MAELYS_EGRESS_TLS_CLOSED,
    MAELYS_EGRESS_TLS_FAILED
} maelys_egress_tls_step_t;

/*
 * Backend-neutral nonblocking TLS seam. The provider borrows fd for the
 * session lifetime and never closes it. Buffers are borrowed for one call.
 * Implementations must not perform hidden blocking I/O. COMPLETE from read or
 * write requires a nonzero transfer for a nonempty request; providers must
 * return WANT_READ/WANT_WRITE instead of busy-looping. last_error returns a
 * nonempty borrowed, provider-owned diagnostic after FAILED.
 *
 * session_create receives a nonblocking stream socket. server_name is the
 * verification/SNI name for a client session and may be NULL for a server
 * session. It sets *out_session only on success. A session is owner-thread
 * confined; a provider shared by multiple servers must support independent
 * sessions concurrently. session_release is local-only: no I/O, wait or fd
 * close. The name and ops table are copied during provider_create; context is
 * retained until the provider's last release.
 */
typedef struct maelys_egress_tls_ops {
    unsigned int abi_version;
    const char *name;
    maelys_egress_result_t (*session_create)(
        void *context,
        maelys_egress_tls_role_t role,
        int fd,
        const char *server_name,
        void **out_session,
        char **out_error);
    maelys_egress_tls_step_t (*handshake)(void *context, void *session);
    maelys_egress_tls_step_t (*read)(
        void *context,
        void *session,
        void *buffer,
        size_t capacity,
        size_t *out_read);
    maelys_egress_tls_step_t (*write)(
        void *context,
        void *session,
        const void *buffer,
        size_t length,
        size_t *out_written);
    maelys_egress_tls_step_t (*shutdown)(void *context, void *session);
    const char *(*last_error)(void *context, const void *session);
    void (*session_release)(void *context, void *session);
} maelys_egress_tls_ops_t;

maelys_egress_result_t maelys_egress_tls_provider_create(
    const maelys_egress_tls_ops_t *ops,
    void *context,
    void (*release_context)(void *context),
    maelys_egress_tls_provider_t **out_provider,
    char **out_error);
const char *maelys_egress_tls_provider_name(
    const maelys_egress_tls_provider_t *provider);
void maelys_egress_tls_provider_retain(maelys_egress_tls_provider_t *provider);
void maelys_egress_tls_provider_release(maelys_egress_tls_provider_t *provider);

#ifdef __cplusplus
}
#endif

#endif

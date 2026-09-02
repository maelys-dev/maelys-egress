#include "maelys/egress_tls_modules.h"

#include <limits.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

typedef struct wolfssl_provider_context {
    WOLFSSL_CTX *server_context;
    WOLFSSL_CTX *client_context;
} wolfssl_provider_context_t;

typedef struct wolfssl_provider_session {
    WOLFSSL *ssl;
    int fd;
    char error[192];
} wolfssl_provider_session_t;

static int socket_receive(WOLFSSL *ssl, char *buffer, int length, void *opaque) {
    (void)ssl;
    wolfssl_provider_session_t *session = opaque;
    ssize_t received = recv(session->fd, buffer, (size_t)length, 0);
    if (received > 0) return (int)received;
    if (received == 0) return WOLFSSL_CBIO_ERR_CONN_CLOSE;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        return WOLFSSL_CBIO_ERR_WANT_READ;
    }
    return WOLFSSL_CBIO_ERR_GENERAL;
}

static int socket_send(WOLFSSL *ssl, char *buffer, int length, void *opaque) {
    (void)ssl;
    wolfssl_provider_session_t *session = opaque;
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    ssize_t sent = send(session->fd, buffer, (size_t)length, flags);
    if (sent >= 0) return (int)sent;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        return WOLFSSL_CBIO_ERR_WANT_WRITE;
    }
    if (errno == EPIPE || errno == ECONNRESET) return WOLFSSL_CBIO_ERR_CONN_CLOSE;
    return WOLFSSL_CBIO_ERR_GENERAL;
}

static pthread_once_t wolfssl_once = PTHREAD_ONCE_INIT;
static int wolfssl_initialized;
static void initialize_wolfssl(void) {
    wolfssl_initialized = wolfSSL_Init() == WOLFSSL_SUCCESS;
}

static void set_error(char **out_error, const char *message) {
    if (!out_error) return;
    size_t length = strlen(message) + 1u;
    *out_error = malloc(length);
    if (*out_error) memcpy(*out_error, message, length);
}

static void remember_error(wolfssl_provider_session_t *session, int result) {
    int code = wolfSSL_get_error(session->ssl, result);
    (void)snprintf(session->error, sizeof(session->error),
                   "wolfSSL error %d", code);
}

static maelys_egress_tls_step_t translate(
    wolfssl_provider_session_t *session, int result, int zero_is_closed) {
    if (result > 0) return MAELYS_EGRESS_TLS_COMPLETE;
    int code = wolfSSL_get_error(session->ssl, result);
    if (code == WOLFSSL_ERROR_WANT_READ) return MAELYS_EGRESS_TLS_WANT_READ;
    if (code == WOLFSSL_ERROR_WANT_WRITE) return MAELYS_EGRESS_TLS_WANT_WRITE;
    if ((zero_is_closed && result == 0) || code == WOLFSSL_ERROR_ZERO_RETURN) {
        return MAELYS_EGRESS_TLS_CLOSED;
    }
    remember_error(session, result);
    return MAELYS_EGRESS_TLS_FAILED;
}

static maelys_egress_result_t session_create(
    void *opaque, maelys_egress_tls_role_t role, int fd,
    const char *server_name, void **out_session, char **out_error) {
    if (out_error) *out_error = NULL;
    if (out_session) *out_session = NULL;
    wolfssl_provider_context_t *context = opaque;
    WOLFSSL_CTX *selected = role == MAELYS_EGRESS_TLS_SERVER ?
        (context ? context->server_context : NULL) :
        (context ? context->client_context : NULL);
    if (!context || !out_session || fd < 0 || !selected ||
        (role == MAELYS_EGRESS_TLS_CLIENT && (!server_name || !server_name[0])) ||
        (role != MAELYS_EGRESS_TLS_SERVER && role != MAELYS_EGRESS_TLS_CLIENT)) {
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    wolfssl_provider_session_t *session = calloc(1, sizeof(*session));
    if (!session) return MAELYS_EGRESS_ERR_MEMORY;
    session->ssl = wolfSSL_new(selected);
    session->fd = fd;
    if (!session->ssl ||
        (role == MAELYS_EGRESS_TLS_CLIENT &&
         wolfSSL_check_domain_name(session->ssl, server_name) != WOLFSSL_SUCCESS)) {
        set_error(out_error, "wolfSSL session setup failed");
        if (session->ssl) wolfSSL_free(session->ssl);
        free(session);
        return MAELYS_EGRESS_ERR_CRYPTO;
    }
    wolfSSL_SSLSetIORecv(session->ssl, socket_receive);
    wolfSSL_SSLSetIOSend(session->ssl, socket_send);
    wolfSSL_SetIOReadCtx(session->ssl, session);
    wolfSSL_SetIOWriteCtx(session->ssl, session);
    *out_session = session;
    return MAELYS_EGRESS_OK;
}

static maelys_egress_tls_step_t handshake(void *context, void *opaque) {
    (void)context;
    wolfssl_provider_session_t *session = opaque;
    return translate(session, wolfSSL_negotiate(session->ssl), 0);
}

static maelys_egress_tls_step_t tls_read(
    void *context, void *opaque, void *buffer, size_t capacity, size_t *out_read) {
    (void)context;
    wolfssl_provider_session_t *session = opaque;
    if (out_read) *out_read = 0u;
    if (!session || !buffer || !capacity || !out_read) return MAELYS_EGRESS_TLS_FAILED;
    int bounded = capacity > (size_t)INT_MAX ? INT_MAX : (int)capacity;
    int result = wolfSSL_read(session->ssl, buffer, bounded);
    if (result > 0) *out_read = (size_t)result;
    return translate(session, result, 1);
}

static maelys_egress_tls_step_t tls_write(
    void *context, void *opaque, const void *buffer, size_t length,
    size_t *out_written) {
    (void)context;
    wolfssl_provider_session_t *session = opaque;
    if (out_written) *out_written = 0u;
    if (!session || !buffer || !length || !out_written) return MAELYS_EGRESS_TLS_FAILED;
    int bounded = length > (size_t)INT_MAX ? INT_MAX : (int)length;
    int result = wolfSSL_write(session->ssl, buffer, bounded);
    if (result > 0) *out_written = (size_t)result;
    return translate(session, result, 0);
}

static maelys_egress_tls_step_t tls_shutdown(void *context, void *opaque) {
    (void)context;
    wolfssl_provider_session_t *session = opaque;
    int result = wolfSSL_shutdown(session->ssl);
    if (result == WOLFSSL_SUCCESS) return MAELYS_EGRESS_TLS_COMPLETE;
    if (result == WOLFSSL_SHUTDOWN_NOT_DONE) return MAELYS_EGRESS_TLS_WANT_READ;
    return translate(session, result, 0);
}

static const char *last_error(void *context, const void *opaque) {
    (void)context;
    const wolfssl_provider_session_t *session = opaque;
    return session && session->error[0] ? session->error : "wolfSSL operation failed";
}

static void session_release(void *context, void *opaque) {
    (void)context;
    wolfssl_provider_session_t *session = opaque;
    if (!session) return;
    wolfSSL_free(session->ssl);
    free(session);
}

static void context_release(void *opaque) {
    wolfssl_provider_context_t *context = opaque;
    if (!context) return;
    if (context->server_context) wolfSSL_CTX_free(context->server_context);
    if (context->client_context) wolfSSL_CTX_free(context->client_context);
    free(context);
}

static int configure_identity(
    WOLFSSL_CTX *context, const maelys_egress_tls_files_t *files) {
    return wolfSSL_CTX_use_certificate_file(context, files->certificate_file,
               WOLFSSL_FILETYPE_PEM) == WOLFSSL_SUCCESS &&
           wolfSSL_CTX_use_PrivateKey_file(context, files->private_key_file,
               WOLFSSL_FILETYPE_PEM) == WOLFSSL_SUCCESS;
}

maelys_egress_result_t maelys_egress_tls_wolfssl_create(
    const maelys_egress_tls_files_t *files,
    maelys_egress_tls_provider_t **out_provider,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (out_provider) *out_provider = NULL;
    if (!files || !out_provider ||
        (!!files->certificate_file != !!files->private_key_file) ||
        (!files->certificate_file && !files->ca_file)) {
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    (void)pthread_once(&wolfssl_once, initialize_wolfssl);
    if (!wolfssl_initialized) return MAELYS_EGRESS_ERR_CRYPTO;
    wolfssl_provider_context_t *context = calloc(1, sizeof(*context));
    if (!context) return MAELYS_EGRESS_ERR_MEMORY;
    int ok = 1;
    if (files->certificate_file) {
        context->server_context = wolfSSL_CTX_new(wolfSSLv23_server_method());
        ok = context->server_context && configure_identity(context->server_context, files);
        if (ok && files->require_client_certificate) {
            ok = files->ca_file &&
                wolfSSL_CTX_load_verify_locations(context->server_context,
                    files->ca_file, NULL) == WOLFSSL_SUCCESS;
            if (ok) wolfSSL_CTX_set_verify(context->server_context,
                WOLFSSL_VERIFY_PEER | WOLFSSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
        } else if (ok) {
            wolfSSL_CTX_set_verify(context->server_context, WOLFSSL_VERIFY_NONE, NULL);
        }
    }
    if (ok && files->ca_file) {
        context->client_context = wolfSSL_CTX_new(wolfSSLv23_client_method());
        ok = context->client_context &&
            wolfSSL_CTX_load_verify_locations(context->client_context,
                files->ca_file, NULL) == WOLFSSL_SUCCESS;
        if (ok) wolfSSL_CTX_set_verify(
            context->client_context, WOLFSSL_VERIFY_PEER, NULL);
        if (ok && files->certificate_file) {
            ok = configure_identity(context->client_context, files);
        }
    }
    if (!ok) {
        set_error(out_error, "wolfSSL provider setup failed");
        context_release(context);
        return MAELYS_EGRESS_ERR_CRYPTO;
    }
    const maelys_egress_tls_ops_t ops = {
        .abi_version = MAELYS_EGRESS_TLS_ABI_VERSION,
        .name = "wolfssl",
        .session_create = session_create,
        .handshake = handshake,
        .read = tls_read,
        .write = tls_write,
        .shutdown = tls_shutdown,
        .last_error = last_error,
        .session_release = session_release
    };
    maelys_egress_result_t created = maelys_egress_tls_provider_create(
        &ops, context, context_release, out_provider, out_error);
    if (created != MAELYS_EGRESS_OK) context_release(context);
    return created;
}

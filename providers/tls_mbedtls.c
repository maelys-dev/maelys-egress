#include "maelys/egress_tls_modules.h"

#include <errno.h>
#include <limits.h>
#include <mbedtls/version.h>
#if MBEDTLS_VERSION_MAJOR < 4
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#else
#include <psa/crypto.h>
#endif
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

typedef struct mbedtls_provider_context {
    mbedtls_ssl_config server_config;
    mbedtls_ssl_config client_config;
    mbedtls_x509_crt certificate;
    mbedtls_x509_crt authorities;
    mbedtls_pk_context private_key;
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context random;
#endif
    int server_ready;
    int client_ready;
} mbedtls_provider_context_t;

typedef struct mbedtls_provider_session {
    mbedtls_ssl_context ssl;
    int fd;
    char error[192];
} mbedtls_provider_session_t;

#if MBEDTLS_VERSION_MAJOR >= 4
static pthread_once_t psa_once = PTHREAD_ONCE_INIT;
static psa_status_t psa_status = PSA_ERROR_GENERIC_ERROR;
static void initialize_psa(void) { psa_status = psa_crypto_init(); }
#endif

static void set_creation_error(char **out_error, const char *prefix, int code) {
    if (!out_error) return;
    char detail[128];
    mbedtls_strerror(code, detail, sizeof(detail));
    size_t length = strlen(prefix) + strlen(detail) + 3u;
    char *message = malloc(length);
    if (message) (void)snprintf(message, length, "%s: %s", prefix, detail);
    *out_error = message;
}

static int socket_send(void *context, const unsigned char *buffer, size_t length) {
    mbedtls_provider_session_t *session = context;
    size_t bounded = length > (size_t)INT_MAX ? (size_t)INT_MAX : length;
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    ssize_t sent = send(session->fd, buffer, bounded, flags);
    if (sent >= 0) return (int)sent;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int socket_receive(void *context, unsigned char *buffer, size_t length) {
    mbedtls_provider_session_t *session = context;
    size_t bounded = length > (size_t)INT_MAX ? (size_t)INT_MAX : length;
    ssize_t received = recv(session->fd, buffer, bounded, 0);
    if (received >= 0) return (int)received;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

static void remember_error(mbedtls_provider_session_t *session, int code) {
    mbedtls_strerror(code, session->error, sizeof(session->error));
    if (!session->error[0]) {
        (void)snprintf(session->error, sizeof(session->error),
                       "Mbed TLS error %d", code);
    }
}

static maelys_egress_tls_step_t translate(
    mbedtls_provider_session_t *session, int result, int zero_is_closed) {
    if (result > 0 || (result == 0 && !zero_is_closed)) {
        return MAELYS_EGRESS_TLS_COMPLETE;
    }
    if (result == MBEDTLS_ERR_SSL_WANT_READ) return MAELYS_EGRESS_TLS_WANT_READ;
    if (result == MBEDTLS_ERR_SSL_WANT_WRITE) return MAELYS_EGRESS_TLS_WANT_WRITE;
    if ((zero_is_closed && result == 0) ||
        result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
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
    mbedtls_provider_context_t *context = opaque;
    if (!context || !out_session || fd < 0 ||
        (role == MAELYS_EGRESS_TLS_SERVER && !context->server_ready) ||
        (role == MAELYS_EGRESS_TLS_CLIENT &&
         (!context->client_ready || !server_name || !server_name[0])) ||
        (role != MAELYS_EGRESS_TLS_SERVER && role != MAELYS_EGRESS_TLS_CLIENT)) {
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    mbedtls_provider_session_t *session = calloc(1, sizeof(*session));
    if (!session) return MAELYS_EGRESS_ERR_MEMORY;
    session->fd = fd;
    mbedtls_ssl_init(&session->ssl);
    int result = mbedtls_ssl_setup(&session->ssl,
        role == MAELYS_EGRESS_TLS_SERVER ? &context->server_config :
                                         &context->client_config);
    if (result == 0 && role == MAELYS_EGRESS_TLS_CLIENT) {
        result = mbedtls_ssl_set_hostname(&session->ssl, server_name);
    }
    if (result != 0) {
        set_creation_error(out_error, "Mbed TLS session setup failed", result);
        mbedtls_ssl_free(&session->ssl);
        free(session);
        return MAELYS_EGRESS_ERR_CRYPTO;
    }
    mbedtls_ssl_set_bio(&session->ssl, session, socket_send, socket_receive, NULL);
    *out_session = session;
    return MAELYS_EGRESS_OK;
}

static maelys_egress_tls_step_t handshake(void *context, void *opaque) {
    (void)context;
    mbedtls_provider_session_t *session = opaque;
    return translate(session, mbedtls_ssl_handshake(&session->ssl), 0);
}

static maelys_egress_tls_step_t tls_read(
    void *context, void *opaque, void *buffer, size_t capacity, size_t *out_read) {
    (void)context;
    mbedtls_provider_session_t *session = opaque;
    if (out_read) *out_read = 0u;
    if (!session || !buffer || !capacity || !out_read) return MAELYS_EGRESS_TLS_FAILED;
    int result = mbedtls_ssl_read(&session->ssl, buffer, capacity);
    if (result > 0) *out_read = (size_t)result;
    return translate(session, result, 1);
}

static maelys_egress_tls_step_t tls_write(
    void *context, void *opaque, const void *buffer, size_t length,
    size_t *out_written) {
    (void)context;
    mbedtls_provider_session_t *session = opaque;
    if (out_written) *out_written = 0u;
    if (!session || !buffer || !length || !out_written) return MAELYS_EGRESS_TLS_FAILED;
    int result = mbedtls_ssl_write(&session->ssl, buffer, length);
    if (result > 0) *out_written = (size_t)result;
    return translate(session, result, 0);
}

static maelys_egress_tls_step_t tls_shutdown(void *context, void *opaque) {
    (void)context;
    mbedtls_provider_session_t *session = opaque;
    return translate(session, mbedtls_ssl_close_notify(&session->ssl), 0);
}

static const char *last_error(void *context, const void *opaque) {
    (void)context;
    const mbedtls_provider_session_t *session = opaque;
    return session && session->error[0] ? session->error : "Mbed TLS operation failed";
}

static void session_release(void *context, void *opaque) {
    (void)context;
    mbedtls_provider_session_t *session = opaque;
    if (!session) return;
    mbedtls_ssl_free(&session->ssl);
    free(session);
}

static void context_release(void *opaque) {
    mbedtls_provider_context_t *context = opaque;
    if (!context) return;
    mbedtls_ssl_config_free(&context->server_config);
    mbedtls_ssl_config_free(&context->client_config);
    mbedtls_x509_crt_free(&context->certificate);
    mbedtls_x509_crt_free(&context->authorities);
    mbedtls_pk_free(&context->private_key);
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_ctr_drbg_free(&context->random);
    mbedtls_entropy_free(&context->entropy);
#endif
    free(context);
}

maelys_egress_result_t maelys_egress_tls_mbedtls_create(
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
#if MBEDTLS_VERSION_MAJOR >= 4
    (void)pthread_once(&psa_once, initialize_psa);
    if (psa_status != PSA_SUCCESS) {
        return MAELYS_EGRESS_ERR_CRYPTO;
    }
#endif
    mbedtls_provider_context_t *context = calloc(1, sizeof(*context));
    if (!context) return MAELYS_EGRESS_ERR_MEMORY;
    mbedtls_ssl_config_init(&context->server_config);
    mbedtls_ssl_config_init(&context->client_config);
    mbedtls_x509_crt_init(&context->certificate);
    mbedtls_x509_crt_init(&context->authorities);
    mbedtls_pk_init(&context->private_key);
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_entropy_init(&context->entropy);
    mbedtls_ctr_drbg_init(&context->random);
    const unsigned char personalization[] = "maelys-egress";
    int result = mbedtls_ctr_drbg_seed(&context->random, mbedtls_entropy_func,
        &context->entropy, personalization, sizeof(personalization) - 1u);
#else
    int result = 0;
#endif
    if (result == 0 && files->certificate_file) {
        result = mbedtls_x509_crt_parse_file(
            &context->certificate, files->certificate_file);
    }
    if (result == 0 && files->private_key_file) {
#if MBEDTLS_VERSION_MAJOR == 3
        result = mbedtls_pk_parse_keyfile(
            &context->private_key, files->private_key_file, NULL,
            mbedtls_ctr_drbg_random, &context->random);
#else
        result = mbedtls_pk_parse_keyfile(
            &context->private_key, files->private_key_file, NULL);
#endif
    }
    if (result == 0 && files->ca_file) {
        result = mbedtls_x509_crt_parse_file(&context->authorities, files->ca_file);
    }
    if (result == 0 && files->certificate_file) {
        result = mbedtls_ssl_config_defaults(&context->server_config,
            MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
            MBEDTLS_SSL_PRESET_DEFAULT);
        if (result == 0) {
#if MBEDTLS_VERSION_MAJOR < 4
            mbedtls_ssl_conf_rng(&context->server_config, mbedtls_ctr_drbg_random,
                                 &context->random);
#endif
            result = mbedtls_ssl_conf_own_cert(&context->server_config,
                &context->certificate, &context->private_key);
            if (result == 0 && files->require_client_certificate) {
                if (!files->ca_file) result = MBEDTLS_ERR_X509_BAD_INPUT_DATA;
                else {
                    mbedtls_ssl_conf_ca_chain(&context->server_config,
                                              &context->authorities, NULL);
                    mbedtls_ssl_conf_authmode(&context->server_config,
                                              MBEDTLS_SSL_VERIFY_REQUIRED);
                }
            } else if (result == 0) {
                mbedtls_ssl_conf_authmode(&context->server_config,
                                          MBEDTLS_SSL_VERIFY_NONE);
            }
        }
        if (result == 0) context->server_ready = 1;
    }
    if (result == 0 && files->ca_file) {
        result = mbedtls_ssl_config_defaults(&context->client_config,
            MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
            MBEDTLS_SSL_PRESET_DEFAULT);
        if (result == 0) {
#if MBEDTLS_VERSION_MAJOR < 4
            mbedtls_ssl_conf_rng(&context->client_config, mbedtls_ctr_drbg_random,
                                 &context->random);
#endif
            mbedtls_ssl_conf_ca_chain(&context->client_config,
                                      &context->authorities, NULL);
            mbedtls_ssl_conf_authmode(&context->client_config,
                                      MBEDTLS_SSL_VERIFY_REQUIRED);
            if (result == 0 && files->certificate_file) {
                result = mbedtls_ssl_conf_own_cert(&context->client_config,
                    &context->certificate, &context->private_key);
            }
        }
        if (result == 0) context->client_ready = 1;
    }
    if (result != 0) {
        set_creation_error(out_error, "Mbed TLS provider setup failed", result);
        context_release(context);
        return MAELYS_EGRESS_ERR_CRYPTO;
    }
    const maelys_egress_tls_ops_t ops = {
        .abi_version = MAELYS_EGRESS_TLS_ABI_VERSION,
        .name = "mbedtls",
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

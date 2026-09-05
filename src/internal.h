#ifndef MAELYS_EGRESS_INTERNAL_H
#define MAELYS_EGRESS_INTERNAL_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "maelys/egress.h"
#include "maelys/egress_tls.h"
#include "maelys/sys.h"

#define EGRESS_MAX_DESTINATIONS 256u
#define EGRESS_MAX_PINNED_ADDRESSES 16u
#define EGRESS_MAX_HOST 253u
#define EGRESS_MAX_USERNAME 63u
#define EGRESS_MAX_SECRET 255u
#define EGRESS_MAX_INVOCATION_ID 127u
#define EGRESS_MAX_PRINCIPALS 64u
#define EGRESS_MAX_ATTESTATION_BYTES 512u
#define EGRESS_HANDSHAKE_MAX 16384u
#define EGRESS_DEFAULT_MAX_CONNECTIONS 128u
#define EGRESS_DEFAULT_BUFFER_BYTES 32768u
#define EGRESS_MIN_BUFFER_BYTES (EGRESS_HANDSHAKE_MAX + 64u)
#define EGRESS_MAX_BUFFER_BYTES (1024u * 1024u)

typedef struct egress_address {
    struct sockaddr_storage storage;
    socklen_t length;
} egress_address_t;

typedef struct egress_destination {
    char *host;
    uint16_t port;
    int allow_private;
    int require_tls_sni;
    egress_address_t addresses[EGRESS_MAX_PINNED_ADDRESSES];
    size_t address_count;
} egress_destination_t;

typedef struct egress_proxy_request {
    maelys_egress_protocol_t protocol;
    char host[EGRESS_MAX_HOST + 1u];
    uint16_t port;
    size_t consumed;
    unsigned char *forward_bytes;
    size_t forward_length;
    char invocation_id[EGRESS_MAX_INVOCATION_ID + 1u];
    size_t principal_index;
} egress_proxy_request_t;

typedef struct egress_principal {
    char username[EGRESS_MAX_USERNAME + 1u];
    char secret[EGRESS_MAX_SECRET + 1u];
    char invocation_id[EGRESS_MAX_INVOCATION_ID + 1u];
    size_t max_active_connections;
    uint64_t max_bytes_per_connection;
    uint64_t max_bytes_total;
} egress_principal_t;

struct maelys_egress_policy {
    egress_destination_t *destinations;
    size_t count;
    size_t capacity;
    int sealed;
    char digest_hex[65];
};

struct maelys_egress_config {
    int native_only;
    int listen_unix;
    char listen_host[64];
    uint16_t port;
    char unix_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    maelys_egress_unix_peer_policy_t unix_peer_policy;
    egress_principal_t principals[EGRESS_MAX_PRINCIPALS];
    size_t principal_count;
    int authentication_set;
    int unauthenticated_loopback;
    int unix_principal_bound;
    size_t unix_principal_index;
    size_t max_connections;
    size_t buffer_bytes;
    uint64_t handshake_timeout_ms;
    uint64_t idle_timeout_ms;
    maelys_egress_receipt_sink_fn receipt_sink;
    void *receipt_context;
    maelys_egress_tls_provider_t *tls_provider;
    int admin_enabled;
    char admin_host[64];
    uint16_t admin_port;
    maelys_egress_audit_t *audit;
    maelys_egress_attestor_t *attestor;
};

struct maelys_egress_receipt {
    uint64_t id;
    maelys_egress_protocol_t protocol;
    char host[EGRESS_MAX_HOST + 1u];
    uint16_t port;
    maelys_egress_result_t result;
    uint64_t started_unix_ms;
    uint64_t duration_ms;
    uint64_t bytes_from_client;
    uint64_t bytes_to_client;
    maelys_egress_quota_scope_t quota_scope;
    uint64_t quota_connection_max_bytes;
    uint64_t quota_execution_max_bytes;
    uint64_t quota_connection_observed_bytes;
    uint64_t quota_execution_before_bytes;
    uint64_t quota_execution_after_bytes;
    char policy_digest_hex[65];
    char invocation_id[EGRESS_MAX_INVOCATION_ID + 1u];
    int tls_sni_verified;
    char principal[EGRESS_MAX_USERNAME + 1u];
    uint64_t policy_generation;
    char attestor[64];
    char attestation_key_id[128];
    char attestation_hex[EGRESS_MAX_ATTESTATION_BYTES * 2u + 1u];
};

struct maelys_egress_metrics {
    uint64_t accepted;
    uint64_t active;
    uint64_t admitted;
    uint64_t denied;
    uint64_t auth_failures;
    uint64_t quota_denials;
    uint64_t bytes_from_clients;
    uint64_t bytes_to_clients;
    uint64_t receipts;
    uint64_t policy_generation;
    uint64_t reload_failures;
    uint64_t audit_failures;
};

struct maelys_egress_audit {
    atomic_uint references;
    pthread_mutex_t lock;
    /* The journal is held through System's identity-checked lock; fd is
     * the borrowed descriptor of that lock, -1 once released. */
    maelys_sys_file_lock_t *journal_lock;
    int fd;
    unsigned char *key;
    size_t key_length;
    char key_id[64];
    char chain_hex[65];
    atomic_uint_fast64_t records;
    atomic_int healthy;
};

struct maelys_egress_attestor {
    atomic_uint references;
    char name[64];
    char key_id[128];
    size_t max_signature_bytes;
    maelys_egress_attest_fn attest;
    void *context;
    void (*release_context)(void *context);
};

struct maelys_egress_tls_provider {
    atomic_uint references;
    maelys_egress_tls_ops_t ops;
    char *name;
    void *context;
    void (*release_context)(void *context);
};

#if defined(__GNUC__) || defined(__clang__)
void egress_set_error(char **out_error, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
#else
void egress_set_error(char **out_error, const char *format, ...);
#endif
char *egress_strdup(const char *value);
void egress_secure_zero(void *data, size_t length);
int egress_canonical_host(const char *input, char output[EGRESS_MAX_HOST + 1u]);
int egress_constant_time_equal(const char *left, const char *right);
int egress_address_is_private(const struct sockaddr *address);
int egress_unix_parent_is_private(const char *path, char **out_error);
/* 0=incomplete, 1=complete, -1=protocol, -2=authentication. */
int egress_parse_http_request(
    const unsigned char *bytes,
    size_t length,
    const maelys_egress_config_t *config,
    egress_proxy_request_t *out_request,
    char **out_error);
/* 0=incomplete, 1=protocol frame consumed, 2=request complete, negative=failure. */
int egress_parse_socks_frame(
    const unsigned char *bytes,
    size_t length,
    const maelys_egress_config_t *config,
    int *phase,
    size_t *out_consumed,
    unsigned char out_response[10],
    size_t *out_response_length,
    egress_proxy_request_t *out_request,
    char authenticated_invocation_id[EGRESS_MAX_INVOCATION_ID + 1u],
    size_t *authenticated_principal_index);
int egress_credentials_match(
    const maelys_egress_config_t *config,
    const char *username,
    const char *secret);
int egress_credentials_lookup(
    const maelys_egress_config_t *config,
    const char *username,
    const char *secret,
    char out_invocation_id[EGRESS_MAX_INVOCATION_ID + 1u],
    size_t *out_principal_index);
int egress_unix_principal_lookup(
    const maelys_egress_config_t *config,
    char out_invocation_id[EGRESS_MAX_INVOCATION_ID + 1u],
    size_t *out_principal_index);
void egress_proxy_request_clear(egress_proxy_request_t *request);
void egress_policy_compute_digest(maelys_egress_policy_t *policy);
const egress_destination_t *egress_policy_find(
    const maelys_egress_policy_t *policy,
    const char *host,
    uint16_t port);
void egress_sha256_hex(const void *data, size_t length, char out_hex[65]);
void egress_sha256(const void *data, size_t length, unsigned char out_digest[32]);
int egress_hmac_sha256(
    const unsigned char *key, size_t key_length,
    const void *data, size_t data_length,
    unsigned char output[32]);
int egress_audit_append(maelys_egress_audit_t *audit,
                      const maelys_egress_receipt_t *receipt);
/* Canonical receipt bytes shared by attestation and the audit journal.
 * Returns the length written or -1 when the buffer is too small. */
int egress_receipt_canonical(
    const maelys_egress_receipt_t *receipt, char *buffer, size_t capacity);
/* 0=incomplete, 1=exact match, -1=malformed/unsupported/mismatch. */
int egress_tls_client_hello_matches(
    const unsigned char *bytes,
    size_t length,
    const char *expected_host,
    char **out_error);

maelys_egress_result_t egress_server_connector_bind(
    maelys_egress_server_t *server,
    const char *username,
    const char *secret,
    size_t *out_principal_index,
    char out_principal[EGRESS_MAX_USERNAME + 1u],
    char out_invocation_id[EGRESS_MAX_INVOCATION_ID + 1u],
    char **out_error);
void egress_server_connector_release(maelys_egress_server_t *server);
maelys_egress_result_t egress_server_open_stream(
    maelys_egress_server_t *server,
    size_t principal_index,
    const char *principal,
    const char *invocation_id,
    const char *canonical_host,
    uint16_t port,
    uint64_t timeout_ms,
    int *out_fd,
    char **out_error);

#endif

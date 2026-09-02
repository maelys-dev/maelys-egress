#ifndef MAELYS_EGRESS_H
#define MAELYS_EGRESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAELYS_EGRESS_ABI_VERSION 2u

typedef enum maelys_egress_result {
    MAELYS_EGRESS_OK = 0,
    MAELYS_EGRESS_ERR_ARGUMENT,
    MAELYS_EGRESS_ERR_MEMORY,
    MAELYS_EGRESS_ERR_STATE,
    MAELYS_EGRESS_ERR_IO,
    MAELYS_EGRESS_ERR_DENIED,
    MAELYS_EGRESS_ERR_PROTOCOL,
    MAELYS_EGRESS_ERR_TIMEOUT,
    MAELYS_EGRESS_ERR_CANCELLED,
    MAELYS_EGRESS_ERR_UNSUPPORTED,
    MAELYS_EGRESS_ERR_CRYPTO
} maelys_egress_result_t;

typedef enum maelys_egress_protocol {
    MAELYS_EGRESS_PROTOCOL_HTTP_CONNECT = 1,
    MAELYS_EGRESS_PROTOCOL_HTTP_FORWARD = 2,
    MAELYS_EGRESS_PROTOCOL_SOCKS5 = 3,
    /* Native embedding session; no HTTP/SOCKS bytes cross the client stream. */
    MAELYS_EGRESS_PROTOCOL_CONNECTOR = 4
} maelys_egress_protocol_t;

typedef enum maelys_egress_quota_scope {
    MAELYS_EGRESS_QUOTA_NONE = 0,
    MAELYS_EGRESS_QUOTA_ACTIVE_CONNECTIONS = 1,
    MAELYS_EGRESS_QUOTA_CONNECTION_BYTES = 2,
    MAELYS_EGRESS_QUOTA_EXECUTION_BYTES = 3
} maelys_egress_quota_scope_t;

typedef struct maelys_egress_policy maelys_egress_policy_t;
typedef struct maelys_egress_config maelys_egress_config_t;
typedef struct maelys_egress_server maelys_egress_server_t;
typedef struct maelys_egress_receipt maelys_egress_receipt_t;
typedef struct maelys_egress_tls_provider maelys_egress_tls_provider_t;
typedef struct maelys_egress_metrics maelys_egress_metrics_t;
typedef struct maelys_egress_audit maelys_egress_audit_t;
typedef struct maelys_egress_attestor maelys_egress_attestor_t;
typedef struct maelys_egress_connector maelys_egress_connector_t;
typedef struct maelys_egress_session maelys_egress_session_t;

typedef maelys_egress_result_t (*maelys_egress_attest_fn)(
    void *context,
    const void *canonical_receipt,
    size_t canonical_receipt_length,
    unsigned char *signature,
    size_t signature_capacity,
    size_t *out_signature_length,
    char **out_error);

typedef enum maelys_egress_unix_peer_policy {
    /* Require the normal HTTP/SOCKS credential only. */
    MAELYS_EGRESS_UNIX_PEER_AUTHENTICATED = 0,
    /* Additionally require the connecting process to have Egress's effective UID. */
    MAELYS_EGRESS_UNIX_PEER_SAME_EUID = 1
} maelys_egress_unix_peer_policy_t;

/*
 * receipt and all strings borrowed from it remain valid only during the call.
 * The sink runs synchronously on the loop owner and must not re-enter server
 * control operations.
 */
typedef void (*maelys_egress_receipt_sink_fn)(
    void *context,
    const maelys_egress_receipt_t *receipt);

const char *maelys_egress_version_string(void);
unsigned int maelys_egress_abi_version(void);
const char *maelys_egress_result_string(maelys_egress_result_t result);
void maelys_egress_error_free(char *error);

maelys_egress_result_t maelys_egress_policy_create(
    maelys_egress_policy_t **out_policy,
    char **out_error);
maelys_egress_result_t maelys_egress_policy_allow_tcp(
    maelys_egress_policy_t *policy,
    const char *canonical_host,
    uint16_t port,
    int allow_private_addresses,
    char **out_error);
/*
 * Require the first bytes of every tunnel to be a bounded TLS ClientHello
 * whose sole server_name equals the destination host. The destination must
 * already exist, must use a DNS name (not a numeric address), and the setting
 * becomes part of the sealed policy digest. HTTP forward mode is unaffected.
 */
maelys_egress_result_t maelys_egress_policy_require_tls_sni(
    maelys_egress_policy_t *policy,
    const char *canonical_host,
    uint16_t port,
    char **out_error);
/* Resolves and pins every allowed hostname. This can use the host resolver. */
maelys_egress_result_t maelys_egress_policy_seal(
    maelys_egress_policy_t *policy,
    char **out_error);
/*
 * Build and seal a fresh immutable policy from an existing sealed policy,
 * resolving every destination again. The source is never mutated. Embedders
 * can atomically replace their server/policy generation after this succeeds.
 */
maelys_egress_result_t maelys_egress_policy_reseal(
    const maelys_egress_policy_t *source,
    maelys_egress_policy_t **out_policy,
    char **out_error);
int maelys_egress_policy_is_sealed(const maelys_egress_policy_t *policy);
const char *maelys_egress_policy_digest_hex(const maelys_egress_policy_t *policy);
void maelys_egress_policy_destroy(maelys_egress_policy_t *policy);

maelys_egress_result_t maelys_egress_config_create(
    maelys_egress_config_t **out_config,
    char **out_error);
maelys_egress_result_t maelys_egress_config_set_listen(
    maelys_egress_config_t *config,
    const char *numeric_host,
    uint16_t port,
    char **out_error);
/*
 * Select a filesystem AF_UNIX listener. path_length must equal strlen(path),
 * so embedded NUL input is rejected. The path must be absent beneath an
 * existing caller-owned mode-0700 directory. Proxy authentication remains
 * mandatory under both peer policies; SAME_EUID is an additional check and
 * the loopback development opt-out never applies here.
 */
maelys_egress_result_t maelys_egress_config_set_listen_unix(
    maelys_egress_config_t *config,
    const char *path,
    size_t path_length,
    maelys_egress_unix_peer_policy_t peer_policy,
    char **out_error);
/*
 * Bind a private AF_UNIX listener to exactly one execution principal without
 * an HTTP/SOCKS credential. The Unix listener must already be configured with
 * SAME_EUID, no other principal may exist, and the binding is immutable for
 * the rest of the configuration lifetime. Requests carrying proxy
 * credentials are rejected rather than silently ignored.
 */
maelys_egress_result_t maelys_egress_config_set_unix_principal(
    maelys_egress_config_t *config,
    const char *username,
    const char *invocation_id,
    char **out_error);
/*
 * Disable the HTTP/SOCKS listener and expose only authenticated native
 * connectors. For an in-process embedder, the event loop and the complete
 * policy/relay/receipt engine remain active, but no proxy port or pathname is
 * made reachable.
 */
maelys_egress_result_t maelys_egress_config_set_native_only(
    maelys_egress_config_t *config,
    int enabled,
    char **out_error);
maelys_egress_result_t maelys_egress_config_set_authentication(
    maelys_egress_config_t *config,
    const char *username,
    const char *secret,
    char **out_error);
/* Add one independently receipted principal. Secrets must be unique. */
maelys_egress_result_t maelys_egress_config_add_principal(
    maelys_egress_config_t *config,
    const char *username,
    const char *secret,
    const char *invocation_id,
    char **out_error);
/* Zero means unlimited. The named principal must already exist. */
maelys_egress_result_t maelys_egress_config_set_principal_quota(
    maelys_egress_config_t *config,
    const char *username,
    size_t max_active_connections,
    uint64_t max_bytes_per_connection,
    char **out_error);
/*
 * ABI 2 quota contract. All byte ceilings count admitted relay payload in
 * both directions; proxy authentication and framing bytes are excluded.
 * Zero retains the historical unlimited meaning for either byte ceiling.
 */
maelys_egress_result_t maelys_egress_config_set_principal_quota_v2(
    maelys_egress_config_t *config,
    const char *username,
    size_t max_active_connections,
    uint64_t max_bytes_per_connection,
    uint64_t max_bytes_total,
    char **out_error);
maelys_egress_result_t maelys_egress_config_allow_unauthenticated_loopback(
    maelys_egress_config_t *config,
    int enabled,
    char **out_error);
maelys_egress_result_t maelys_egress_config_set_limits(
    maelys_egress_config_t *config,
    size_t max_connections,
    size_t buffer_bytes_per_direction,
    uint64_t handshake_timeout_ms,
    uint64_t idle_timeout_ms,
    char **out_error);
void maelys_egress_config_set_receipt_sink(
    maelys_egress_config_t *config,
    maelys_egress_receipt_sink_fn sink,
    void *context);
/* Retains provider. NULL restores the plaintext loopback-only listener. */
maelys_egress_result_t maelys_egress_config_set_tls_listener(
    maelys_egress_config_t *config,
    maelys_egress_tls_provider_t *provider,
    char **out_error);
/* Optional loopback-only HTTP endpoint exposing /healthz and /metrics. Port 0 is allowed. */
maelys_egress_result_t maelys_egress_config_set_admin_listen(
    maelys_egress_config_t *config,
    const char *numeric_loopback_host,
    uint16_t port,
    char **out_error);
/* Retains audit. NULL disables durable audit. */
maelys_egress_result_t maelys_egress_config_set_audit(
    maelys_egress_config_t *config,
    maelys_egress_audit_t *audit,
    char **out_error);
/* Retains attestor. NULL disables asymmetric receipt attestation. */
maelys_egress_result_t maelys_egress_config_set_receipt_attestor(
    maelys_egress_config_t *config,
    maelys_egress_attestor_t *attestor,
    char **out_error);
void maelys_egress_config_destroy(maelys_egress_config_t *config);

maelys_egress_result_t maelys_egress_server_create(
    const maelys_egress_policy_t *policy,
    const maelys_egress_config_t *config,
    maelys_egress_server_t **out_server,
    char **out_error);
uint16_t maelys_egress_server_port(const maelys_egress_server_t *server);
uint16_t maelys_egress_server_admin_port(const maelys_egress_server_t *server);
/* Thread-safe readiness predicate for connector users. */
int maelys_egress_server_is_running(const maelys_egress_server_t *server);
/*
 * Thread-safe. Atomically replace the sealed policy used for new admissions.
 * Existing streams retain their destination, digest and generation snapshot.
 */
maelys_egress_result_t maelys_egress_server_replace_policy(
    maelys_egress_server_t *server,
    const maelys_egress_policy_t *policy,
    uint64_t *out_generation,
    char **out_error);
uint64_t maelys_egress_server_policy_generation(
    const maelys_egress_server_t *server);
maelys_egress_result_t maelys_egress_server_metrics_snapshot(
    const maelys_egress_server_t *server,
    maelys_egress_metrics_t **out_metrics,
    char **out_error);
/* Single-use: run owns the creating thread until stop or a fatal loop error. */
maelys_egress_result_t maelys_egress_server_run(
    maelys_egress_server_t *server,
    char **out_error);
/*
 * Thread-safe, idempotent and wakeup-backed. Policy replacement, generation
 * reads and metric snapshots are also thread-safe. Destruction serializes with
 * an already-entered control call. As with every opaque handle, callers must
 * not begin a new operation after destroy begins.
 */
maelys_egress_result_t maelys_egress_server_stop(maelys_egress_server_t *server);
/* Call on the creating/owner thread after server_run has returned. */
void maelys_egress_server_destroy(maelys_egress_server_t *server);

/*
 * Create an authenticated in-process connector bound to this server's
 * immutable principal namespace. The credentials are checked once and are
 * not retained by the connector. The connector retains the server control
 * object, is immutable after creation and may open sessions concurrently.
 *
 * The server must have entered server_run before session_open. Stopping the
 * server cancels pending opens and active sessions. Releasing the connector
 * does not close sessions that were already returned.
 */
maelys_egress_result_t maelys_egress_server_connector_create(
    maelys_egress_server_t *server,
    const char *username,
    const char *secret,
    maelys_egress_connector_t **out_connector,
    char **out_error);
void maelys_egress_connector_retain(maelys_egress_connector_t *connector);
void maelys_egress_connector_release(maelys_egress_connector_t *connector);

/*
 * Admit one exact TCP destination and return only after Egress has connected to
 * a pinned upstream address. timeout_ms is a finite relative deadline and
 * must be non-zero. The resulting fd is a blocking CLOEXEC TCP stream whose
 * peer remains owned and relayed by Egress; it is never the upstream socket.
 * SNI guards, quotas, byte accounting, half-close and receipts therefore have
 * the same semantics as HTTP CONNECT and SOCKS5.
 */
maelys_egress_result_t maelys_egress_connector_session_open(
    maelys_egress_connector_t *connector,
    const char *canonical_host,
    uint16_t port,
    uint64_t timeout_ms,
    maelys_egress_session_t **out_session,
    char **out_error);
int maelys_egress_session_fd(const maelys_egress_session_t *session);
/* Transfer fd ownership to the caller and leave the session empty. */
maelys_egress_result_t maelys_egress_session_take_fd(
    maelys_egress_session_t *session,
    int *out_fd,
    char **out_error);
/* Closes the client stream if ownership was not transferred. */
void maelys_egress_session_release(maelys_egress_session_t *session);

uint64_t maelys_egress_receipt_id(const maelys_egress_receipt_t *receipt);
maelys_egress_protocol_t maelys_egress_receipt_protocol(
    const maelys_egress_receipt_t *receipt);
const char *maelys_egress_receipt_host(const maelys_egress_receipt_t *receipt);
uint16_t maelys_egress_receipt_port(const maelys_egress_receipt_t *receipt);
maelys_egress_result_t maelys_egress_receipt_result(
    const maelys_egress_receipt_t *receipt);
uint64_t maelys_egress_receipt_started_unix_ms(
    const maelys_egress_receipt_t *receipt);
uint64_t maelys_egress_receipt_duration_ms(const maelys_egress_receipt_t *receipt);
uint64_t maelys_egress_receipt_bytes_from_client(
    const maelys_egress_receipt_t *receipt);
uint64_t maelys_egress_receipt_bytes_to_client(
    const maelys_egress_receipt_t *receipt);
const char *maelys_egress_receipt_policy_digest_hex(
    const maelys_egress_receipt_t *receipt);
const char *maelys_egress_receipt_invocation_id(
    const maelys_egress_receipt_t *receipt);
int maelys_egress_receipt_tls_sni_verified(
    const maelys_egress_receipt_t *receipt);
const char *maelys_egress_receipt_principal(
    const maelys_egress_receipt_t *receipt);
uint64_t maelys_egress_receipt_policy_generation(
    const maelys_egress_receipt_t *receipt);
const char *maelys_egress_receipt_attestor(const maelys_egress_receipt_t *receipt);
const char *maelys_egress_receipt_attestation_key_id(
    const maelys_egress_receipt_t *receipt);
const char *maelys_egress_receipt_attestation_hex(
    const maelys_egress_receipt_t *receipt);
maelys_egress_quota_scope_t maelys_egress_receipt_quota_scope(
    const maelys_egress_receipt_t *receipt);
uint64_t maelys_egress_receipt_quota_connection_max_bytes(
    const maelys_egress_receipt_t *receipt);
uint64_t maelys_egress_receipt_quota_execution_max_bytes(
    const maelys_egress_receipt_t *receipt);
uint64_t maelys_egress_receipt_quota_connection_observed_bytes(
    const maelys_egress_receipt_t *receipt);
uint64_t maelys_egress_receipt_quota_execution_before_bytes(
    const maelys_egress_receipt_t *receipt);
uint64_t maelys_egress_receipt_quota_execution_after_bytes(
    const maelys_egress_receipt_t *receipt);

uint64_t maelys_egress_metrics_accepted(const maelys_egress_metrics_t *metrics);
uint64_t maelys_egress_metrics_active(const maelys_egress_metrics_t *metrics);
uint64_t maelys_egress_metrics_admitted(const maelys_egress_metrics_t *metrics);
uint64_t maelys_egress_metrics_denied(const maelys_egress_metrics_t *metrics);
uint64_t maelys_egress_metrics_auth_failures(const maelys_egress_metrics_t *metrics);
uint64_t maelys_egress_metrics_quota_denials(const maelys_egress_metrics_t *metrics);
uint64_t maelys_egress_metrics_bytes_from_clients(const maelys_egress_metrics_t *metrics);
uint64_t maelys_egress_metrics_bytes_to_clients(const maelys_egress_metrics_t *metrics);
uint64_t maelys_egress_metrics_receipts(const maelys_egress_metrics_t *metrics);
uint64_t maelys_egress_metrics_policy_generation(const maelys_egress_metrics_t *metrics);
uint64_t maelys_egress_metrics_reload_failures(const maelys_egress_metrics_t *metrics);
uint64_t maelys_egress_metrics_audit_failures(const maelys_egress_metrics_t *metrics);
void maelys_egress_metrics_destroy(maelys_egress_metrics_t *metrics);

/*
 * Append-only, fdatasync-backed, HMAC-SHA-256 chained JSONL audit. The key is
 * copied and zeroed at final release. This proves integrity to key holders;
 * it is not a public-key attestation and cannot alone detect tail truncation.
 */
maelys_egress_result_t maelys_egress_audit_file_create(
    const char *path,
    const void *key,
    size_t key_length,
    const char *key_id,
    maelys_egress_audit_t **out_audit,
    char **out_error);
void maelys_egress_audit_retain(maelys_egress_audit_t *audit);
void maelys_egress_audit_release(maelys_egress_audit_t *audit);
uint64_t maelys_egress_audit_record_count(const maelys_egress_audit_t *audit);
maelys_egress_result_t maelys_egress_audit_chain_copy(
    maelys_egress_audit_t *audit,
    char out_chain_hex[65]);
int maelys_egress_audit_healthy(const maelys_egress_audit_t *audit);

/*
 * Generic asymmetric/HSM attestation seam. Signing executes synchronously on
 * the loop owner and must not re-enter server control operations.
 */
maelys_egress_result_t maelys_egress_attestor_create(
    const char *name,
    const char *key_id,
    size_t max_signature_bytes,
    maelys_egress_attest_fn attest,
    void *context,
    void (*release_context)(void *context),
    maelys_egress_attestor_t **out_attestor,
    char **out_error);
void maelys_egress_attestor_retain(maelys_egress_attestor_t *attestor);
void maelys_egress_attestor_release(maelys_egress_attestor_t *attestor);

/* Every receipt pointer and borrowed string is valid only during its sink call. */

#ifdef __cplusplus
}
#endif

#endif

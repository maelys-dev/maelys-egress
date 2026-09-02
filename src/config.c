#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "src/internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

maelys_egress_result_t maelys_egress_config_create(
    maelys_egress_config_t **out_config,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (!out_config) return MAELYS_EGRESS_ERR_ARGUMENT;
    maelys_egress_config_t *config = calloc(1, sizeof(*config));
    if (!config) return MAELYS_EGRESS_ERR_MEMORY;
    (void)snprintf(config->listen_host, sizeof(config->listen_host), "127.0.0.1");
    config->max_connections = EGRESS_DEFAULT_MAX_CONNECTIONS;
    config->buffer_bytes = EGRESS_DEFAULT_BUFFER_BYTES;
    config->handshake_timeout_ms = 10000u;
    config->idle_timeout_ms = 60000u;
    (void)snprintf(config->admin_host, sizeof(config->admin_host), "127.0.0.1");
    *out_config = config;
    return MAELYS_EGRESS_OK;
}

maelys_egress_result_t maelys_egress_config_set_listen(
    maelys_egress_config_t *config,
    const char *numeric_host,
    uint16_t port,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (!config || !numeric_host) return MAELYS_EGRESS_ERR_ARGUMENT;
    if (config->unix_principal_bound) {
        egress_set_error(out_error, "endpoint-bound Unix principal is immutable");
        return MAELYS_EGRESS_ERR_STATE;
    }
    struct in_addr ipv4;
    struct in6_addr ipv6;
    if (inet_pton(AF_INET, numeric_host, &ipv4) != 1 &&
        inet_pton(AF_INET6, numeric_host, &ipv6) != 1) {
        egress_set_error(out_error, "listen host must be a numeric IPv4 or IPv6 address");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    if (strlen(numeric_host) >= sizeof(config->listen_host)) {
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    (void)snprintf(config->listen_host, sizeof(config->listen_host), "%s", numeric_host);
    config->port = port;
    config->native_only = 0;
    config->listen_unix = 0;
    config->unix_path[0] = '\0';
    return MAELYS_EGRESS_OK;
}

static int unix_path_lexically_canonical(const char *path) {
    if (!path || path[0] != '/' || path[1] == '\0') return 0;
    const char *component = path + 1;
    while (*component) {
        const char *slash = strchr(component, '/');
        size_t length = slash ? (size_t)(slash - component) : strlen(component);
        if (length == 0u || (length == 1u && component[0] == '.') ||
            (length == 2u && component[0] == '.' && component[1] == '.')) return 0;
        if (!slash) return 1;
        component = slash + 1;
    }
    return 0;
}

int egress_unix_parent_is_private(const char *path, char **out_error) {
    char parent[sizeof(((struct sockaddr_un *)0)->sun_path)];
    (void)snprintf(parent, sizeof(parent), "%s", path);
    char *last = strrchr(parent, '/');
    if (!last || last == parent) {
        egress_set_error(out_error, "Unix listener requires a private parent directory");
        return 0;
    }
    *last = '\0';
    int directory = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
        egress_set_error(out_error, "cannot open Unix listener root: %s", strerror(errno));
        return 0;
    }
    const char *component = parent + 1;
    int safe = 1;
    while (*component && safe) {
        const char *slash = strchr(component, '/');
        size_t length = slash ? (size_t)(slash - component) : strlen(component);
        char name[sizeof(parent)];
        if (length == 0u || length >= sizeof(name)) {
            errno = EINVAL;
            safe = 0;
            break;
        }
        memcpy(name, component, length);
        name[length] = '\0';
        int next = openat(directory, name,
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) {
            safe = 0;
            break;
        }
        (void)maelys_sys_fd_close(&directory);
        directory = next;
        component = slash ? slash + 1 : component + length;
    }
    struct stat status;
    if (safe && fstat(directory, &status) != 0) safe = 0;
    if (safe && (status.st_uid != geteuid() ||
                 (status.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) != S_IRWXU)) {
        errno = EPERM;
        safe = 0;
    }
    int saved = errno;
    (void)maelys_sys_fd_close(&directory);
    if (!safe) {
        egress_set_error(out_error,
            "Unix listener parent must be caller-owned mode 0700 and contain no symlink: %s",
            strerror(saved));
        errno = saved;
    }
    return safe;
}

maelys_egress_result_t maelys_egress_config_set_listen_unix(
    maelys_egress_config_t *config,
    const char *path,
    size_t path_length,
    maelys_egress_unix_peer_policy_t peer_policy,
    char **out_error) {
    if (out_error) *out_error = NULL;
    size_t bounded_length = path ? strnlen(path, sizeof(config->unix_path)) : 0u;
    if (config && config->unix_principal_bound) {
        egress_set_error(out_error, "endpoint-bound Unix principal is immutable");
        return MAELYS_EGRESS_ERR_STATE;
    }
    if (!config || !path || path_length == 0u ||
        bounded_length != path_length || path_length >= sizeof(config->unix_path) ||
        (peer_policy != MAELYS_EGRESS_UNIX_PEER_AUTHENTICATED &&
         peer_policy != MAELYS_EGRESS_UNIX_PEER_SAME_EUID)) {
        egress_set_error(out_error, "invalid Unix listener path or peer policy");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    if (!unix_path_lexically_canonical(path)) {
        egress_set_error(out_error, "Unix listener path must be absolute and canonical");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    if (!egress_unix_parent_is_private(path, out_error)) return MAELYS_EGRESS_ERR_DENIED;
    struct stat status;
    if (lstat(path, &status) == 0) {
        egress_set_error(out_error, "Unix listener path already exists");
        return MAELYS_EGRESS_ERR_STATE;
    }
    if (errno != ENOENT) {
        egress_set_error(out_error, "cannot inspect Unix listener path: %s", strerror(errno));
        return MAELYS_EGRESS_ERR_IO;
    }
    memcpy(config->unix_path, path, path_length + 1u);
    config->native_only = 0;
    config->listen_unix = 1;
    config->unix_peer_policy = peer_policy;
    config->port = 0u;
    return MAELYS_EGRESS_OK;
}

static int canonical_principal_identity(
    const char *username, const char *invocation_id) {
    if (!username || !username[0] || strlen(username) > EGRESS_MAX_USERNAME ||
        (invocation_id && (!invocation_id[0] ||
         strlen(invocation_id) > EGRESS_MAX_INVOCATION_ID))) return 0;
    for (const unsigned char *p = (const unsigned char *)username; *p; ++p) {
        int allowed = (*p >= 'a' && *p <= 'z') ||
            (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '+' ||
            *p == '@' || *p == '/';
        if (!allowed) return 0;
    }
    if (invocation_id) {
        for (const unsigned char *p = (const unsigned char *)invocation_id; *p; ++p) {
            int allowed = (*p >= 'a' && *p <= 'z') ||
                (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
                *p == '-' || *p == '_' || *p == '.' || *p == ':' || *p == '/';
            if (!allowed) return 0;
        }
    }
    return 1;
}

maelys_egress_result_t maelys_egress_config_set_unix_principal(
    maelys_egress_config_t *config,
    const char *username,
    const char *invocation_id,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (!config || !canonical_principal_identity(username, invocation_id)) {
        egress_set_error(out_error, "canonical Unix principal and invocation id are required");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    if (!config->listen_unix ||
        config->unix_peer_policy != MAELYS_EGRESS_UNIX_PEER_SAME_EUID) {
        egress_set_error(out_error,
            "endpoint-bound principal requires a private SAME_EUID Unix listener");
        return MAELYS_EGRESS_ERR_DENIED;
    }
    if (config->unix_principal_bound || config->principal_count != 0u ||
        config->authentication_set) {
        egress_set_error(out_error,
            "endpoint-bound Unix listener requires an empty principal namespace");
        return MAELYS_EGRESS_ERR_STATE;
    }
    egress_principal_t *principal = &config->principals[0];
    (void)snprintf(principal->username, sizeof(principal->username), "%s", username);
    if (invocation_id) {
        (void)snprintf(principal->invocation_id, sizeof(principal->invocation_id),
                       "%s", invocation_id);
    }
    config->principal_count = 1u;
    config->unix_principal_index = 0u;
    config->unix_principal_bound = 1;
    return MAELYS_EGRESS_OK;
}

maelys_egress_result_t maelys_egress_config_set_native_only(
    maelys_egress_config_t *config,
    int enabled,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (!config || (enabled != 0 && enabled != 1)) {
        egress_set_error(out_error, "native-only mode expects enabled=0 or enabled=1");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    if (config->unix_principal_bound) {
        egress_set_error(out_error, "endpoint-bound Unix principal is immutable");
        return MAELYS_EGRESS_ERR_STATE;
    }
    config->native_only = enabled;
    if (enabled) {
        config->listen_unix = 0;
        config->listen_host[0] = '\0';
        config->unix_path[0] = '\0';
        config->port = 0u;
    } else if (!config->listen_host[0]) {
        (void)snprintf(config->listen_host, sizeof(config->listen_host), "127.0.0.1");
    }
    return MAELYS_EGRESS_OK;
}

maelys_egress_result_t maelys_egress_config_set_authentication(
    maelys_egress_config_t *config,
    const char *username,
    const char *secret,
    char **out_error) {
    if (!config) return MAELYS_EGRESS_ERR_ARGUMENT;
    if (config->unix_principal_bound) {
        egress_set_error(out_error, "endpoint-bound Unix principal forbids credential authentication");
        return MAELYS_EGRESS_ERR_STATE;
    }
    maelys_egress_config_t scratch;
    memset(&scratch, 0, sizeof(scratch));
    maelys_egress_result_t result = maelys_egress_config_add_principal(
        &scratch, username, secret, NULL, out_error);
    if (result != MAELYS_EGRESS_OK) {
        egress_secure_zero(&scratch, sizeof(scratch));
        return result;
    }
    for (size_t i = 0; i < config->principal_count; ++i) {
        egress_secure_zero(&config->principals[i], sizeof(config->principals[i]));
    }
    config->principals[0] = scratch.principals[0];
    config->principal_count = 1u;
    config->authentication_set = 1;
    egress_secure_zero(&scratch, sizeof(scratch));
    return MAELYS_EGRESS_OK;
}

maelys_egress_result_t maelys_egress_config_add_principal(
    maelys_egress_config_t *config,
    const char *username,
    const char *secret,
    const char *invocation_id,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (config && config->unix_principal_bound) {
        egress_set_error(out_error, "endpoint-bound Unix principal forbids credential principals");
        return MAELYS_EGRESS_ERR_STATE;
    }
    if (!config || !username || !secret || !username[0] ||
        strlen(username) > EGRESS_MAX_USERNAME || strlen(secret) < 16u ||
        strlen(secret) > EGRESS_MAX_SECRET || strchr(username, ':') ||
        (invocation_id && (!invocation_id[0] ||
         strlen(invocation_id) > EGRESS_MAX_INVOCATION_ID)) ||
        config->principal_count >= EGRESS_MAX_PRINCIPALS) {
        egress_set_error(out_error,
            "principal requires username <= %u, secret length 16..%u and bounded invocation id",
            (unsigned int)EGRESS_MAX_USERNAME, (unsigned int)EGRESS_MAX_SECRET);
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    if (!canonical_principal_identity(username, invocation_id)) {
        egress_set_error(out_error, "authentication principal identity must be canonical ASCII");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    for (const unsigned char *p = (const unsigned char *)secret; *p; ++p) {
        if (*p <= 0x20u || *p >= 0x7fu) {
            egress_set_error(out_error, "authentication secret must be visible ASCII");
            return MAELYS_EGRESS_ERR_ARGUMENT;
        }
    }
    for (size_t i = 0; i < config->principal_count; ++i) {
        if (egress_constant_time_equal(secret, config->principals[i].secret)) {
            egress_set_error(out_error, "principal secret must be unique");
            return MAELYS_EGRESS_ERR_ARGUMENT;
        }
    }
    egress_principal_t *principal = &config->principals[config->principal_count++];
    (void)snprintf(principal->username, sizeof(principal->username), "%s", username);
    (void)snprintf(principal->secret, sizeof(principal->secret), "%s", secret);
    if (invocation_id) {
        (void)snprintf(principal->invocation_id, sizeof(principal->invocation_id),
                       "%s", invocation_id);
    }
    config->authentication_set = 1;
    return MAELYS_EGRESS_OK;
}

maelys_egress_result_t maelys_egress_config_set_principal_quota(
    maelys_egress_config_t *config, const char *username,
    size_t max_active_connections, uint64_t max_bytes_per_connection,
    char **out_error) {
    return maelys_egress_config_set_principal_quota_v2(
        config, username, max_active_connections, max_bytes_per_connection,
        0u, out_error);
}

maelys_egress_result_t maelys_egress_config_set_principal_quota_v2(
    maelys_egress_config_t *config, const char *username,
    size_t max_active_connections, uint64_t max_bytes_per_connection,
    uint64_t max_bytes_total, char **out_error) {
    if (out_error) *out_error = NULL;
    if (!config || !username || !username[0] || max_active_connections > 4096u) {
        egress_set_error(out_error, "valid existing principal and bounded quota required");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    for (size_t i = 0; i < config->principal_count; ++i) {
        if (strcmp(config->principals[i].username, username) == 0) {
            config->principals[i].max_active_connections = max_active_connections;
            config->principals[i].max_bytes_per_connection = max_bytes_per_connection;
            config->principals[i].max_bytes_total = max_bytes_total;
            return MAELYS_EGRESS_OK;
        }
    }
    egress_set_error(out_error, "quota principal does not exist: %s", username);
    return MAELYS_EGRESS_ERR_ARGUMENT;
}

maelys_egress_result_t maelys_egress_config_allow_unauthenticated_loopback(
    maelys_egress_config_t *config,
    int enabled,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (!config) return MAELYS_EGRESS_ERR_ARGUMENT;
    config->unauthenticated_loopback = enabled != 0;
    return MAELYS_EGRESS_OK;
}

maelys_egress_result_t maelys_egress_config_set_limits(
    maelys_egress_config_t *config,
    size_t max_connections,
    size_t buffer_bytes_per_direction,
    uint64_t handshake_timeout_ms,
    uint64_t idle_timeout_ms,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (!config || max_connections == 0u || max_connections > 4096u ||
        buffer_bytes_per_direction < EGRESS_MIN_BUFFER_BYTES ||
        buffer_bytes_per_direction > EGRESS_MAX_BUFFER_BYTES ||
        handshake_timeout_ms == 0u || idle_timeout_ms == 0u) {
        egress_set_error(out_error, "invalid connection, buffer or timeout limit");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    config->max_connections = max_connections;
    config->buffer_bytes = buffer_bytes_per_direction;
    config->handshake_timeout_ms = handshake_timeout_ms;
    config->idle_timeout_ms = idle_timeout_ms;
    return MAELYS_EGRESS_OK;
}

void maelys_egress_config_set_receipt_sink(
    maelys_egress_config_t *config,
    maelys_egress_receipt_sink_fn sink,
    void *context) {
    if (!config) return;
    config->receipt_sink = sink;
    config->receipt_context = context;
}

maelys_egress_result_t maelys_egress_config_set_tls_listener(
    maelys_egress_config_t *config,
    maelys_egress_tls_provider_t *provider,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (!config) return MAELYS_EGRESS_ERR_ARGUMENT;
    if (provider) maelys_egress_tls_provider_retain(provider);
    maelys_egress_tls_provider_release(config->tls_provider);
    config->tls_provider = provider;
    return MAELYS_EGRESS_OK;
}

maelys_egress_result_t maelys_egress_config_set_admin_listen(
    maelys_egress_config_t *config, const char *numeric_loopback_host,
    uint16_t port, char **out_error) {
    if (out_error) *out_error = NULL;
    if (!config || !numeric_loopback_host ||
        (strcmp(numeric_loopback_host, "127.0.0.1") != 0 &&
         strcmp(numeric_loopback_host, "::1") != 0)) {
        egress_set_error(out_error, "admin endpoint must be non-zero IPv4/IPv6 loopback");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    (void)snprintf(config->admin_host, sizeof(config->admin_host), "%s",
                   numeric_loopback_host);
    config->admin_port = port;
    config->admin_enabled = 1;
    return MAELYS_EGRESS_OK;
}

maelys_egress_result_t maelys_egress_config_set_audit(
    maelys_egress_config_t *config, maelys_egress_audit_t *audit,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (!config) return MAELYS_EGRESS_ERR_ARGUMENT;
    if (audit && !maelys_egress_audit_healthy(audit)) {
        egress_set_error(out_error, "audit handle is unhealthy");
        return MAELYS_EGRESS_ERR_STATE;
    }
    maelys_egress_audit_retain(audit);
    maelys_egress_audit_release(config->audit);
    config->audit = audit;
    return MAELYS_EGRESS_OK;
}

maelys_egress_result_t maelys_egress_config_set_receipt_attestor(
    maelys_egress_config_t *config, maelys_egress_attestor_t *attestor,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (!config) return MAELYS_EGRESS_ERR_ARGUMENT;
    maelys_egress_attestor_retain(attestor);
    maelys_egress_attestor_release(config->attestor);
    config->attestor = attestor;
    return MAELYS_EGRESS_OK;
}

void maelys_egress_config_destroy(maelys_egress_config_t *config) {
    if (!config) return;
    egress_secure_zero(config->principals, sizeof(config->principals));
    maelys_egress_tls_provider_release(config->tls_provider);
    maelys_egress_audit_release(config->audit);
    maelys_egress_attestor_release(config->attestor);
    free(config);
}

uint64_t maelys_egress_receipt_id(const maelys_egress_receipt_t *receipt) {
    return receipt ? receipt->id : 0u;
}
maelys_egress_protocol_t maelys_egress_receipt_protocol(const maelys_egress_receipt_t *receipt) {
    return receipt ? receipt->protocol : 0;
}
const char *maelys_egress_receipt_host(const maelys_egress_receipt_t *receipt) {
    return receipt ? receipt->host : NULL;
}
uint16_t maelys_egress_receipt_port(const maelys_egress_receipt_t *receipt) {
    return receipt ? receipt->port : 0u;
}
maelys_egress_result_t maelys_egress_receipt_result(const maelys_egress_receipt_t *receipt) {
    return receipt ? receipt->result : MAELYS_EGRESS_ERR_ARGUMENT;
}
uint64_t maelys_egress_receipt_started_unix_ms(const maelys_egress_receipt_t *receipt) {
    return receipt ? receipt->started_unix_ms : 0u;
}
uint64_t maelys_egress_receipt_duration_ms(const maelys_egress_receipt_t *receipt) {
    return receipt ? receipt->duration_ms : 0u;
}
uint64_t maelys_egress_receipt_bytes_from_client(const maelys_egress_receipt_t *receipt) {
    return receipt ? receipt->bytes_from_client : 0u;
}
uint64_t maelys_egress_receipt_bytes_to_client(const maelys_egress_receipt_t *receipt) {
    return receipt ? receipt->bytes_to_client : 0u;
}
const char *maelys_egress_receipt_policy_digest_hex(const maelys_egress_receipt_t *receipt) {
    return receipt ? receipt->policy_digest_hex : NULL;
}
const char *maelys_egress_receipt_invocation_id(const maelys_egress_receipt_t *receipt) {
    return receipt && receipt->invocation_id[0] ? receipt->invocation_id : NULL;
}
int maelys_egress_receipt_tls_sni_verified(const maelys_egress_receipt_t *receipt) {
    return receipt ? receipt->tls_sni_verified : 0;
}
const char *maelys_egress_receipt_principal(const maelys_egress_receipt_t *receipt) {
    return receipt && receipt->principal[0] ? receipt->principal : NULL;
}
uint64_t maelys_egress_receipt_policy_generation(const maelys_egress_receipt_t *receipt) {
    return receipt ? receipt->policy_generation : 0u;
}
const char *maelys_egress_receipt_attestor(const maelys_egress_receipt_t *receipt) {
    return receipt && receipt->attestor[0] ? receipt->attestor : NULL;
}
const char *maelys_egress_receipt_attestation_key_id(
    const maelys_egress_receipt_t *receipt) {
    return receipt && receipt->attestation_key_id[0] ?
        receipt->attestation_key_id : NULL;
}
const char *maelys_egress_receipt_attestation_hex(
    const maelys_egress_receipt_t *receipt) {
    return receipt && receipt->attestation_hex[0] ? receipt->attestation_hex : NULL;
}
maelys_egress_quota_scope_t maelys_egress_receipt_quota_scope(
    const maelys_egress_receipt_t *receipt) {
    return receipt ? receipt->quota_scope : MAELYS_EGRESS_QUOTA_NONE;
}
uint64_t maelys_egress_receipt_quota_connection_max_bytes(
    const maelys_egress_receipt_t *receipt) {
    return receipt ? receipt->quota_connection_max_bytes : 0u;
}
uint64_t maelys_egress_receipt_quota_execution_max_bytes(
    const maelys_egress_receipt_t *receipt) {
    return receipt ? receipt->quota_execution_max_bytes : 0u;
}
uint64_t maelys_egress_receipt_quota_connection_observed_bytes(
    const maelys_egress_receipt_t *receipt) {
    return receipt ? receipt->quota_connection_observed_bytes : 0u;
}
uint64_t maelys_egress_receipt_quota_execution_before_bytes(
    const maelys_egress_receipt_t *receipt) {
    return receipt ? receipt->quota_execution_before_bytes : 0u;
}
uint64_t maelys_egress_receipt_quota_execution_after_bytes(
    const maelys_egress_receipt_t *receipt) {
    return receipt ? receipt->quota_execution_after_bytes : 0u;
}

#define METRIC_GETTER(name, field) \
    uint64_t maelys_egress_metrics_##name(const maelys_egress_metrics_t *metrics) { \
        return metrics ? metrics->field : 0u; \
    }
METRIC_GETTER(accepted, accepted)
METRIC_GETTER(active, active)
METRIC_GETTER(admitted, admitted)
METRIC_GETTER(denied, denied)
METRIC_GETTER(auth_failures, auth_failures)
METRIC_GETTER(quota_denials, quota_denials)
METRIC_GETTER(bytes_from_clients, bytes_from_clients)
METRIC_GETTER(bytes_to_clients, bytes_to_clients)
METRIC_GETTER(receipts, receipts)
METRIC_GETTER(policy_generation, policy_generation)
METRIC_GETTER(reload_failures, reload_failures)
METRIC_GETTER(audit_failures, audit_failures)
#undef METRIC_GETTER

void maelys_egress_metrics_destroy(maelys_egress_metrics_t *metrics) { free(metrics); }

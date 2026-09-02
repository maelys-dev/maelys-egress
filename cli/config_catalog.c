#include "cli/config_catalog.h"

#include <string.h>

/* name, key, type, required, repeatable, secret, tls_only, default, range,
 * allowed values, requires, conflicts, description. */
static const egress_cli_config_spec_t config_specs[] = {
    {"schema_version", EGRESS_CLI_KEY_SCHEMA_VERSION, EGRESS_CLI_CONFIG_INTEGER,
        1, 0, 0, 0, NULL, "exactly 1", NULL, NULL, NULL,
        "Configuration grammar version. Version 1 is required."},
    {"listen", EGRESS_CLI_KEY_LISTEN, EGRESS_CLI_CONFIG_ENDPOINT, 0, 0, 0, 0,
        "127.0.0.1:0", "numeric loopback unless TLS+authentication are enabled",
        NULL, NULL, "listen_unix",
        "TCP proxy listener as HOST:PORT."},
    {"listen_unix", EGRESS_CLI_KEY_LISTEN_UNIX, EGRESS_CLI_CONFIG_PATH, 0, 0, 0, 0,
        NULL, "private existing parent directory", NULL, NULL, "listen",
        "Filesystem AF_UNIX proxy listener."},
    {"unix_peer", EGRESS_CLI_KEY_UNIX_PEER, EGRESS_CLI_CONFIG_ENUM, 0, 0, 0, 0,
        "authenticated", NULL, "authenticated,same-euid", "listen_unix", NULL,
        "Peer authentication rule for an AF_UNIX listener."},
    {"allow", EGRESS_CLI_KEY_ALLOW, EGRESS_CLI_CONFIG_DESTINATION, 0, 1, 0, 0,
        NULL, "HOST:1..65535", NULL, NULL, NULL,
        "Allow one exact public TCP destination."},
    {"allow_private", EGRESS_CLI_KEY_ALLOW_PRIVATE, EGRESS_CLI_CONFIG_DESTINATION,
        0, 1, 0, 0, NULL, "HOST:1..65535", NULL, NULL, NULL,
        "Allow one exact destination to resolve to private addresses."},
    {"allow_tls_sni", EGRESS_CLI_KEY_ALLOW_TLS_SNI, EGRESS_CLI_CONFIG_DESTINATION,
        0, 1, 0, 0, NULL, "HOST:1..65535", NULL, NULL, NULL,
        "Allow one destination and require matching readable TLS SNI."},
    {"token_file", EGRESS_CLI_KEY_TOKEN_FILE, EGRESS_CLI_CONFIG_PATH, 0, 0, 1, 0,
        NULL, "owner-only regular file, at least 16 bytes", NULL, NULL,
        "unauthenticated_loopback",
        "Bearer secret; the proxy username is maelys."},
    {"unauthenticated_loopback", EGRESS_CLI_KEY_UNAUTHENTICATED_LOOPBACK,
        EGRESS_CLI_CONFIG_BOOLEAN, 0, 0, 0, 0, "false", NULL,
        "true,false", NULL, "token_file,listen_unix,principal quotas",
        "Development-only opt-out on a loopback TCP listener."},
    {"max_connections", EGRESS_CLI_KEY_MAX_CONNECTIONS, EGRESS_CLI_CONFIG_INTEGER,
        0, 0, 0, 0, "128", "1..4096", NULL, NULL, NULL,
        "Maximum concurrent mediated connections."},
    {"quota_connections", EGRESS_CLI_KEY_QUOTA_CONNECTIONS, EGRESS_CLI_CONFIG_INTEGER,
        0, 0, 0, 0, "0", "0..4096; 0 disables", NULL, "token_file", NULL,
        "Maximum active connections for the authenticated principal."},
    {"quota_bytes", EGRESS_CLI_KEY_QUOTA_BYTES, EGRESS_CLI_CONFIG_INTEGER,
        0, 0, 0, 0, "0", "0..2^64-1; 0 disables", NULL, "token_file", NULL,
        "Per-stream client plus server byte budget."},
    {"quota_total_bytes", EGRESS_CLI_KEY_QUOTA_TOTAL_BYTES, EGRESS_CLI_CONFIG_INTEGER,
        0, 0, 0, 0, "0", "0..2^64-1; 0 disables", NULL, "token_file", NULL,
        "Cumulative byte budget for the principal execution."},
    {"admin_listen", EGRESS_CLI_KEY_ADMIN_LISTEN, EGRESS_CLI_CONFIG_ENDPOINT,
        0, 0, 0, 0, NULL, "numeric loopback only", NULL, NULL, NULL,
        "Independent health and aggregate metrics listener."},
    {"audit_log", EGRESS_CLI_KEY_AUDIT_LOG, EGRESS_CLI_CONFIG_PATH,
        0, 0, 0, 0, NULL, "append-only JSONL file", NULL,
        "audit_key_file,audit_key_id", NULL,
        "Durable authenticated receipt log."},
    {"audit_key_file", EGRESS_CLI_KEY_AUDIT_KEY_FILE, EGRESS_CLI_CONFIG_PATH,
        0, 0, 1, 0, NULL, "owner-only regular file, 16..4096 bytes", NULL,
        "audit_log,audit_key_id", NULL,
        "HMAC key protecting the durable audit chain."},
    {"audit_key_id", EGRESS_CLI_KEY_AUDIT_KEY_ID, EGRESS_CLI_CONFIG_STRING,
        0, 0, 0, 0, NULL, "stable printable identifier", NULL,
        "audit_log,audit_key_file", NULL,
        "Non-secret identifier recorded beside every audit MAC."},
    {"tls_cert", EGRESS_CLI_KEY_TLS_CERT, EGRESS_CLI_CONFIG_PATH,
        0, 0, 0, 1, NULL, "PEM certificate", NULL, "tls_key", NULL,
        "TLS listener certificate."},
    {"tls_key", EGRESS_CLI_KEY_TLS_KEY, EGRESS_CLI_CONFIG_PATH,
        0, 0, 1, 1, NULL, "PEM private key", NULL, "tls_cert", NULL,
        "TLS listener private key."},
    {"tls_ca", EGRESS_CLI_KEY_TLS_CA, EGRESS_CLI_CONFIG_PATH,
        0, 0, 0, 1, NULL, "PEM trust bundle", NULL, NULL, NULL,
        "Client CA used by mutual TLS."},
    {"require_client_cert", EGRESS_CLI_KEY_REQUIRE_CLIENT_CERT, EGRESS_CLI_CONFIG_BOOLEAN,
        0, 0, 0, 1, "false", NULL, "true,false", "tls_ca,tls_cert,tls_key", NULL,
        "Require a trusted client certificate on the proxy listener."}
};

static const char *const config_constraints[] = {
    "At least one of allow, allow_private or allow_tls_sni is required.",
    "Choose token_file or unauthenticated_loopback=true, never both.",
    "listen and listen_unix are mutually exclusive; neither means 127.0.0.1:0.",
    "listen_unix always requires token_file; unix_peer applies only to listen_unix.",
    "Principal quotas require token_file authentication.",
    "audit_log, audit_key_file and audit_key_id are all present or all absent.",
    "In a TLS-enabled binary, tls_cert and tls_key are paired; require_client_cert also requires tls_ca."
};

const egress_cli_config_spec_t *egress_cli_config_specs(size_t *out_count) {
    if (out_count) *out_count = sizeof(config_specs) / sizeof(config_specs[0]);
    return config_specs;
}

const egress_cli_config_spec_t *egress_cli_config_find(const char *name) {
    size_t count = 0u;
    const egress_cli_config_spec_t *values = egress_cli_config_specs(&count);
    for (size_t i = 0u; i < count; ++i) {
        if (strcmp(values[i].name, name) == 0) return &values[i];
    }
    return NULL;
}

const char *const *egress_cli_config_constraints(size_t *out_count) {
    if (out_count) {
        *out_count = sizeof(config_constraints) / sizeof(config_constraints[0]);
    }
    return config_constraints;
}

const char *egress_cli_config_type_name(egress_cli_config_type_t type) {
    switch (type) {
        case EGRESS_CLI_CONFIG_NONE: return "flag";
        case EGRESS_CLI_CONFIG_PATH: return "path";
        case EGRESS_CLI_CONFIG_STRING: return "string";
        case EGRESS_CLI_CONFIG_BOOLEAN: return "boolean";
        case EGRESS_CLI_CONFIG_ENDPOINT: return "endpoint";
        case EGRESS_CLI_CONFIG_DESTINATION: return "destination";
        case EGRESS_CLI_CONFIG_INTEGER: return "integer";
        case EGRESS_CLI_CONFIG_ENUM: return "enum";
    }
    return "unknown";
}

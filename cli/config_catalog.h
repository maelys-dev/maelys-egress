#ifndef MAELYS_EGRESS_CLI_CONFIG_CATALOG_H
#define MAELYS_EGRESS_CLI_CONFIG_CATALOG_H

#include <stddef.h>

/*
 * Executable catalog of the strict configuration file. Commands, options
 * and operands are declared with the maelys-cli macros in cli/main.c; this
 * catalog covers the configuration keys that `config describe` publishes
 * and that cli/config_file.c applies to egress_cli_settings_t.
 */

typedef enum egress_cli_config_type {
    EGRESS_CLI_CONFIG_NONE = 0,
    EGRESS_CLI_CONFIG_PATH,
    EGRESS_CLI_CONFIG_STRING,
    EGRESS_CLI_CONFIG_BOOLEAN,
    EGRESS_CLI_CONFIG_ENDPOINT,
    EGRESS_CLI_CONFIG_DESTINATION,
    EGRESS_CLI_CONFIG_INTEGER,
    EGRESS_CLI_CONFIG_ENUM
} egress_cli_config_type_t;

/* One value per key; the loader switches on it to fill the settings. */
typedef enum egress_cli_config_key {
    EGRESS_CLI_KEY_SCHEMA_VERSION = 0,
    EGRESS_CLI_KEY_LISTEN,
    EGRESS_CLI_KEY_LISTEN_UNIX,
    EGRESS_CLI_KEY_UNIX_PEER,
    EGRESS_CLI_KEY_ALLOW,
    EGRESS_CLI_KEY_ALLOW_PRIVATE,
    EGRESS_CLI_KEY_ALLOW_TLS_SNI,
    EGRESS_CLI_KEY_TOKEN_FILE,
    EGRESS_CLI_KEY_UNAUTHENTICATED_LOOPBACK,
    EGRESS_CLI_KEY_MAX_CONNECTIONS,
    EGRESS_CLI_KEY_QUOTA_CONNECTIONS,
    EGRESS_CLI_KEY_QUOTA_BYTES,
    EGRESS_CLI_KEY_QUOTA_TOTAL_BYTES,
    EGRESS_CLI_KEY_ADMIN_LISTEN,
    EGRESS_CLI_KEY_AUDIT_LOG,
    EGRESS_CLI_KEY_AUDIT_KEY_FILE,
    EGRESS_CLI_KEY_AUDIT_KEY_ID,
    EGRESS_CLI_KEY_TLS_CERT,
    EGRESS_CLI_KEY_TLS_KEY,
    EGRESS_CLI_KEY_TLS_CA,
    EGRESS_CLI_KEY_REQUIRE_CLIENT_CERT,
    EGRESS_CLI_KEY_COUNT
} egress_cli_config_key_t;

typedef struct egress_cli_config_spec {
    const char *name;
    egress_cli_config_key_t key;
    egress_cli_config_type_t type;
    int required;
    int repeatable;
    int secret;
    int tls_only;
    const char *default_value;
    const char *range;
    const char *allowed_values;
    const char *requires;
    const char *conflicts;
    const char *description;
} egress_cli_config_spec_t;

const egress_cli_config_spec_t *egress_cli_config_specs(size_t *out_count);
const egress_cli_config_spec_t *egress_cli_config_find(const char *name);
const char *const *egress_cli_config_constraints(size_t *out_count);
const char *egress_cli_config_type_name(egress_cli_config_type_t type);

#endif

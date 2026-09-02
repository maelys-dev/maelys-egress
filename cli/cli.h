#ifndef MAELYS_EGRESS_CLI_H
#define MAELYS_EGRESS_CLI_H

/*
 * Private contract of the maelys-egress command-line binary, built on
 * libmaelys_cli (agent-cli/v2). The public commands, options and exit codes
 * are declared in cli/main.c with the framework macros; the configuration
 * keys are catalogued in cli/config_catalog.c. This header only wires the
 * implementation files together:
 *
 *   main.c            application identity and command catalog
 *   commands.c        handlers: config describe, config validate, serve
 *   config_catalog.c  executable catalog of configuration keys
 *   config_file.c     strict key = value loader producing typed settings
 *   secrets.c         owner-only token and audit-key files
 *   serve.c           typed settings to sealed policy and running daemon
 *   reload.c          SIGHUP policy reload thread
 *   output.c          the only writer of the lifecycle JSON Lines stream
 *   tls_listener.c    the only file that knows which TLS module, if any, is
 *                     linked into this binary
 */

#include "maelys/egress.h"
#include "maelys/egress_tls_modules.h"
#include "cli/config_catalog.h"

#include <maelys/cli.h>

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define EGRESS_CLI_MAX_DESTINATIONS 256u

typedef struct egress_cli_destination {
    char host[254];
    uint16_t port;
    int allow_private;
    int require_tls_sni;
} egress_cli_destination_t;

/* The complete, validated content of one configuration file. Strings are
 * owned; NULL means the key was absent. Every cross-key constraint of the
 * catalog holds once egress_cli_settings_load() returned 0. */
typedef struct egress_cli_settings {
    char listen_host[64];
    uint16_t listen_port;
    int listen_set;
    char *listen_unix;
    maelys_egress_unix_peer_policy_t unix_peer;
    int unix_peer_set;
    egress_cli_destination_t *destinations;
    size_t destination_count;
    char *token_file;
    int unauthenticated_loopback;
    size_t max_connections;
    size_t quota_connections;
    uint64_t quota_bytes;
    uint64_t quota_total_bytes;
    char admin_host[64];
    uint16_t admin_port;
    int admin_set;
    char *audit_log;
    char *audit_key_file;
    char *audit_key_id;
    char *tls_cert;
    char *tls_key;
    char *tls_ca;
    int require_client_cert;
} egress_cli_settings_t;

typedef struct signal_context {
    maelys_egress_server_t *server;
    sigset_t signals;
    const char *config_path;
    const egress_cli_settings_t *baseline;
} signal_context_t;

/* config_file.c. Loading returns 0, or -1 with a framework error whose code
 * is NOT_FOUND, ACCESS_DENIED or IO_FAILED for the file itself, and
 * VALIDATION_FAILED or UNSUPPORTED for its content. */
int egress_cli_settings_load(
    const char *path, egress_cli_settings_t *out_settings, maelys_cli_error_t *error);
void egress_cli_settings_destroy(egress_cli_settings_t *settings);
/* 1 when everything except the destinations is identical: the reload
 * boundary of the control plane. */
int egress_cli_settings_control_equal(
    const egress_cli_settings_t *left, const egress_cli_settings_t *right);

/* secrets.c */
void egress_cli_secure_zero(void *data, size_t length);
int egress_cli_read_secret(const char *path, char secret[256]);
int egress_cli_read_audit_key(
    const char *path, unsigned char **out_key, size_t *out_length);

/* output.c: every byte of the maelys-egress-lifecycle/1 stream. */
void egress_cli_lifecycle_message(const char *event, const char *message);
void egress_cli_lifecycle_ready(
    const char *unix_path, const char *tcp_host, uint16_t tcp_port,
    const char *admin_host, uint16_t admin_port, const char *policy_digest);
void egress_cli_lifecycle_policy_reloaded(uint64_t generation, const char *digest);
void egress_cli_receipt_sink(void *context, const maelys_egress_receipt_t *receipt);

/* reload.c */
void *egress_cli_signal_main(void *opaque);

/* tls_listener.c */
int egress_cli_tls_listener_supported(void);
maelys_egress_result_t egress_cli_tls_listener_create(
    const maelys_egress_tls_files_t *files,
    maelys_egress_tls_provider_t **out_provider,
    char **out_error);

/* serve.c */
maelys_egress_result_t egress_cli_build_policy(
    const egress_cli_settings_t *settings, maelys_egress_policy_t **out_policy,
    char **out_error);
/* Returns 0 when validation succeeded (out_digest filled) or the daemon
 * stopped cleanly, 1 when the daemon reported a fatal lifecycle event, and
 * -1 with a framework error when nothing was started. reload_config_path
 * enables SIGHUP reload; check_only stops after sealing the policy. */
int egress_cli_run(
    const egress_cli_settings_t *settings, const char *reload_config_path,
    int check_only, maelys_cli_error_t *out_error, char out_digest[65]);

/* commands.c */
int egress_cli_command_config_describe(maelys_cli_context_t *context);
int egress_cli_command_config_validate(maelys_cli_context_t *context);
int egress_cli_command_serve(maelys_cli_context_t *context);

#endif

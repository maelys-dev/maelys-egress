#include "cli/cli.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* `config validate` and `serve`: turn typed settings into a sealed policy
 * and a server configuration, then run the daemon behind the lifecycle
 * stream owned by output.c. */

static int failure(
    maelys_cli_error_t *out_error, const char *code, const char *hint,
    const char *message) {
    maelys_cli_error_set(out_error, code, hint, "%s", message);
    return -1;
}

/* Maps a library result to the stable agent-cli/v2 error code. */
static const char *result_code(maelys_egress_result_t result) {
    switch (result) {
        case MAELYS_EGRESS_ERR_ARGUMENT: return MAELYS_CLI_CODE_VALIDATION_FAILED;
        case MAELYS_EGRESS_ERR_DENIED:
        case MAELYS_EGRESS_ERR_CRYPTO: return MAELYS_CLI_CODE_ACCESS_DENIED;
        case MAELYS_EGRESS_ERR_IO:
        case MAELYS_EGRESS_ERR_TIMEOUT: return MAELYS_CLI_CODE_IO_FAILED;
        case MAELYS_EGRESS_ERR_STATE:
        case MAELYS_EGRESS_ERR_CANCELLED: return MAELYS_CLI_CODE_PRECONDITION_FAILED;
        case MAELYS_EGRESS_ERR_UNSUPPORTED: return MAELYS_CLI_CODE_UNSUPPORTED;
        default: return MAELYS_CLI_CODE_UNEXPECTED;
    }
}

maelys_egress_result_t egress_cli_build_policy(
    const egress_cli_settings_t *settings, maelys_egress_policy_t **out_policy,
    char **out_error) {
    if (out_policy) *out_policy = NULL;
    if (!settings || !out_policy) return MAELYS_EGRESS_ERR_ARGUMENT;
    maelys_egress_policy_t *policy = NULL;
    maelys_egress_result_t result = maelys_egress_policy_create(&policy, out_error);
    for (size_t i = 0; result == MAELYS_EGRESS_OK && i < settings->destination_count; ++i) {
        const egress_cli_destination_t *destination = &settings->destinations[i];
        result = maelys_egress_policy_allow_tcp(policy, destination->host,
            destination->port, destination->allow_private, out_error);
        if (result == MAELYS_EGRESS_OK && destination->require_tls_sni) {
            result = maelys_egress_policy_require_tls_sni(policy,
                destination->host, destination->port, out_error);
        }
    }
    if (result == MAELYS_EGRESS_OK && settings->destination_count == 0u) {
        result = MAELYS_EGRESS_ERR_ARGUMENT;
    }
    if (result == MAELYS_EGRESS_OK) result = maelys_egress_policy_seal(policy, out_error);
    if (result != MAELYS_EGRESS_OK) {
        maelys_egress_policy_destroy(policy);
        return result;
    }
    *out_policy = policy;
    return MAELYS_EGRESS_OK;
}

static void set_text_error(char **out_error, const char *message) {
    if (!out_error || *out_error) return;
    char *copy = strdup(message);
    if (copy) *out_error = copy;
}

int egress_cli_run(
    const egress_cli_settings_t *settings, const char *reload_config_path,
    int check_only, maelys_cli_error_t *out_error, char out_digest[65]) {
    char secret[256] = {0};
    if (settings->token_file && !egress_cli_read_secret(settings->token_file, secret)) {
        char message[384];
        (void)snprintf(message, sizeof(message), "cannot read a valid token_file: %s",
                       strerror(errno));
        return failure(out_error, MAELYS_CLI_CODE_ACCESS_DENIED,
            "token_file must be an owner-only regular file of at least 16 bytes.",
            message);
    }
    char *error = NULL;
    maelys_egress_policy_t *policy = NULL;
    maelys_egress_config_t *config = NULL;
    maelys_egress_server_t *server = NULL;
    maelys_egress_audit_t *audit = NULL;
    maelys_egress_tls_provider_t *tls_provider = NULL;
    maelys_egress_result_t result = egress_cli_build_policy(settings, &policy, &error);
    const char *failure_code = result != MAELYS_EGRESS_OK ?
        MAELYS_CLI_CODE_POLICY_FAILED : NULL;
    if (result == MAELYS_EGRESS_OK) result = maelys_egress_config_create(&config, &error);
    if (result == MAELYS_EGRESS_OK) {
        result = settings->listen_unix ? maelys_egress_config_set_listen_unix(
            config, settings->listen_unix, strlen(settings->listen_unix),
            settings->unix_peer, &error) :
            maelys_egress_config_set_listen(config, settings->listen_host,
                settings->listen_port, &error);
    }
    if (result == MAELYS_EGRESS_OK && settings->tls_cert) {
        const maelys_egress_tls_files_t tls_files = {
            .certificate_file = settings->tls_cert,
            .private_key_file = settings->tls_key,
            .ca_file = settings->tls_ca,
            .require_client_certificate = settings->require_client_cert
        };
        result = egress_cli_tls_listener_create(&tls_files, &tls_provider, &error);
        if (result == MAELYS_EGRESS_OK) result = maelys_egress_config_set_tls_listener(
            config, tls_provider, &error);
        maelys_egress_tls_provider_release(tls_provider);
    }
    if (result == MAELYS_EGRESS_OK && settings->token_file) result =
        maelys_egress_config_set_authentication(config, "maelys", secret, &error);
    if (result == MAELYS_EGRESS_OK && settings->token_file) result =
        maelys_egress_config_set_principal_quota_v2(config, "maelys",
            settings->quota_connections, settings->quota_bytes,
            settings->quota_total_bytes, &error);
    if (result == MAELYS_EGRESS_OK && settings->unauthenticated_loopback) result =
        maelys_egress_config_allow_unauthenticated_loopback(config, 1, &error);
    if (result == MAELYS_EGRESS_OK) result = maelys_egress_config_set_limits(
        config, settings->max_connections, 32768u, 10000u, 60000u, &error);
    if (result == MAELYS_EGRESS_OK && settings->admin_set) result =
        maelys_egress_config_set_admin_listen(
            config, settings->admin_host, settings->admin_port, &error);
    unsigned char *audit_key = NULL;
    size_t audit_key_length = 0u;
    if (result == MAELYS_EGRESS_OK && settings->audit_log) {
        if (!egress_cli_read_audit_key(settings->audit_key_file, &audit_key,
                                       &audit_key_length)) {
            char message[384];
            (void)snprintf(message, sizeof(message),
                "cannot read owner-only audit_key_file: %s", strerror(errno));
            set_text_error(&error, message);
            result = MAELYS_EGRESS_ERR_DENIED;
        } else if (!check_only) {
            result = maelys_egress_audit_file_create(settings->audit_log, audit_key,
                audit_key_length, settings->audit_key_id, &audit, &error);
            if (result == MAELYS_EGRESS_OK) {
                result = maelys_egress_config_set_audit(config, audit, &error);
            }
        }
    }
    if (audit_key) { egress_cli_secure_zero(audit_key, audit_key_length); free(audit_key); }
    maelys_egress_audit_release(audit);
    if (config) maelys_egress_config_set_receipt_sink(config, egress_cli_receipt_sink, NULL);
    if (check_only && result == MAELYS_EGRESS_OK) {
        if (out_digest) {
            (void)snprintf(out_digest, 65u, "%s", maelys_egress_policy_digest_hex(policy));
        }
        egress_cli_secure_zero(secret, sizeof(secret));
        maelys_egress_config_destroy(config);
        maelys_egress_policy_destroy(policy);
        return 0;
    }
    if (result == MAELYS_EGRESS_OK) result = maelys_egress_server_create(
        policy, config, &server, &error);
    egress_cli_secure_zero(secret, sizeof(secret));
    if (result != MAELYS_EGRESS_OK) {
        (void)failure(out_error, failure_code ? failure_code : result_code(result),
            check_only ? "Fix the configuration and validate again." :
            "Inspect the configuration, referenced files and listener availability.",
            error ? error : maelys_egress_result_string(result));
        maelys_egress_error_free(error);
        maelys_egress_server_destroy(server);
        maelys_egress_config_destroy(config);
        maelys_egress_policy_destroy(policy);
        return -1;
    }
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGHUP);
    (void)pthread_sigmask(SIG_BLOCK, &signals, NULL);
    signal_context_t signal_context = {
        .server = server,
        .signals = signals,
        .config_path = reload_config_path,
        .baseline = settings
    };
    pthread_t signal_thread;
    if (pthread_create(&signal_thread, NULL, egress_cli_signal_main, &signal_context) != 0) {
        (void)failure(out_error, MAELYS_CLI_CODE_UNEXPECTED, NULL,
                      "cannot create signal waiter");
        maelys_egress_server_destroy(server);
        maelys_egress_config_destroy(config);
        maelys_egress_policy_destroy(policy);
        return -1;
    }
    egress_cli_lifecycle_ready(settings->listen_unix, settings->listen_host,
        maelys_egress_server_port(server), settings->admin_host,
        maelys_egress_server_admin_port(server),
        maelys_egress_policy_digest_hex(policy));
    result = maelys_egress_server_run(server, &error);
    (void)pthread_kill(signal_thread, SIGTERM);
    (void)pthread_join(signal_thread, NULL);
    if (result != MAELYS_EGRESS_OK) {
        egress_cli_lifecycle_message("fatal",
            error ? error : maelys_egress_result_string(result));
    } else {
        egress_cli_lifecycle_message("stopping", "shutdown requested");
    }
    maelys_egress_error_free(error);
    maelys_egress_server_destroy(server);
    maelys_egress_config_destroy(config);
    maelys_egress_policy_destroy(policy);
    if (result == MAELYS_EGRESS_OK) {
        egress_cli_lifecycle_message("stopped", "shutdown complete");
    }
    return result == MAELYS_EGRESS_OK ? 0 : 1;
}

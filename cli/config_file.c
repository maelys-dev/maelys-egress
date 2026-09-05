#include "cli/cli.h"

#include <errno.h>

#ifndef EFTYPE
#define EFTYPE EINVAL
#endif
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Strict configuration file: trusted-file checks, the key = value grammar
 * against the compiled catalog, typed values applied straight into
 * egress_cli_settings_t, then the cross-key constraints of the catalog. */

#define CONFIG_HINT \
    "Fix the reported line, then run 'maelys-egress config describe' for the " \
    "accepted keys."

static const char *const unix_peer_choices[] = {"authenticated", "same-euid", NULL};

void egress_cli_settings_destroy(egress_cli_settings_t *settings) {
    if (!settings) return;
    free(settings->listen_unix);
    free(settings->destinations);
    free(settings->token_file);
    free(settings->audit_log);
    free(settings->audit_key_file);
    free(settings->audit_key_id);
    free(settings->tls_cert);
    free(settings->tls_key);
    free(settings->tls_ca);
    memset(settings, 0, sizeof(*settings));
}

static int same_text(const char *left, const char *right) {
    if (!left || !right) return left == right;
    return strcmp(left, right) == 0;
}

int egress_cli_settings_control_equal(
    const egress_cli_settings_t *a, const egress_cli_settings_t *b) {
    return strcmp(a->listen_host, b->listen_host) == 0 &&
        a->listen_port == b->listen_port && a->listen_set == b->listen_set &&
        same_text(a->listen_unix, b->listen_unix) &&
        a->unix_peer == b->unix_peer && a->unix_peer_set == b->unix_peer_set &&
        same_text(a->token_file, b->token_file) &&
        a->unauthenticated_loopback == b->unauthenticated_loopback &&
        a->max_connections == b->max_connections &&
        a->quota_connections == b->quota_connections &&
        a->quota_bytes == b->quota_bytes &&
        a->quota_total_bytes == b->quota_total_bytes &&
        strcmp(a->admin_host, b->admin_host) == 0 &&
        a->admin_port == b->admin_port && a->admin_set == b->admin_set &&
        same_text(a->audit_log, b->audit_log) &&
        same_text(a->audit_key_file, b->audit_key_file) &&
        same_text(a->audit_key_id, b->audit_key_id) &&
        same_text(a->tls_cert, b->tls_cert) && same_text(a->tls_key, b->tls_key) &&
        same_text(a->tls_ca, b->tls_ca) &&
        a->require_client_cert == b->require_client_cert;
}

static char *trim(char *value) {
    while (*value == ' ' || *value == '\t') ++value;
    size_t length = strlen(value);
    while (length && (value[length - 1u] == ' ' || value[length - 1u] == '\t')) {
        value[--length] = '\0';
    }
    return value;
}

static int parse_endpoint(
    const char *input, char *host, size_t host_capacity, uint16_t *out_port) {
    if (!input || !host || !out_port) return 0;
    const char *begin = input;
    const char *port_text = NULL;
    size_t host_length;
    if (*begin == '[') {
        const char *end = strchr(begin, ']');
        if (!end || end[1] != ':') return 0;
        host_length = (size_t)(end - begin - 1);
        begin++;
        port_text = end + 2;
    } else {
        const char *colon = strrchr(begin, ':');
        if (!colon || strchr(begin, ':') != colon) return 0;
        host_length = (size_t)(colon - begin);
        port_text = colon + 1;
    }
    if (host_length == 0u || host_length >= host_capacity || !*port_text) return 0;
    unsigned long port = 0u;
    for (const unsigned char *p = (const unsigned char *)port_text; *p; ++p) {
        if (*p < '0' || *p > '9') return 0;
        port = port * 10u + (unsigned long)(*p - '0');
        if (port > 65535u) return 0;
    }
    memcpy(host, begin, host_length);
    host[host_length] = '\0';
    *out_port = (uint16_t)port;
    return 1;
}

static int own_text(char **slot, const char *value) {
    char *copy = strdup(value);
    if (!copy) return 0;
    free(*slot);
    *slot = copy;
    return 1;
}

/* Applies one validated line. Returns NULL, or the violation to report. */
static const char *apply_value(
    egress_cli_settings_t *settings, const egress_cli_config_spec_t *spec,
    const char *value, int *out_memory) {
    uint64_t number = 0u;
    switch (spec->key) {
        case EGRESS_CLI_KEY_SCHEMA_VERSION:
            return strcmp(value, "1") == 0 ? NULL : "schema_version must be 1";
        case EGRESS_CLI_KEY_LISTEN:
            if (!parse_endpoint(value, settings->listen_host,
                                sizeof(settings->listen_host), &settings->listen_port))
                return "invalid listen endpoint";
            settings->listen_set = 1;
            return NULL;
        case EGRESS_CLI_KEY_LISTEN_UNIX:
            *out_memory = !own_text(&settings->listen_unix, value);
            return NULL;
        case EGRESS_CLI_KEY_UNIX_PEER: {
            size_t index = 0u;
            if (maelys_cli_parse_choice(value, unix_peer_choices, &index) != 0)
                return "unix_peer must be authenticated or same-euid";
            settings->unix_peer = index == 0u ? MAELYS_EGRESS_UNIX_PEER_AUTHENTICATED :
                MAELYS_EGRESS_UNIX_PEER_SAME_EUID;
            settings->unix_peer_set = 1;
            return NULL;
        }
        case EGRESS_CLI_KEY_ALLOW:
        case EGRESS_CLI_KEY_ALLOW_PRIVATE:
        case EGRESS_CLI_KEY_ALLOW_TLS_SNI: {
            if (settings->destination_count >= EGRESS_CLI_MAX_DESTINATIONS)
                return "too many allowed destinations";
            egress_cli_destination_t *destination =
                &settings->destinations[settings->destination_count];
            memset(destination, 0, sizeof(*destination));
            if (!parse_endpoint(value, destination->host, sizeof(destination->host),
                                &destination->port) || destination->port == 0u)
                return "invalid allowed destination";
            destination->allow_private = spec->key == EGRESS_CLI_KEY_ALLOW_PRIVATE;
            destination->require_tls_sni = spec->key == EGRESS_CLI_KEY_ALLOW_TLS_SNI;
            ++settings->destination_count;
            return NULL;
        }
        case EGRESS_CLI_KEY_TOKEN_FILE:
            *out_memory = !own_text(&settings->token_file, value);
            return NULL;
        case EGRESS_CLI_KEY_UNAUTHENTICATED_LOOPBACK:
        case EGRESS_CLI_KEY_REQUIRE_CLIENT_CERT: {
            int flag = 0;
            if (strcmp(value, "true") == 0) flag = 1;
            else if (strcmp(value, "false") != 0) return "boolean must be true or false";
            if (spec->key == EGRESS_CLI_KEY_UNAUTHENTICATED_LOOPBACK)
                settings->unauthenticated_loopback = flag;
            else settings->require_client_cert = flag;
            return NULL;
        }
        case EGRESS_CLI_KEY_MAX_CONNECTIONS:
            if (maelys_cli_parse_u64_decimal(value, 1u, 4096u, &number) != 0)
                return "max_connections must be in 1..4096";
            settings->max_connections = (size_t)number;
            return NULL;
        case EGRESS_CLI_KEY_QUOTA_CONNECTIONS:
            if (maelys_cli_parse_u64_decimal(value, 0u, 4096u, &number) != 0)
                return "quota_connections must be in 0..4096";
            settings->quota_connections = (size_t)number;
            return NULL;
        case EGRESS_CLI_KEY_QUOTA_BYTES:
            if (maelys_cli_parse_u64_decimal(value, 0u, UINT64_MAX, &number) != 0)
                return "quota_bytes must be an unsigned integer";
            settings->quota_bytes = number;
            return NULL;
        case EGRESS_CLI_KEY_QUOTA_TOTAL_BYTES:
            if (maelys_cli_parse_u64_decimal(value, 0u, UINT64_MAX, &number) != 0)
                return "quota_total_bytes must be an unsigned integer";
            settings->quota_total_bytes = number;
            return NULL;
        case EGRESS_CLI_KEY_ADMIN_LISTEN:
            if (!parse_endpoint(value, settings->admin_host,
                                sizeof(settings->admin_host), &settings->admin_port))
                return "invalid admin_listen endpoint";
            settings->admin_set = 1;
            return NULL;
        case EGRESS_CLI_KEY_AUDIT_LOG:
            *out_memory = !own_text(&settings->audit_log, value);
            return NULL;
        case EGRESS_CLI_KEY_AUDIT_KEY_FILE:
            *out_memory = !own_text(&settings->audit_key_file, value);
            return NULL;
        case EGRESS_CLI_KEY_AUDIT_KEY_ID:
            *out_memory = !own_text(&settings->audit_key_id, value);
            return NULL;
        case EGRESS_CLI_KEY_TLS_CERT:
            *out_memory = !own_text(&settings->tls_cert, value);
            return NULL;
        case EGRESS_CLI_KEY_TLS_KEY:
            *out_memory = !own_text(&settings->tls_key, value);
            return NULL;
        case EGRESS_CLI_KEY_TLS_CA:
            *out_memory = !own_text(&settings->tls_ca, value);
            return NULL;
        case EGRESS_CLI_KEY_COUNT:
            break;
    }
    return "configuration catalog names an unknown key";
}

/* The cross-key constraints published by `config describe`. */
static const char *check_constraints(const egress_cli_settings_t *s) {
    if ((s->listen_unix && s->listen_set) || (s->unix_peer_set && !s->listen_unix))
        return "choose exactly one listener; unix_peer requires listen_unix";
    if (s->destination_count == 0u || (!s->token_file && !s->unauthenticated_loopback))
        return "at least one destination and an explicit authentication mode are required";
    if (s->token_file && s->unauthenticated_loopback)
        return "choose token_file or unauthenticated_loopback, not both";
    if (s->listen_unix && s->unauthenticated_loopback)
        return "Unix listeners require token_file authentication";
    if (s->unauthenticated_loopback &&
        (s->quota_connections || s->quota_bytes || s->quota_total_bytes))
        return "principal quotas require token_file authentication";
    int audit_parts = !!s->audit_log + !!s->audit_key_file + !!s->audit_key_id;
    if (audit_parts != 0 && audit_parts != 3)
        return "audit_log, audit_key_file and audit_key_id are all required together";
    if (!!s->tls_cert != !!s->tls_key || (s->require_client_cert && !s->tls_ca))
        return "TLS requires tls_cert and tls_key; mutual TLS also requires tls_ca";
    return NULL;
}

/* A refusal of the file's identity (link, type, owner, mode, links) stays an
 * ACCESS_DENIED for this command, as its contract documents; the framework
 * classifies the other errno values. */
static int file_failure(
    maelys_cli_error_t *error, const char *path, int saved, const char *explanation) {
    int identity = saved == EPERM || saved == ELOOP || saved == EMLINK ||
        saved == EFTYPE || saved == EINVAL || saved == EISDIR;
    maelys_cli_error_set(error,
        identity ? MAELYS_CLI_CODE_ACCESS_DENIED : maelys_cli_file_error_code(saved),
        identity ? "Make the file a regular, single-link file owned by root or the "
                   "daemon user, without group or world write permission, and not "
                   "a symbolic link." :
                   "Name an existing configuration file readable by the daemon.",
        "%s: %s", path, explanation ? explanation : strerror(saved));
    errno = saved;
    return -1;
}

int egress_cli_settings_load(
    const char *path, egress_cli_settings_t *settings, maelys_cli_error_t *error) {
    memset(settings, 0, sizeof(*settings));
    memset(error, 0, sizeof(*error));
    (void)snprintf(settings->listen_host, sizeof(settings->listen_host), "127.0.0.1");
    settings->unix_peer = MAELYS_EGRESS_UNIX_PEER_AUTHENTICATED;
    settings->max_connections = 128u;
    /* The framework judges the descriptor it opens: regular, not a symbolic
     * link, one hard link, owned by root or the daemon user, not writable by
     * group or world. */
    int fd = -1;
    const char *explanation = NULL;
    if (maelys_cli_open_trusted(path,
            MAELYS_CLI_FILE_REGULAR | MAELYS_CLI_FILE_NO_SYMLINK |
            MAELYS_CLI_FILE_SINGLE_LINK | MAELYS_CLI_FILE_OWNER_TRUSTED |
            MAELYS_CLI_FILE_NOT_WRITABLE_BY_OTHERS,
            &fd, &explanation) != 0) {
        return file_failure(error, path, errno, explanation);
    }
    FILE *stream = fdopen(fd, "r");
    if (!stream) {
        int saved = errno;
        (void)close(fd);
        maelys_cli_error_from_errno(error, MAELYS_CLI_CODE_IO_FAILED, saved, path);
        return -1;
    }
    settings->destinations = calloc(EGRESS_CLI_MAX_DESTINATIONS,
                                    sizeof(*settings->destinations));
    int ok = settings->destinations != NULL;
    if (!ok) maelys_cli_error_from_errno(error, MAELYS_CLI_CODE_UNEXPECTED, ENOMEM, path);
    unsigned int seen = 0u;
    char *line = NULL;
    size_t capacity = 0u;
    unsigned long line_number = 0u;
    while (ok) {
        errno = 0;
        ssize_t amount = getline(&line, &capacity, stream);
        if (amount < 0) {
            if (feof(stream)) break;
            maelys_cli_error_from_errno(error, MAELYS_CLI_CODE_IO_FAILED,
                                        errno ? errno : EIO, path);
            ok = 0;
            break;
        }
        ++line_number;
        const char *violation = NULL;
        const char *subject = NULL;
        if (amount > 4096 || memchr(line, '\0', (size_t)amount) != NULL) {
            violation = "line is too long or contains NUL";
        } else {
            while (amount > 0 && (line[amount - 1] == '\n' || line[amount - 1] == '\r')) {
                line[--amount] = '\0';
            }
            char *entry = trim(line);
            if (!*entry || *entry == '#') continue;
            char *equals = strchr(entry, '=');
            if (!equals) {
                violation = "expected key = value";
            } else {
                *equals = '\0';
                char *name = trim(entry);
                char *value = trim(equals + 1);
                const egress_cli_config_spec_t *spec = egress_cli_config_find(name);
                unsigned int bit = spec ? 1u << spec->key : 0u;
                if (!spec || !*value) {
                    violation = "unknown key or empty value";
                    subject = name;
                } else if (!spec->repeatable && (seen & bit)) {
                    violation = "duplicate key";
                    subject = name;
                } else if (spec->tls_only && !egress_cli_tls_listener_supported()) {
                    maelys_cli_error_set(error, MAELYS_CLI_CODE_UNSUPPORTED,
                        "Use maelys-egress-mbedtls or maelys-egress-wolfssl, or remove the key.",
                        "%s:%lu: %s requires a TLS-enabled binary", path, line_number, name);
                    ok = 0;
                    break;
                } else {
                    seen |= bit;
                    int memory = 0;
                    violation = apply_value(settings, spec, value, &memory);
                    if (memory) {
                        maelys_cli_error_from_errno(error, MAELYS_CLI_CODE_UNEXPECTED,
                                                    ENOMEM, path);
                        ok = 0;
                        break;
                    }
                }
            }
        }
        if (violation) {
            if (subject) {
                maelys_cli_error_set(error, MAELYS_CLI_CODE_VALIDATION_FAILED, CONFIG_HINT,
                    "%s:%lu: %s: %s", path, line_number, violation, subject);
            } else {
                maelys_cli_error_set(error, MAELYS_CLI_CODE_VALIDATION_FAILED, CONFIG_HINT,
                    "%s:%lu: %s", path, line_number, violation);
            }
            ok = 0;
            break;
        }
    }
    free(line);
    if (fclose(stream) != 0) {
        if (ok) maelys_cli_error_from_errno(error, MAELYS_CLI_CODE_IO_FAILED, errno, path);
        ok = 0;
    }
    if (ok && !(seen & (1u << EGRESS_CLI_KEY_SCHEMA_VERSION))) {
        maelys_cli_error_set(error, MAELYS_CLI_CODE_VALIDATION_FAILED, CONFIG_HINT,
            "%s: missing required key: schema_version", path);
        ok = 0;
    }
    if (ok) {
        const char *violation = check_constraints(settings);
        if (violation) {
            maelys_cli_error_set(error, MAELYS_CLI_CODE_VALIDATION_FAILED,
                "Fix the configuration file, then run "
                "'maelys-egress config validate --config FILE'.",
                "%s: %s", path, violation);
            ok = 0;
        }
    }
    if (!ok) {
        egress_cli_settings_destroy(settings);
        return -1;
    }
    return 0;
}
